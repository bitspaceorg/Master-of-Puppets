/*
 * Master of Puppets — .mop v2 Scene File Format
 * mop_scene.c — Load/save multi-mesh scene files with quantized vertices
 *
 * File layout (all 8-byte aligned for mmap):
 *   [0..63]      MopSceneHeader
 *   [64..]       TOC entries (section_count * sizeof(MopSceneTocEntry))
 *   [aligned..]  Section data (each section 8-byte aligned)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/loader/loader.h>
#include <mop/loader/mop_scene.h>
#include <mop/query/camera_query.h>
#include <mop/query/scene_query.h>
#include <mop/util/log.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* POSIX mmap support */
#if defined(__unix__) || defined(__APPLE__)
#define MOP_HAS_MMAP 1
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* -------------------------------------------------------------------------
 * On-disk structures (packed, stable ABI)
 * ------------------------------------------------------------------------- */

#define MOP_SCENE_HEADER_SIZE 64
#define MOP_SCENE_ALIGN 8

typedef struct MopSceneHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t section_count;
  uint32_t flags;
  uint8_t _reserved[MOP_SCENE_HEADER_SIZE - 16];
} MopSceneHeader;

typedef struct MopSceneTocEntry {
  uint32_t type;   /* MopSectionType */
  uint32_t flags;  /* section-specific flags */
  uint64_t offset; /* byte offset from file start */
  uint64_t size;   /* byte size of section data */
} MopSceneTocEntry;

/* Mesh section header — precedes vertex and index data */
typedef struct MopMeshSectionHeader {
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t vertex_encoding; /* MOP_VTX_RAW or MOP_VTX_QUANTIZED */
  uint32_t index_size;      /* 2 = uint16, 4 = uint32 */
  float bbox_min[3];
  float bbox_max[3];
  uint8_t _pad[8]; /* align to 48 bytes total */
} MopMeshSectionHeader;

/* Camera section — stored camera parameters */
typedef struct MopCameraSection {
  float eye[3];
  float target[3];
  float up[3];
  float fov_radians;
  float near_plane;
  float far_plane;
  float aspect_ratio;
  uint8_t _pad[4];
} MopCameraSection;

/* Light section — matches MopLight layout */
typedef struct MopLightSection {
  uint32_t type;
  float position[3];
  float direction[3];
  float color[4];
  float intensity;
  float range;
  float spot_inner_cos;
  float spot_outer_cos;
  uint8_t _pad[4];
} MopLightSection;

/* -------------------------------------------------------------------------
 * In-memory scene handle
 * ------------------------------------------------------------------------- */

#define MOP_SCENE_MAX_SECTIONS 256

struct MopSceneFile {
  MopSceneHeader header;
  MopSceneTocEntry toc[MOP_SCENE_MAX_SECTIONS];
  uint8_t *data; /* points into mmap or malloc'd file */
  size_t data_size;
  bool is_mmapped;
  void *mmap_base;
  size_t mmap_size;
};

/* -------------------------------------------------------------------------
 * Alignment helper
 * ------------------------------------------------------------------------- */

static uint64_t align8(uint64_t v) { return (v + 7) & ~(uint64_t)7; }

/* -------------------------------------------------------------------------
 * Octahedral normal encoding/decoding
 *
 * Maps unit sphere -> unit square with ~14-bit precision.
 * Exact reconstruction for axis-aligned normals.
 * Reference: "A Survey of Efficient Representations for Independent Unit
 * Vectors" - Cigolle et al. 2014
 * ------------------------------------------------------------------------- */

static void oct_encode(float nx, float ny, float nz, int16_t *out) {
  float ax = fabsf(nx), ay = fabsf(ny), az = fabsf(nz);
  float sum = ax + ay + az;
  if (sum < 1e-8f)
    sum = 1e-8f;
  float ox = nx / sum;
  float oy = ny / sum;

  if (nz < 0.0f) {
    float tmpx = (1.0f - fabsf(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
    float tmpy = (1.0f - fabsf(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
    ox = tmpx;
    oy = tmpy;
  }

  /* Map [-1, 1] to int16 range */
  out[0] = (int16_t)(ox * 32767.0f);
  out[1] = (int16_t)(oy * 32767.0f);
}

static void oct_decode(int16_t ix, int16_t iy, float *nx, float *ny,
                       float *nz) {
  float ox = (float)ix / 32767.0f;
  float oy = (float)iy / 32767.0f;

  *nz = 1.0f - fabsf(ox) - fabsf(oy);
  if (*nz < 0.0f) {
    float tmpx = (1.0f - fabsf(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
    float tmpy = (1.0f - fabsf(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
    ox = tmpx;
    oy = tmpy;
  }
  *nx = ox;
  *ny = oy;

  float len = sqrtf((*nx) * (*nx) + (*ny) * (*ny) + (*nz) * (*nz));
  if (len > 1e-8f) {
    *nx /= len;
    *ny /= len;
    *nz /= len;
  }
}

/* -------------------------------------------------------------------------
 * Quantized vertex encode/decode
 * ------------------------------------------------------------------------- */

static MopQuantizedVertex quantize_vertex(const MopVertex *v,
                                          const float bbox_min[3],
                                          const float bbox_max[3]) {
  MopQuantizedVertex q;

  for (int i = 0; i < 3; i++) {
    float range = bbox_max[i] - bbox_min[i];
    float pos = (&v->position.x)[i];
    float t = (range > 1e-8f) ? (pos - bbox_min[i]) / range : 0.0f;
    if (t < 0.0f)
      t = 0.0f;
    if (t > 1.0f)
      t = 1.0f;
    q.pos[i] = (uint16_t)(t * 65535.0f + 0.5f);
  }

  oct_encode(v->normal.x, v->normal.y, v->normal.z, q.nrm);

  q.color[0] = (uint8_t)(v->color.r * 255.0f + 0.5f);
  q.color[1] = (uint8_t)(v->color.g * 255.0f + 0.5f);
  q.color[2] = (uint8_t)(v->color.b * 255.0f + 0.5f);
  q.color[3] = (uint8_t)(v->color.a * 255.0f + 0.5f);

  q.uv[0] = (uint16_t)(v->u * 65535.0f + 0.5f);
  q.uv[1] = (uint16_t)(v->v * 65535.0f + 0.5f);

  return q;
}

static MopVertex dequantize_vertex(const MopQuantizedVertex *q,
                                   const float bbox_min[3],
                                   const float bbox_max[3]) {
  MopVertex v;

  for (int i = 0; i < 3; i++) {
    float range = bbox_max[i] - bbox_min[i];
    (&v.position.x)[i] = bbox_min[i] + ((float)q->pos[i] / 65535.0f) * range;
  }

  oct_decode(q->nrm[0], q->nrm[1], &v.normal.x, &v.normal.y, &v.normal.z);

  v.color.r = (float)q->color[0] / 255.0f;
  v.color.g = (float)q->color[1] / 255.0f;
  v.color.b = (float)q->color[2] / 255.0f;
  v.color.a = (float)q->color[3] / 255.0f;

  v.u = (float)q->uv[0] / 65535.0f;
  v.v = (float)q->uv[1] / 65535.0f;

  return v;
}

/* -------------------------------------------------------------------------
 * Load
 * ------------------------------------------------------------------------- */

MopSceneFile *mop_scene_load(const char *path) {
  if (!path) {
    MOP_ERROR("mop_scene_load: NULL path");
    return NULL;
  }

  MopSceneFile *scene = calloc(1, sizeof(MopSceneFile));
  if (!scene)
    return NULL;

#if MOP_HAS_MMAP
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    MOP_ERROR("mop_scene_load: failed to open '%s'", path);
    free(scene);
    return NULL;
  }

  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < (off_t)MOP_SCENE_HEADER_SIZE) {
    close(fd);
    MOP_ERROR("mop_scene_load: file too small '%s'", path);
    free(scene);
    return NULL;
  }

  size_t file_size = (size_t)st.st_size;
  void *mapping = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);

  if (mapping == MAP_FAILED) {
    MOP_ERROR("mop_scene_load: mmap failed for '%s'", path);
    free(scene);
    return NULL;
  }

  scene->data = (uint8_t *)mapping;
  scene->data_size = file_size;
  scene->is_mmapped = true;
  scene->mmap_base = mapping;
  scene->mmap_size = file_size;
#else
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    MOP_ERROR("mop_scene_load: failed to open '%s'", path);
    free(scene);
    return NULL;
  }

  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if (file_size < (long)MOP_SCENE_HEADER_SIZE) {
    fclose(fp);
    MOP_ERROR("mop_scene_load: file too small '%s'", path);
    free(scene);
    return NULL;
  }

  scene->data = malloc((size_t)file_size);
  if (!scene->data) {
    fclose(fp);
    free(scene);
    return NULL;
  }

  if (fread(scene->data, (size_t)file_size, 1, fp) != 1) {
    fclose(fp);
    free(scene->data);
    free(scene);
    return NULL;
  }
  fclose(fp);
  scene->data_size = (size_t)file_size;
  scene->is_mmapped = false;
#endif

  /* Parse header */
  memcpy(&scene->header, scene->data, sizeof(MopSceneHeader));

  if (scene->header.magic != MOP_SCENE_MAGIC) {
    MOP_ERROR("mop_scene_load: bad magic in '%s'", path);
    mop_scene_free(scene);
    return NULL;
  }
  if (scene->header.version != MOP_SCENE_VERSION) {
    MOP_ERROR("mop_scene_load: unsupported version %u in '%s'",
              scene->header.version, path);
    mop_scene_free(scene);
    return NULL;
  }

  uint32_t nsec = scene->header.section_count;
  if (nsec > MOP_SCENE_MAX_SECTIONS) {
    MOP_ERROR("mop_scene_load: too many sections (%u) in '%s'", nsec, path);
    mop_scene_free(scene);
    return NULL;
  }

  /* Parse TOC */
  size_t toc_offset = MOP_SCENE_HEADER_SIZE;
  size_t toc_size = nsec * sizeof(MopSceneTocEntry);
  if (toc_offset + toc_size > scene->data_size) {
    MOP_ERROR("mop_scene_load: TOC exceeds file size in '%s'", path);
    mop_scene_free(scene);
    return NULL;
  }
  memcpy(scene->toc, scene->data + toc_offset, toc_size);

  MOP_INFO("mop_scene_load: loaded '%s' (%u sections)", path, nsec);
  return scene;
}

/* -------------------------------------------------------------------------
 * Free
 * ------------------------------------------------------------------------- */

void mop_scene_free(MopSceneFile *scene) {
  if (!scene)
    return;

  if (scene->is_mmapped) {
#if MOP_HAS_MMAP
    if (scene->mmap_base)
      munmap(scene->mmap_base, scene->mmap_size);
#endif
  } else {
    free(scene->data);
  }

  free(scene);
}

/* -------------------------------------------------------------------------
 * Query helpers
 * ------------------------------------------------------------------------- */

static const MopSceneTocEntry *find_section(const MopSceneFile *s,
                                            MopSectionType type, uint32_t nth) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < s->header.section_count; i++) {
    if ((MopSectionType)s->toc[i].type == type) {
      if (count == nth)
        return &s->toc[i];
      count++;
    }
  }
  return NULL;
}

static uint32_t count_sections(const MopSceneFile *s, MopSectionType type) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < s->header.section_count; i++) {
    if ((MopSectionType)s->toc[i].type == type)
      count++;
  }
  return count;
}

uint32_t mop_scene_mesh_count(const MopSceneFile *s) {
  if (!s)
    return 0;
  return count_sections(s, MOP_SECTION_MESH);
}

uint32_t mop_scene_light_count(const MopSceneFile *s) {
  if (!s)
    return 0;
  return count_sections(s, MOP_SECTION_LIGHT);
}

bool mop_scene_get_mesh(const MopSceneFile *s, uint32_t idx,
                        MopLoadedMesh *out) {
  if (!s || !out)
    return false;
  memset(out, 0, sizeof(*out));

  const MopSceneTocEntry *entry = find_section(s, MOP_SECTION_MESH, idx);
  if (!entry)
    return false;

  if (entry->offset + entry->size > s->data_size) {
    MOP_ERROR("mop_scene_get_mesh: section data out of bounds");
    return false;
  }

  const uint8_t *sec_data = s->data + entry->offset;
  if (entry->size < sizeof(MopMeshSectionHeader))
    return false;

  MopMeshSectionHeader mhdr;
  memcpy(&mhdr, sec_data, sizeof(mhdr));

  uint32_t vcount = mhdr.vertex_count;
  uint32_t icount = mhdr.index_count;

  /* Compute data sizes */
  size_t vtx_bytes;
  bool quantized = (mhdr.vertex_encoding == MOP_VTX_QUANTIZED);
  if (quantized) {
    vtx_bytes = (size_t)vcount * sizeof(MopQuantizedVertex);
  } else {
    vtx_bytes = (size_t)vcount * sizeof(MopVertex);
  }

  size_t idx_bytes;
  bool idx16 = (mhdr.index_size == 2);
  if (idx16) {
    idx_bytes = (size_t)icount * sizeof(uint16_t);
  } else {
    idx_bytes = (size_t)icount * sizeof(uint32_t);
  }

  const uint8_t *vtx_data = sec_data + sizeof(MopMeshSectionHeader);
  const uint8_t *idx_data = vtx_data + align8(vtx_bytes);

  /* Allocate and decode vertices */
  MopVertex *vertices = malloc(vcount * sizeof(MopVertex));
  if (!vertices)
    return false;

  if (quantized) {
    const MopQuantizedVertex *qverts = (const MopQuantizedVertex *)vtx_data;
    for (uint32_t i = 0; i < vcount; i++) {
      vertices[i] = dequantize_vertex(&qverts[i], mhdr.bbox_min, mhdr.bbox_max);
    }
  } else {
    memcpy(vertices, vtx_data, vtx_bytes);
  }

  /* Allocate and decode indices */
  uint32_t *indices = malloc(icount * sizeof(uint32_t));
  if (!indices) {
    free(vertices);
    return false;
  }

  if (idx16) {
    const uint16_t *idx16_data = (const uint16_t *)idx_data;
    for (uint32_t i = 0; i < icount; i++) {
      indices[i] = idx16_data[i];
    }
  } else {
    memcpy(indices, idx_data, idx_bytes);
  }

  out->vertices = vertices;
  out->vertex_count = vcount;
  out->indices = indices;
  out->index_count = icount;
  out->bbox_min =
      (MopVec3){mhdr.bbox_min[0], mhdr.bbox_min[1], mhdr.bbox_min[2]};
  out->bbox_max =
      (MopVec3){mhdr.bbox_max[0], mhdr.bbox_max[1], mhdr.bbox_max[2]};
  out->tangents = NULL;
  out->_format = MOP_FORMAT_MOP_BINARY;
  out->_mmapped = false;
  out->_mmap_base = NULL;
  out->_mmap_size = 0;

  return true;
}

bool mop_scene_get_camera(const MopSceneFile *s, MopVec3 *eye, MopVec3 *target,
                          MopVec3 *up, float *fov, float *near_p,
                          float *far_p) {
  if (!s)
    return false;

  const MopSceneTocEntry *entry = find_section(s, MOP_SECTION_CAMERA, 0);
  if (!entry)
    return false;

  if (entry->offset + sizeof(MopCameraSection) > s->data_size)
    return false;

  MopCameraSection cam;
  memcpy(&cam, s->data + entry->offset, sizeof(cam));

  if (eye)
    *eye = (MopVec3){cam.eye[0], cam.eye[1], cam.eye[2]};
  if (target)
    *target = (MopVec3){cam.target[0], cam.target[1], cam.target[2]};
  if (up)
    *up = (MopVec3){cam.up[0], cam.up[1], cam.up[2]};
  if (fov)
    *fov = cam.fov_radians;
  if (near_p)
    *near_p = cam.near_plane;
  if (far_p)
    *far_p = cam.far_plane;

  return true;
}

/* -------------------------------------------------------------------------
 * Save
 * ------------------------------------------------------------------------- */

/* Helper: write bytes to FILE, return bytes written */
static size_t fwrite_bytes(FILE *fp, const void *data, size_t size) {
  return fwrite(data, 1, size, fp);
}

/* Helper: write padding bytes for alignment */
static void fwrite_pad(FILE *fp, size_t current, size_t alignment) {
  size_t aligned = (current + alignment - 1) & ~(alignment - 1);
  size_t pad = aligned - current;
  if (pad > 0) {
    uint8_t zeros[8] = {0};
    while (pad > 0) {
      size_t chunk = pad > 8 ? 8 : pad;
      fwrite(zeros, 1, chunk, fp);
      pad -= chunk;
    }
  }
}

int mop_scene_save(const MopViewport *vp, const char *path, uint32_t flags) {
  if (!vp || !path) {
    MOP_ERROR("mop_scene_save: NULL viewport or path");
    return -1;
  }

  bool quantize = (flags & MOP_SAVE_QUANTIZE) != 0;

  /* Count sections: meshes + 1 camera + lights */
  uint32_t nmesh = mop_viewport_mesh_count(vp);
  uint32_t nlight = mop_viewport_light_count(vp);
  uint32_t nsec = nmesh + 1 + nlight; /* meshes + camera + lights */

  if (nsec > MOP_SCENE_MAX_SECTIONS) {
    MOP_ERROR("mop_scene_save: too many sections");
    return -1;
  }

  FILE *fp = fopen(path, "wb");
  if (!fp) {
    MOP_ERROR("mop_scene_save: failed to open '%s'", path);
    return -1;
  }

  /* Write header */
  MopSceneHeader header = {0};
  header.magic = MOP_SCENE_MAGIC;
  header.version = MOP_SCENE_VERSION;
  header.section_count = nsec;
  header.flags = flags;
  fwrite_bytes(fp, &header, sizeof(header));

  /* Reserve space for TOC (we'll seek back to fill it in) */
  size_t toc_start = MOP_SCENE_HEADER_SIZE;
  MopSceneTocEntry *toc = calloc(nsec, sizeof(MopSceneTocEntry));
  if (!toc) {
    fclose(fp);
    return -1;
  }
  fwrite_bytes(fp, toc, nsec * sizeof(MopSceneTocEntry));

  size_t current_offset = toc_start + nsec * sizeof(MopSceneTocEntry);
  fwrite_pad(fp, current_offset, MOP_SCENE_ALIGN);
  current_offset = align8(current_offset);

  uint32_t sec_idx = 0;

  /* Write camera section */
  {
    MopCameraState cs = mop_viewport_get_camera_state(vp);
    MopCameraSection cam = {0};
    cam.eye[0] = cs.eye.x;
    cam.eye[1] = cs.eye.y;
    cam.eye[2] = cs.eye.z;
    cam.target[0] = cs.target.x;
    cam.target[1] = cs.target.y;
    cam.target[2] = cs.target.z;
    cam.up[0] = cs.up.x;
    cam.up[1] = cs.up.y;
    cam.up[2] = cs.up.z;
    cam.fov_radians = cs.fov_radians;
    cam.near_plane = cs.near_plane;
    cam.far_plane = cs.far_plane;
    cam.aspect_ratio = cs.aspect_ratio;

    toc[sec_idx].type = MOP_SECTION_CAMERA;
    toc[sec_idx].offset = current_offset;
    toc[sec_idx].size = sizeof(cam);
    fwrite_bytes(fp, &cam, sizeof(cam));
    current_offset += sizeof(cam);
    fwrite_pad(fp, current_offset, MOP_SCENE_ALIGN);
    current_offset = align8(current_offset);
    sec_idx++;
  }

  /* Write light sections */
  for (uint32_t i = 0; i < nlight; i++) {
    const MopLight *l = mop_viewport_light_at(vp, i);
    if (!l)
      continue;

    MopLightSection ls = {0};
    ls.type = (uint32_t)l->type;
    ls.position[0] = l->position.x;
    ls.position[1] = l->position.y;
    ls.position[2] = l->position.z;
    ls.direction[0] = l->direction.x;
    ls.direction[1] = l->direction.y;
    ls.direction[2] = l->direction.z;
    ls.color[0] = l->color.r;
    ls.color[1] = l->color.g;
    ls.color[2] = l->color.b;
    ls.color[3] = l->color.a;
    ls.intensity = l->intensity;
    ls.range = l->range;
    ls.spot_inner_cos = l->spot_inner_cos;
    ls.spot_outer_cos = l->spot_outer_cos;

    toc[sec_idx].type = MOP_SECTION_LIGHT;
    toc[sec_idx].offset = current_offset;
    toc[sec_idx].size = sizeof(ls);
    fwrite_bytes(fp, &ls, sizeof(ls));
    current_offset += sizeof(ls);
    fwrite_pad(fp, current_offset, MOP_SCENE_ALIGN);
    current_offset = align8(current_offset);
    sec_idx++;
  }

  /* Write mesh sections */
  for (uint32_t m = 0; m < nmesh; m++) {
    MopMesh *mesh = mop_viewport_mesh_at(vp, m);
    if (!mesh)
      continue;

    const MopVertex *verts = mop_mesh_get_vertices(mesh, vp);
    const uint32_t *idxs = mop_mesh_get_indices(mesh, vp);
    uint32_t vcount = mop_mesh_get_vertex_count(mesh);
    uint32_t icount = mop_mesh_get_index_count(mesh);

    if (!verts || !idxs || vcount == 0)
      continue;

    /* Compute bbox */
    float bmin[3] = {verts[0].position.x, verts[0].position.y,
                     verts[0].position.z};
    float bmax[3] = {bmin[0], bmin[1], bmin[2]};
    for (uint32_t i = 1; i < vcount; i++) {
      const float *p = &verts[i].position.x;
      for (int j = 0; j < 3; j++) {
        if (p[j] < bmin[j])
          bmin[j] = p[j];
        if (p[j] > bmax[j])
          bmax[j] = p[j];
      }
    }

    MopMeshSectionHeader mhdr = {0};
    mhdr.vertex_count = vcount;
    mhdr.index_count = icount;
    mhdr.vertex_encoding = quantize ? MOP_VTX_QUANTIZED : MOP_VTX_RAW;
    mhdr.index_size = (vcount < 65536) ? 2 : 4;
    memcpy(mhdr.bbox_min, bmin, sizeof(bmin));
    memcpy(mhdr.bbox_max, bmax, sizeof(bmax));

    /* Calculate section size */
    size_t vtx_bytes;
    if (quantize) {
      vtx_bytes = (size_t)vcount * sizeof(MopQuantizedVertex);
    } else {
      vtx_bytes = (size_t)vcount * sizeof(MopVertex);
    }
    size_t idx_bytes;
    if (mhdr.index_size == 2) {
      idx_bytes = (size_t)icount * sizeof(uint16_t);
    } else {
      idx_bytes = (size_t)icount * sizeof(uint32_t);
    }

    size_t section_size =
        sizeof(MopMeshSectionHeader) + align8(vtx_bytes) + idx_bytes;

    toc[sec_idx].type = MOP_SECTION_MESH;
    toc[sec_idx].flags = mhdr.vertex_encoding;
    toc[sec_idx].offset = current_offset;
    toc[sec_idx].size = section_size;

    /* Write mesh header */
    fwrite_bytes(fp, &mhdr, sizeof(mhdr));

    /* Write vertex data */
    if (quantize) {
      MopQuantizedVertex *qbuf = malloc(vtx_bytes);
      if (qbuf) {
        for (uint32_t i = 0; i < vcount; i++) {
          qbuf[i] = quantize_vertex(&verts[i], bmin, bmax);
        }
        fwrite_bytes(fp, qbuf, vtx_bytes);
        free(qbuf);
      }
    } else {
      fwrite_bytes(fp, verts, vtx_bytes);
    }

    /* Pad vertex data to 8-byte alignment */
    size_t written = sizeof(mhdr) + vtx_bytes;
    fwrite_pad(fp, current_offset + written, MOP_SCENE_ALIGN);

    /* Write index data */
    if (mhdr.index_size == 2) {
      uint16_t *idx16 = malloc(idx_bytes);
      if (idx16) {
        for (uint32_t i = 0; i < icount; i++) {
          idx16[i] = (uint16_t)idxs[i];
        }
        fwrite_bytes(fp, idx16, idx_bytes);
        free(idx16);
      }
    } else {
      fwrite_bytes(fp, idxs, idx_bytes);
    }

    current_offset += section_size;
    fwrite_pad(fp, current_offset, MOP_SCENE_ALIGN);
    current_offset = align8(current_offset);
    sec_idx++;
  }

  /* Seek back and write the filled-in TOC */
  fseek(fp, (long)toc_start, SEEK_SET);
  fwrite_bytes(fp, toc, sec_idx * sizeof(MopSceneTocEntry));

  /* Update section count if some meshes were skipped */
  if (sec_idx != nsec) {
    header.section_count = sec_idx;
    fseek(fp, 0, SEEK_SET);
    fwrite_bytes(fp, &header, sizeof(header));
  }

  free(toc);
  fclose(fp);

  MOP_INFO("mop_scene_save: wrote '%s' (%u sections, %s)", path, sec_idx,
           quantize ? "quantized" : "raw");
  return 0;
}
