/*
 * Master of Puppets — Conformance Framework
 * scene_gen.c — Procedural mesh generation and stress-scene builder
 *
 * All meshes use CCW winding.  Column-major matrices match the rest of MOP.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scene_gen.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <mop/types.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static MopVertex make_vertex(MopVec3 pos, MopVec3 nrm, float u, float v) {
  MopVertex vtx;
  vtx.position = pos;
  vtx.normal = nrm;
  vtx.color = (MopColor){1.0f, 1.0f, 1.0f, 1.0f};
  vtx.u = u;
  vtx.v = v;
  return vtx;
}

/* Encode a tangent-space normal [−1,1] to RGBA8 [0,255]. */
static void encode_normal_rgba8(uint8_t *out, float nx, float ny, float nz) {
  out[0] = (uint8_t)((nx * 0.5f + 0.5f) * 255.0f);
  out[1] = (uint8_t)((ny * 0.5f + 0.5f) * 255.0f);
  out[2] = (uint8_t)((nz * 0.5f + 0.5f) * 255.0f);
  out[3] = 255;
}

/* =========================================================================
 * mop_gen_sphere — UV sphere
 *
 * rings  = number of latitude bands (e.g. 32)
 * sectors = number of longitude segments (e.g. 64)
 *
 * Vertex count : (rings + 1) * (sectors + 1)
 * Index count  : rings * sectors * 6
 * ========================================================================= */

MopVertex *mop_gen_sphere(uint32_t rings, uint32_t sectors,
                          uint32_t *out_vertex_count, uint32_t **out_indices,
                          uint32_t *out_index_count) {
  if (rings < 2)
    rings = 2;
  if (sectors < 3)
    sectors = 3;

  uint32_t vert_count = (rings + 1) * (sectors + 1);
  uint32_t idx_count = rings * sectors * 6;

  MopVertex *verts = (MopVertex *)calloc(vert_count, sizeof(MopVertex));
  uint32_t *indices = (uint32_t *)calloc(idx_count, sizeof(uint32_t));
  if (!verts || !indices) {
    free(verts);
    free(indices);
    return NULL;
  }

  /* --- vertices --- */
  float const R = 1.0f / (float)rings;
  float const S = 1.0f / (float)sectors;
  uint32_t vi = 0;

  for (uint32_t r = 0; r <= rings; r++) {
    float phi = (float)M_PI * (float)r * R; /* 0 .. pi */
    float sin_phi = sinf(phi);
    float cos_phi = cosf(phi);

    for (uint32_t s = 0; s <= sectors; s++) {
      float theta = 2.0f * (float)M_PI * (float)s * S; /* 0 .. 2pi */
      float sin_theta = sinf(theta);
      float cos_theta = cosf(theta);

      MopVec3 n = {sin_phi * cos_theta, cos_phi, sin_phi * sin_theta};
      float u_coord = (float)s * S;
      float v_coord = (float)r * R;

      verts[vi++] = make_vertex(n, n, u_coord, v_coord);
    }
  }

  /* --- indices (CCW) --- */
  uint32_t ii = 0;
  uint32_t row_len = sectors + 1;

  for (uint32_t r = 0; r < rings; r++) {
    for (uint32_t s = 0; s < sectors; s++) {
      uint32_t cur = r * row_len + s;
      uint32_t nxt = cur + row_len;

      /* First triangle (CCW when viewed from outside) */
      indices[ii++] = cur;
      indices[ii++] = nxt;
      indices[ii++] = cur + 1;

      /* Second triangle */
      indices[ii++] = cur + 1;
      indices[ii++] = nxt;
      indices[ii++] = nxt + 1;
    }
  }

  *out_vertex_count = vert_count;
  *out_indices = indices;
  *out_index_count = idx_count;
  return verts;
}

/* =========================================================================
 * mop_gen_cylinder — capped cylinder
 *
 * segments = slices around circumference
 * stacks   = subdivisions along the height axis
 *
 * Unit radius, unit height (y in [0, 1]).
 * ========================================================================= */

MopVertex *mop_gen_cylinder(uint32_t segments, uint32_t stacks,
                            uint32_t *out_vertex_count, uint32_t **out_indices,
                            uint32_t *out_index_count) {
  if (segments < 3)
    segments = 3;
  if (stacks < 1)
    stacks = 1;

  /* Body: (stacks + 1) * (segments + 1)
   * Top cap: segments + 1  (fan center + ring)
   * Bottom cap: segments + 1 */
  uint32_t body_verts = (stacks + 1) * (segments + 1);
  uint32_t cap_verts = (segments + 1) * 2; /* top + bottom center + ring each */
  uint32_t vert_count = body_verts + cap_verts;

  uint32_t body_idx = stacks * segments * 6;
  uint32_t cap_idx = segments * 3 * 2; /* top + bottom fans */
  uint32_t idx_count = body_idx + cap_idx;

  MopVertex *verts = (MopVertex *)calloc(vert_count, sizeof(MopVertex));
  uint32_t *indices = (uint32_t *)calloc(idx_count, sizeof(uint32_t));
  if (!verts || !indices) {
    free(verts);
    free(indices);
    return NULL;
  }

  uint32_t vi = 0;
  uint32_t ii = 0;

  /* --- body --- */
  for (uint32_t t = 0; t <= stacks; t++) {
    float y = (float)t / (float)stacks;
    for (uint32_t s = 0; s <= segments; s++) {
      float theta = 2.0f * (float)M_PI * (float)s / (float)segments;
      float cx = cosf(theta);
      float cz = sinf(theta);
      MopVec3 pos = {cx, y, cz};
      MopVec3 nrm = {cx, 0.0f, cz};
      nrm = mop_vec3_normalize(nrm);
      float u_coord = (float)s / (float)segments;
      float v_coord = y;
      verts[vi++] = make_vertex(pos, nrm, u_coord, v_coord);
    }
  }

  uint32_t row_len = segments + 1;
  for (uint32_t t = 0; t < stacks; t++) {
    for (uint32_t s = 0; s < segments; s++) {
      uint32_t cur = t * row_len + s;
      uint32_t nxt = cur + row_len;

      /* CCW winding viewed from outside */
      indices[ii++] = cur;
      indices[ii++] = nxt;
      indices[ii++] = cur + 1;

      indices[ii++] = cur + 1;
      indices[ii++] = nxt;
      indices[ii++] = nxt + 1;
    }
  }

  /* --- top cap (y = 1) --- */
  uint32_t top_center = vi;
  verts[vi++] = make_vertex((MopVec3){0.0f, 1.0f, 0.0f},
                            (MopVec3){0.0f, 1.0f, 0.0f}, 0.5f, 0.5f);
  uint32_t top_ring_start = vi;
  for (uint32_t s = 0; s < segments; s++) {
    float theta = 2.0f * (float)M_PI * (float)s / (float)segments;
    float cx = cosf(theta);
    float cz = sinf(theta);
    MopVec3 pos = {cx, 1.0f, cz};
    MopVec3 nrm = {0.0f, 1.0f, 0.0f};
    float u_coord = cx * 0.5f + 0.5f;
    float v_coord = cz * 0.5f + 0.5f;
    verts[vi++] = make_vertex(pos, nrm, u_coord, v_coord);
  }

  /* Fan triangles — CCW when viewed from above (+Y) */
  for (uint32_t s = 0; s < segments; s++) {
    indices[ii++] = top_center;
    indices[ii++] = top_ring_start + s;
    indices[ii++] = top_ring_start + ((s + 1) % segments);
  }

  /* --- bottom cap (y = 0) --- */
  uint32_t bot_center = vi;
  verts[vi++] = make_vertex((MopVec3){0.0f, 0.0f, 0.0f},
                            (MopVec3){0.0f, -1.0f, 0.0f}, 0.5f, 0.5f);
  uint32_t bot_ring_start = vi;
  for (uint32_t s = 0; s < segments; s++) {
    float theta = 2.0f * (float)M_PI * (float)s / (float)segments;
    float cx = cosf(theta);
    float cz = sinf(theta);
    MopVec3 pos = {cx, 0.0f, cz};
    MopVec3 nrm = {0.0f, -1.0f, 0.0f};
    float u_coord = cx * 0.5f + 0.5f;
    float v_coord = cz * 0.5f + 0.5f;
    verts[vi++] = make_vertex(pos, nrm, u_coord, v_coord);
  }

  /* Fan triangles — CCW when viewed from below (-Y) */
  for (uint32_t s = 0; s < segments; s++) {
    indices[ii++] = bot_center;
    indices[ii++] = bot_ring_start + ((s + 1) % segments);
    indices[ii++] = bot_ring_start + s;
  }

  *out_vertex_count = vi;
  *out_indices = indices;
  *out_index_count = ii;
  return verts;
}

/* =========================================================================
 * mop_gen_quad — single quad, 2 triangles
 *
 * Centered at origin in XY plane, facing +Z.
 * 4 vertices, 6 indices.
 * ========================================================================= */

MopVertex *mop_gen_quad(float w, float h, uint32_t *out_vertex_count,
                        uint32_t **out_indices, uint32_t *out_index_count) {
  uint32_t vert_count = 4;
  uint32_t idx_count = 6;

  MopVertex *verts = (MopVertex *)calloc(vert_count, sizeof(MopVertex));
  uint32_t *indices = (uint32_t *)calloc(idx_count, sizeof(uint32_t));
  if (!verts || !indices) {
    free(verts);
    free(indices);
    return NULL;
  }

  float hw = w * 0.5f;
  float hh = h * 0.5f;
  MopVec3 n = {0.0f, 0.0f, 1.0f};

  verts[0] = make_vertex((MopVec3){-hw, -hh, 0.0f}, n, 0.0f, 0.0f);
  verts[1] = make_vertex((MopVec3){hw, -hh, 0.0f}, n, 1.0f, 0.0f);
  verts[2] = make_vertex((MopVec3){hw, hh, 0.0f}, n, 1.0f, 1.0f);
  verts[3] = make_vertex((MopVec3){-hw, hh, 0.0f}, n, 0.0f, 1.0f);

  /* CCW winding */
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 0;
  indices[4] = 2;
  indices[5] = 3;

  *out_vertex_count = vert_count;
  *out_indices = indices;
  *out_index_count = idx_count;
  return verts;
}

/* =========================================================================
 * mop_gen_cube — 6 faces, 24 vertices, 36 indices
 *
 * Centered at origin, side length `size`.
 * Each face has its own outward normal — no shared vertices across faces.
 * ========================================================================= */

MopVertex *mop_gen_cube(float size, uint32_t *out_vertex_count,
                        uint32_t **out_indices, uint32_t *out_index_count) {
  uint32_t vert_count = 24;
  uint32_t idx_count = 36;

  MopVertex *verts = (MopVertex *)calloc(vert_count, sizeof(MopVertex));
  uint32_t *indices = (uint32_t *)calloc(idx_count, sizeof(uint32_t));
  if (!verts || !indices) {
    free(verts);
    free(indices);
    return NULL;
  }

  float hs = size * 0.5f;

  /*
   * Face layout — each face is 4 verts (a quad), 2 triangles.
   * Winding is CCW when viewed from outside.
   *
   *   face 0: +Z   face 1: -Z
   *   face 2: +X   face 3: -X
   *   face 4: +Y   face 5: -Y
   */
  struct {
    MopVec3 n;
    MopVec3 v[4];
    float uv[4][2];
  } faces[6] = {
      /* +Z */
      {.n = {0, 0, 1},
       .v = {{-hs, -hs, hs}, {hs, -hs, hs}, {hs, hs, hs}, {-hs, hs, hs}},
       .uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
      /* -Z */
      {.n = {0, 0, -1},
       .v = {{hs, -hs, -hs}, {-hs, -hs, -hs}, {-hs, hs, -hs}, {hs, hs, -hs}},
       .uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
      /* +X */
      {.n = {1, 0, 0},
       .v = {{hs, -hs, hs}, {hs, -hs, -hs}, {hs, hs, -hs}, {hs, hs, hs}},
       .uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
      /* -X */
      {.n = {-1, 0, 0},
       .v = {{-hs, -hs, -hs}, {-hs, -hs, hs}, {-hs, hs, hs}, {-hs, hs, -hs}},
       .uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
      /* +Y */
      {.n = {0, 1, 0},
       .v = {{-hs, hs, hs}, {hs, hs, hs}, {hs, hs, -hs}, {-hs, hs, -hs}},
       .uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
      /* -Y */
      {.n = {0, -1, 0},
       .v = {{-hs, -hs, -hs}, {hs, -hs, -hs}, {hs, -hs, hs}, {-hs, -hs, hs}},
       .uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
  };

  for (int f = 0; f < 6; f++) {
    uint32_t base = (uint32_t)f * 4;
    for (int v = 0; v < 4; v++) {
      verts[base + v] = make_vertex(faces[f].v[v], faces[f].n,
                                    faces[f].uv[v][0], faces[f].uv[v][1]);
    }
    /* Two CCW triangles per face */
    uint32_t bi = (uint32_t)f * 6;
    indices[bi + 0] = base + 0;
    indices[bi + 1] = base + 1;
    indices[bi + 2] = base + 2;
    indices[bi + 3] = base + 0;
    indices[bi + 4] = base + 2;
    indices[bi + 5] = base + 3;
  }

  *out_vertex_count = vert_count;
  *out_indices = indices;
  *out_index_count = idx_count;
  return verts;
}

/* =========================================================================
 * mop_gen_bone_chain — segmented chain for animation stress
 *
 * bone_count segments of length segment_len each, connected end-to-end
 * along the Y axis.  Each segment is a small cylinder (8 sides, 1 stack)
 * with radius 0.3.  White vertex color, CCW winding.
 * ========================================================================= */

#define BONE_CHAIN_SEGMENTS 8
#define BONE_CHAIN_STACKS 1
#define BONE_CHAIN_RADIUS 0.3f

MopVertex *mop_gen_bone_chain(uint32_t bone_count, float segment_len,
                              uint32_t *out_vertex_count,
                              uint32_t **out_indices,
                              uint32_t *out_index_count) {
  if (bone_count == 0) {
    *out_vertex_count = 0;
    *out_indices = NULL;
    *out_index_count = 0;
    return NULL;
  }

  uint32_t const segs = BONE_CHAIN_SEGMENTS;
  uint32_t const stks = BONE_CHAIN_STACKS;

  /* Per-bone vertex/index counts (same formula as mop_gen_cylinder) */
  uint32_t body_verts_per = (stks + 1) * (segs + 1);
  uint32_t cap_verts_per = (segs + 1) * 2;
  uint32_t verts_per_bone = body_verts_per + cap_verts_per;

  uint32_t body_idx_per = stks * segs * 6;
  uint32_t cap_idx_per = segs * 3 * 2;
  uint32_t idx_per_bone = body_idx_per + cap_idx_per;

  uint32_t total_verts = bone_count * verts_per_bone;
  uint32_t total_idx = bone_count * idx_per_bone;

  MopVertex *verts = (MopVertex *)calloc(total_verts, sizeof(MopVertex));
  uint32_t *indices = (uint32_t *)calloc(total_idx, sizeof(uint32_t));
  if (!verts || !indices) {
    free(verts);
    free(indices);
    return NULL;
  }

  uint32_t vi = 0;
  uint32_t ii = 0;

  for (uint32_t bone = 0; bone < bone_count; bone++) {
    float y_base = (float)bone * segment_len;
    float y_top = y_base + segment_len;
    uint32_t base_vi = vi;

    /* --- body --- */
    for (uint32_t t = 0; t <= stks; t++) {
      float frac = (float)t / (float)stks;
      float y = y_base + frac * segment_len;
      for (uint32_t s = 0; s <= segs; s++) {
        float theta = 2.0f * (float)M_PI * (float)s / (float)segs;
        float cx = cosf(theta) * BONE_CHAIN_RADIUS;
        float cz = sinf(theta) * BONE_CHAIN_RADIUS;
        MopVec3 pos = {cx, y, cz};
        MopVec3 nrm =
            mop_vec3_normalize((MopVec3){cosf(theta), 0.0f, sinf(theta)});
        float u_coord = (float)s / (float)segs;
        float v_coord = frac;
        verts[vi++] = make_vertex(pos, nrm, u_coord, v_coord);
      }
    }

    uint32_t row_len = segs + 1;
    for (uint32_t t = 0; t < stks; t++) {
      for (uint32_t s = 0; s < segs; s++) {
        uint32_t cur = base_vi + t * row_len + s;
        uint32_t nxt = cur + row_len;
        indices[ii++] = cur;
        indices[ii++] = nxt;
        indices[ii++] = cur + 1;
        indices[ii++] = cur + 1;
        indices[ii++] = nxt;
        indices[ii++] = nxt + 1;
      }
    }

    /* --- top cap --- */
    uint32_t top_center = vi;
    verts[vi++] = make_vertex((MopVec3){0.0f, y_top, 0.0f},
                              (MopVec3){0.0f, 1.0f, 0.0f}, 0.5f, 0.5f);
    uint32_t top_ring_start = vi;
    for (uint32_t s = 0; s < segs; s++) {
      float theta = 2.0f * (float)M_PI * (float)s / (float)segs;
      float cx = cosf(theta) * BONE_CHAIN_RADIUS;
      float cz = sinf(theta) * BONE_CHAIN_RADIUS;
      verts[vi++] =
          make_vertex((MopVec3){cx, y_top, cz}, (MopVec3){0.0f, 1.0f, 0.0f},
                      cosf(theta) * 0.5f + 0.5f, sinf(theta) * 0.5f + 0.5f);
    }
    for (uint32_t s = 0; s < segs; s++) {
      indices[ii++] = top_center;
      indices[ii++] = top_ring_start + s;
      indices[ii++] = top_ring_start + ((s + 1) % segs);
    }

    /* --- bottom cap --- */
    uint32_t bot_center = vi;
    verts[vi++] = make_vertex((MopVec3){0.0f, y_base, 0.0f},
                              (MopVec3){0.0f, -1.0f, 0.0f}, 0.5f, 0.5f);
    uint32_t bot_ring_start = vi;
    for (uint32_t s = 0; s < segs; s++) {
      float theta = 2.0f * (float)M_PI * (float)s / (float)segs;
      float cx = cosf(theta) * BONE_CHAIN_RADIUS;
      float cz = sinf(theta) * BONE_CHAIN_RADIUS;
      verts[vi++] =
          make_vertex((MopVec3){cx, y_base, cz}, (MopVec3){0.0f, -1.0f, 0.0f},
                      cosf(theta) * 0.5f + 0.5f, sinf(theta) * 0.5f + 0.5f);
    }
    for (uint32_t s = 0; s < segs; s++) {
      indices[ii++] = bot_center;
      indices[ii++] = bot_ring_start + ((s + 1) % segs);
      indices[ii++] = bot_ring_start + s;
    }
  }

  *out_vertex_count = vi;
  *out_indices = indices;
  *out_index_count = ii;
  return verts;
}

/* =========================================================================
 * Procedural textures
 * ========================================================================= */

/* --- Checker texture (RGBA8) ---
 *
 * `tiles` = number of tiles per axis.  E.g. tiles=8 gives an 8x8 grid.
 * Dark tile = (50,50,50,255), white tile = (255,255,255,255). */

uint8_t *mop_gen_checker_texture(int width, int height, int tiles) {
  if (width <= 0 || height <= 0 || tiles <= 0)
    return NULL;

  uint8_t *tex = (uint8_t *)malloc((size_t)width * (size_t)height * 4);
  if (!tex)
    return NULL;

  for (int y = 0; y < height; y++) {
    int ty = (y * tiles) / height;
    for (int x = 0; x < width; x++) {
      int tx = (x * tiles) / width;
      uint8_t c = ((tx + ty) & 1) ? 50 : 255;
      int idx = (y * width + x) * 4;
      tex[idx + 0] = c;
      tex[idx + 1] = c;
      tex[idx + 2] = c;
      tex[idx + 3] = 255;
    }
  }
  return tex;
}

/* --- Brick normal map (RGBA8, tangent space) ---
 *
 * Generates a simple running-bond brick pattern.  Mortar channels produce
 * normals that tilt away from the groove center; brick faces point straight
 * out in tangent space (0, 0, 1). */

uint8_t *mop_gen_brick_normal_map(int width, int height) {
  if (width <= 0 || height <= 0)
    return NULL;

  uint8_t *tex = (uint8_t *)malloc((size_t)width * (size_t)height * 4);
  if (!tex)
    return NULL;

  /* Brick layout parameters (in texels) */
  int brick_rows = 8;
  int brick_cols = 4; /* bricks per row before offset */
  int mortar_px = 3;  /* mortar groove width in texels at 256 res */

  /* Scale mortar with resolution so the map looks right at any size */
  if (width >= 512)
    mortar_px = 6;
  else if (width <= 128)
    mortar_px = 2;

  float row_h = (float)height / (float)brick_rows;
  float col_w = (float)width / (float)brick_cols;

  for (int y = 0; y < height; y++) {
    int row = (int)((float)y / row_h);
    float y_in_row = (float)y - (float)row * row_h;

    /* Running bond: offset every other row by half a brick width */
    float x_offset = (row & 1) ? col_w * 0.5f : 0.0f;

    for (int x = 0; x < width; x++) {
      float xf = (float)x + x_offset;
      /* Wrap around */
      while (xf >= (float)width)
        xf -= (float)width;

      float x_in_brick = xf - floorf(xf / col_w) * col_w;

      /* Determine if this texel is in the mortar groove */
      float mortar_f = (float)mortar_px;
      int in_h_mortar = (y_in_row < mortar_f) ? 1 : 0;
      int in_v_mortar = (x_in_brick < mortar_f) ? 1 : 0;

      float nx = 0.0f;
      float ny = 0.0f;
      float nz = 1.0f;

      if (in_h_mortar) {
        /* Horizontal mortar: tilt normal in Y based on position in groove */
        float t = y_in_row / mortar_f; /* 0..1 across the groove */
        ny = (t < 0.5f) ? -(1.0f - 2.0f * t) * 0.7f : (2.0f * t - 1.0f) * 0.7f;
        nz = sqrtf(1.0f - ny * ny);
      }

      if (in_v_mortar) {
        /* Vertical mortar: tilt normal in X */
        float t = x_in_brick / mortar_f;
        nx = (t < 0.5f) ? -(1.0f - 2.0f * t) * 0.7f : (2.0f * t - 1.0f) * 0.7f;
        nz = sqrtf(fmaxf(0.0f, 1.0f - nx * nx - ny * ny));
      }

      /* Re-normalize just in case */
      float len = sqrtf(nx * nx + ny * ny + nz * nz);
      if (len > 1e-6f) {
        nx /= len;
        ny /= len;
        nz /= len;
      }

      int idx = (y * width + x) * 4;
      encode_normal_rgba8(tex + idx, nx, ny, nz);
    }
  }
  return tex;
}

/* =========================================================================
 * Conformance stress scene
 * ========================================================================= */

/* --- Zone A helpers --- */

#define ZONE_A_GRID_SIZE 100
#define ZONE_A_SPACING 2.5f
#define ZONE_A_SPHERE_RINGS 16
#define ZONE_A_SPHERE_SECTORS 32

/* --- Zone B helpers --- */

#define ZONE_B_LEVELS 24
#define ZONE_B_CYL_SEGMENTS 16
#define ZONE_B_CYL_STACKS 1

/* --- Texture size --- */

#define CONF_TEX_SIZE 256

MopConfScene *mop_conf_scene_create(void) {
  MopConfScene *sc = (MopConfScene *)calloc(1, sizeof(MopConfScene));
  if (!sc)
    return NULL;

  /* -----------------------------------------------------------------
   * Zone A: instancing grid — 100x100 spheres
   * ----------------------------------------------------------------- */
  sc->sphere_verts = mop_gen_sphere(ZONE_A_SPHERE_RINGS, ZONE_A_SPHERE_SECTORS,
                                    &sc->sphere_vert_count, &sc->sphere_indices,
                                    &sc->sphere_index_count);
  if (!sc->sphere_verts)
    goto fail;

  sc->sphere_instance_count = ZONE_A_GRID_SIZE * ZONE_A_GRID_SIZE;
  sc->sphere_transforms =
      (MopMat4 *)calloc(sc->sphere_instance_count, sizeof(MopMat4));
  if (!sc->sphere_transforms)
    goto fail;

  {
    float half_extent = (float)(ZONE_A_GRID_SIZE - 1) * ZONE_A_SPACING * 0.5f;
    uint32_t idx = 0;

    for (uint32_t row = 0; row < ZONE_A_GRID_SIZE; row++) {
      for (uint32_t col = 0; col < ZONE_A_GRID_SIZE; col++) {
        float x = (float)col * ZONE_A_SPACING - half_extent;
        float z = (float)row * ZONE_A_SPACING - half_extent;

        /* Slight Y variation based on grid position for visual interest */
        float y = sinf((float)row * 0.3f) * cosf((float)col * 0.3f) * 0.5f;

        /* Scale varies subtly: 0.3 .. 0.5 */
        float s = 0.3f + 0.2f * (float)((row * 7 + col * 13) % 17) / 16.0f;

        sc->sphere_transforms[idx] = mop_mat4_compose_trs(
            (MopVec3){x, y, z}, (MopVec3){0.0f, 0.0f, 0.0f},
            (MopVec3){s, s, s});
        idx++;
      }
    }
  }

  /* -----------------------------------------------------------------
   * Zone B: hierarchy tower — 24 levels
   *
   * Each level is a cylinder, translated up by 1 unit (local) and
   * rotated by 15 degrees around Y.  World transforms are the
   * concatenation of local transforms from root to leaf.
   * ----------------------------------------------------------------- */
  sc->cylinder_verts = mop_gen_cylinder(
      ZONE_B_CYL_SEGMENTS, ZONE_B_CYL_STACKS, &sc->cylinder_vert_count,
      &sc->cylinder_indices, &sc->cylinder_index_count);
  if (!sc->cylinder_verts)
    goto fail;

  {
    float angle_step = 15.0f * (float)M_PI / 180.0f;
    float scale_decay = 0.97f; /* each level slightly smaller */

    for (int i = 0; i < ZONE_B_LEVELS; i++) {
      float s = powf(scale_decay, (float)i);
      sc->tower_local_transforms[i] = mop_mat4_compose_trs(
          (MopVec3){0.0f, 1.0f, 0.0f},
          (MopVec3){0.0f, angle_step * (float)i, 0.0f}, (MopVec3){s, 1.0f, s});
    }

    /* Compute world transforms: world[0] = local[0],
     *                            world[i] = world[i-1] * local[i] */
    sc->tower_world_transforms[0] = sc->tower_local_transforms[0];
    for (int i = 1; i < ZONE_B_LEVELS; i++) {
      sc->tower_world_transforms[i] = mop_mat4_multiply(
          sc->tower_world_transforms[i - 1], sc->tower_local_transforms[i]);
    }
  }

  /* -----------------------------------------------------------------
   * Zone C: precision stress — basic quad and cube
   * ----------------------------------------------------------------- */
  sc->quad_verts = mop_gen_quad(2.0f, 2.0f, &sc->quad_vert_count,
                                &sc->quad_indices, &sc->quad_index_count);
  if (!sc->quad_verts)
    goto fail;

  sc->cube_verts = mop_gen_cube(1.0f, &sc->cube_vert_count, &sc->cube_indices,
                                &sc->cube_index_count);
  if (!sc->cube_verts)
    goto fail;

  /* -----------------------------------------------------------------
   * Zone C: precision stress variants
   * ----------------------------------------------------------------- */

  /* Coplanar quads: 8 quads at z=-5.0 with tiny z offsets */
  sc->coplanar_quad_count = 8;
  sc->coplanar_quad_verts =
      mop_gen_quad(2.0f, 2.0f, &sc->coplanar_quad_vert_count,
                   &sc->coplanar_quad_indices, &sc->coplanar_quad_index_count);
  if (!sc->coplanar_quad_verts)
    goto fail;

  {
    float offsets[8] = {0.0f, 1e-2f, 1e-3f, 1e-4f, 1e-5f, 1e-6f, 1e-7f, 1e-8f};
    for (int i = 0; i < 8; i++) {
      sc->coplanar_transforms[i] =
          mop_mat4_translate((MopVec3){0.0f, 0.0f, -5.0f + offsets[i]});
    }
  }

  /* Degenerate triangles: 64 total
   *   [0..15]  collinear points
   *   [16..31] zero-area
   *   [32..63] extremely thin needle triangles (aspect 1:10000) */
  {
    uint32_t degen_tri_count = 64;
    uint32_t degen_vert_count = degen_tri_count * 3;
    uint32_t degen_idx_count = degen_tri_count * 3;

    sc->degenerate_verts =
        (MopVertex *)calloc(degen_vert_count, sizeof(MopVertex));
    sc->degenerate_indices =
        (uint32_t *)calloc(degen_idx_count, sizeof(uint32_t));
    if (!sc->degenerate_verts || !sc->degenerate_indices)
      goto fail;

    MopVec3 up = {0.0f, 1.0f, 0.0f};
    uint32_t vi = 0;
    uint32_t ii = 0;

    /* Collinear triangles (0..15): all 3 points on a line */
    for (int i = 0; i < 16; i++) {
      float offset = (float)i * 0.5f;
      MopVec3 a = {offset, 0.0f, 0.0f};
      MopVec3 b = {offset + 1.0f, 0.0f, 0.0f};
      MopVec3 c = {offset + 2.0f, 0.0f, 0.0f};
      sc->degenerate_verts[vi] = make_vertex(a, up, 0.0f, 0.0f);
      sc->degenerate_indices[ii++] = vi++;
      sc->degenerate_verts[vi] = make_vertex(b, up, 0.5f, 0.0f);
      sc->degenerate_indices[ii++] = vi++;
      sc->degenerate_verts[vi] = make_vertex(c, up, 1.0f, 0.0f);
      sc->degenerate_indices[ii++] = vi++;
    }

    /* Zero-area triangles (16..31): two or more coincident vertices */
    for (int i = 0; i < 16; i++) {
      float offset = (float)i * 0.5f;
      MopVec3 a = {offset, 0.0f, -2.0f};
      MopVec3 b = {offset, 0.0f, -2.0f}; /* same as a */
      MopVec3 c = {offset + 1.0f, 0.0f, -2.0f};
      sc->degenerate_verts[vi] = make_vertex(a, up, 0.0f, 0.0f);
      sc->degenerate_indices[ii++] = vi++;
      sc->degenerate_verts[vi] = make_vertex(b, up, 0.0f, 0.0f);
      sc->degenerate_indices[ii++] = vi++;
      sc->degenerate_verts[vi] = make_vertex(c, up, 1.0f, 0.0f);
      sc->degenerate_indices[ii++] = vi++;
    }

    /* Needle triangles (32..63): extremely thin, aspect ratio 1:10000 */
    for (int i = 0; i < 32; i++) {
      float offset = (float)i * 0.5f;
      float length = 10.0f;
      float width = 0.001f; /* 10 / 0.001 = 10000:1 aspect */
      MopVec3 a = {offset, 0.0f, -4.0f};
      MopVec3 b = {offset + length, 0.0f, -4.0f};
      MopVec3 c = {offset + length * 0.5f, width, -4.0f};
      MopVec3 nrm = {0.0f, 0.0f, -1.0f};
      sc->degenerate_verts[vi] = make_vertex(a, nrm, 0.0f, 0.0f);
      sc->degenerate_indices[ii++] = vi++;
      sc->degenerate_verts[vi] = make_vertex(b, nrm, 1.0f, 0.0f);
      sc->degenerate_indices[ii++] = vi++;
      sc->degenerate_verts[vi] = make_vertex(c, nrm, 0.5f, 1.0f);
      sc->degenerate_indices[ii++] = vi++;
    }

    sc->degenerate_vert_count = vi;
    sc->degenerate_index_count = ii;
  }

  /* Extreme-scale transforms */
  sc->huge_plane_transform = mop_mat4_scale((MopVec3){1e6f, 1.0f, 1e6f});
  sc->micro_cube_transform = mop_mat4_scale((MopVec3){1e-6f, 1e-6f, 1e-6f});
  sc->macro_cube_transform =
      mop_mat4_multiply(mop_mat4_translate((MopVec3){1e4f, 0.0f, 0.0f}),
                        mop_mat4_scale((MopVec3){1e3f, 1e3f, 1e3f}));
  sc->neg_scale_cube_transform = mop_mat4_scale((MopVec3){-1.0f, 1.0f, 1.0f});

  /* -----------------------------------------------------------------
   * Zone D: transparency stress
   * ----------------------------------------------------------------- */

  /* 16 intersecting alpha planes at (200, 0, -400) */
  {
    MopVec3 base_pos = {200.0f, 0.0f, -400.0f};
    for (int i = 0; i < 16; i++) {
      float y_rot = (float)i * (float)M_PI / 16.0f;
      float x_tilt = (i % 3 == 0) ? 0.2f * (float)(i / 3) : 0.0f;
      MopMat4 rot = mop_mat4_multiply(mop_mat4_rotate_y(y_rot),
                                      mop_mat4_rotate_x(x_tilt));
      MopMat4 trans = mop_mat4_translate(base_pos);
      sc->alpha_plane_transforms[i] = mop_mat4_multiply(trans, rot);
      /* Opacity varies 0.1 .. 0.9 across 16 planes */
      sc->alpha_plane_opacities[i] = 0.1f + 0.8f * (float)i / 15.0f;
    }
  }

  /* 4x4 alpha-clipped billboard grid at (200, 0, -400) offset */
  {
    float spacing = 3.0f;
    float half = (4.0f - 1.0f) * spacing * 0.5f;
    MopVec3 grid_origin = {200.0f, 0.0f, -400.0f};
    int idx = 0;
    for (int row = 0; row < 4; row++) {
      for (int col = 0; col < 4; col++) {
        float x = grid_origin.x + (float)col * spacing - half;
        float y = grid_origin.y + (float)row * spacing - half;
        float z = grid_origin.z;
        sc->alpha_clip_transforms[idx] = mop_mat4_translate((MopVec3){x, y, z});
        idx++;
      }
    }
  }

  /* 8 double-sided alpha planes stacked with alternating normals */
  {
    for (int i = 0; i < 8; i++) {
      float y_offset = (float)i * 1.5f;
      float flip = (i % 2 == 0) ? 0.0f : (float)M_PI;
      MopMat4 rot = mop_mat4_rotate_y(flip);
      MopMat4 trans = mop_mat4_translate((MopVec3){200.0f, y_offset, -410.0f});
      sc->double_sided_transforms[i] = mop_mat4_multiply(trans, rot);
    }
  }

  /* -----------------------------------------------------------------
   * Zone F: material stress grid -- 6x4 spheres
   * ----------------------------------------------------------------- */
  {
    float spacing = 3.0f;
    MopVec3 grid_center = {-200.0f, 0.0f, -400.0f};
    float half_cols = (6.0f - 1.0f) * spacing * 0.5f;
    float half_rows = (4.0f - 1.0f) * spacing * 0.5f;
    float roughness_vals[6] = {0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f};

    int idx = 0;
    for (int row = 0; row < 4; row++) {
      for (int col = 0; col < 6; col++) {
        float x = grid_center.x + (float)col * spacing - half_cols;
        float y = grid_center.y + (float)row * spacing - half_rows;
        float z = grid_center.z;
        sc->material_sphere_transforms[idx] =
            mop_mat4_translate((MopVec3){x, y, z});

        switch (row) {
        case 0:
          sc->material_metallic[idx] = 0.0f;
          sc->material_roughness[idx] = roughness_vals[col];
          break;
        case 1:
          sc->material_metallic[idx] = 0.5f;
          sc->material_roughness[idx] = roughness_vals[col];
          break;
        case 2:
          sc->material_metallic[idx] = 1.0f;
          sc->material_roughness[idx] = roughness_vals[col];
          break;
        case 3:
          /* Special materials row: emissive, normal-mapped, textured,
           * double-sided, high-roughness metallic, glass-like */
          sc->material_metallic[idx] = (col < 3) ? 0.0f : 1.0f;
          sc->material_roughness[idx] = 0.5f;
          break;
        }
        idx++;
      }
    }
  }

  /* -----------------------------------------------------------------
   * Zone G: animation stress -- bone chain
   * ----------------------------------------------------------------- */
  sc->bone_chain_bone_count = 128;
  sc->bone_chain_segment_len = 1.5f;
  sc->bone_chain_verts =
      mop_gen_bone_chain(sc->bone_chain_bone_count, sc->bone_chain_segment_len,
                         &sc->bone_chain_vert_count, &sc->bone_chain_indices,
                         &sc->bone_chain_index_count);
  if (!sc->bone_chain_verts)
    goto fail;

  /* -----------------------------------------------------------------
   * Procedural textures (256x256)
   * ----------------------------------------------------------------- */
  sc->checker_tex_size = CONF_TEX_SIZE;
  sc->checker_tex = mop_gen_checker_texture(CONF_TEX_SIZE, CONF_TEX_SIZE, 8);
  if (!sc->checker_tex)
    goto fail;

  sc->brick_nm_tex_size = CONF_TEX_SIZE;
  sc->brick_nm_tex = mop_gen_brick_normal_map(CONF_TEX_SIZE, CONF_TEX_SIZE);
  if (!sc->brick_nm_tex)
    goto fail;

  return sc;

fail:
  mop_conf_scene_destroy(sc);
  return NULL;
}

void mop_conf_scene_destroy(MopConfScene *scene) {
  if (!scene)
    return;

  /* Zone A */
  free(scene->sphere_verts);
  free(scene->sphere_indices);
  free(scene->sphere_transforms);

  /* Zone B */
  free(scene->cylinder_verts);
  free(scene->cylinder_indices);

  /* Zone C */
  free(scene->quad_verts);
  free(scene->quad_indices);
  free(scene->cube_verts);
  free(scene->cube_indices);
  free(scene->coplanar_quad_verts);
  free(scene->coplanar_quad_indices);
  free(scene->degenerate_verts);
  free(scene->degenerate_indices);

  /* Zone G */
  free(scene->bone_chain_verts);
  free(scene->bone_chain_indices);

  /* Textures */
  free(scene->checker_tex);
  free(scene->brick_nm_tex);

  free(scene);
}
