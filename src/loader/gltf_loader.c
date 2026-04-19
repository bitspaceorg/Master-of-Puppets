/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * gltf_loader.c — glTF 2.0 loader (Phase 8C)
 *
 * Loads .glb (binary glTF) and .gltf (JSON + external .bin) files.
 * Parses the JSON chunk with a minimal hand-rolled parser (no deps).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <mop/core/material.h>
#include <mop/core/scene.h>
#include <mop/core/texture_pipeline.h>
#include <mop/loader/gltf.h>
#include <mop/util/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_image for decoding embedded image data */
#include "stb_image.h"

/* -------------------------------------------------------------------------
 * GLB binary container constants
 * ------------------------------------------------------------------------- */

#define GLB_MAGIC 0x46546C67u /* "glTF" */
#define GLB_VERSION 2
#define GLB_CHUNK_JSON 0x4E4F534Au /* "JSON" */
#define GLB_CHUNK_BIN 0x004E4942u  /* "BIN\0" */

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t length;
} GlbHeader;

typedef struct {
  uint32_t chunk_length;
  uint32_t chunk_type;
} GlbChunkHeader;

/* -------------------------------------------------------------------------
 * Minimal JSON parser helpers (reused from material_graph)
 * ------------------------------------------------------------------------- */

static const char *gltf_skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  return p;
}

static const char *gltf_read_string(const char *p, char *dst, size_t dstlen) {
  if (*p != '"')
    return p;
  p++;
  size_t i = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && *(p + 1))
      p++;
    if (i + 1 < dstlen)
      dst[i++] = *p;
    p++;
  }
  dst[i] = '\0';
  if (*p == '"')
    p++;
  return p;
}

static const char *gltf_read_int(const char *p, int32_t *out) {
  char *end;
  *out = (int32_t)strtol(p, &end, 10);
  return end;
}

static const char *gltf_read_float(const char *p, float *out) {
  char *end;
  *out = strtof(p, &end);
  return end;
}

static const char *gltf_skip_value(const char *p) {
  p = gltf_skip_ws(p);
  if (*p == '"') {
    p++;
    while (*p && *p != '"') {
      if (*p == '\\')
        p++;
      p++;
    }
    if (*p == '"')
      p++;
  } else if (*p == '{') {
    int d = 1;
    p++;
    while (*p && d > 0) {
      if (*p == '{')
        d++;
      else if (*p == '}')
        d--;
      else if (*p == '"') {
        p++;
        while (*p && *p != '"') {
          if (*p == '\\')
            p++;
          p++;
        }
      }
      p++;
    }
  } else if (*p == '[') {
    int d = 1;
    p++;
    while (*p && d > 0) {
      if (*p == '[')
        d++;
      else if (*p == ']')
        d--;
      else if (*p == '"') {
        p++;
        while (*p && *p != '"') {
          if (*p == '\\')
            p++;
          p++;
        }
      }
      p++;
    }
  } else {
    while (*p && *p != ',' && *p != '}' && *p != ']')
      p++;
  }
  return p;
}

/* -------------------------------------------------------------------------
 * File reading helpers
 * ------------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, uint32_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return NULL;
  }
  fseek(f, 0, SEEK_SET);
  uint8_t *data = (uint8_t *)malloc((size_t)sz);
  if (!data) {
    fclose(f);
    return NULL;
  }
  if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
    free(data);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *out_size = (uint32_t)sz;
  return data;
}

/* Read uint32_t from little-endian bytes */
static uint32_t read_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* -------------------------------------------------------------------------
 * Accessor data extraction
 *
 * glTF accessors reference buffer views which reference buffers.
 * We resolve the full chain to get a pointer into _buffer_data.
 * ------------------------------------------------------------------------- */

/* Parsed accessor info (temporary, during loading) */
typedef struct {
  int32_t buffer_view;
  uint32_t byte_offset;
  uint32_t count;
  uint32_t component_type; /* 5120-5126 */
  uint32_t type_count;     /* SCALAR=1, VEC2=2, VEC3=3, VEC4=4, MAT4=16 */
} GltfAccessor;

typedef struct {
  int32_t buffer;
  uint32_t byte_offset;
  uint32_t byte_length;
  uint32_t byte_stride;
} GltfBufferView;

/* Temporary parsing state */
typedef struct {
  const char *json;
  const uint8_t *bin_data;
  uint32_t bin_size;

  GltfAccessor *accessors;
  uint32_t accessor_count;
  GltfBufferView *buffer_views;
  uint32_t buffer_view_count;

  MopGltfScene *scene;
} GltfParseCtx;

static uint32_t type_name_to_count(const char *name) {
  if (strcmp(name, "SCALAR") == 0)
    return 1;
  if (strcmp(name, "VEC2") == 0)
    return 2;
  if (strcmp(name, "VEC3") == 0)
    return 3;
  if (strcmp(name, "VEC4") == 0)
    return 4;
  if (strcmp(name, "MAT2") == 0)
    return 4;
  if (strcmp(name, "MAT3") == 0)
    return 9;
  if (strcmp(name, "MAT4") == 0)
    return 16;
  return 0;
}

static const uint8_t *resolve_accessor(const GltfParseCtx *ctx,
                                       uint32_t accessor_idx,
                                       uint32_t *out_count,
                                       uint32_t *out_stride) {
  if (accessor_idx >= ctx->accessor_count)
    return NULL;
  const GltfAccessor *acc = &ctx->accessors[accessor_idx];
  if (acc->buffer_view < 0 ||
      (uint32_t)acc->buffer_view >= ctx->buffer_view_count)
    return NULL;

  const GltfBufferView *bv = &ctx->buffer_views[acc->buffer_view];
  uint32_t offset = bv->byte_offset + acc->byte_offset;
  if (offset >= ctx->bin_size)
    return NULL;

  *out_count = acc->count;
  /* Compute element size */
  uint32_t comp_size = 4; /* float */
  if (acc->component_type == 5120 || acc->component_type == 5121)
    comp_size = 1;
  else if (acc->component_type == 5122 || acc->component_type == 5123)
    comp_size = 2;

  *out_stride =
      bv->byte_stride ? bv->byte_stride : (comp_size * acc->type_count);
  return ctx->bin_data + offset;
}

/* -------------------------------------------------------------------------
 * Parse JSON sections
 * ------------------------------------------------------------------------- */

/* Parse a float array from JSON: [1.0, 2.0, ...] */
static const char *parse_float_array(const char *p, float *out, int max_count,
                                     int *actual_count) {
  p = gltf_skip_ws(p);
  if (*p != '[')
    return p;
  p++;
  int n = 0;
  while (*p && *p != ']') {
    p = gltf_skip_ws(p);
    if (*p == ',') {
      p++;
      continue;
    }
    if (n < max_count) {
      p = gltf_read_float(p, &out[n]);
      n++;
    } else {
      p = gltf_skip_value(p);
    }
    p = gltf_skip_ws(p);
    if (*p == ',')
      p++;
  }
  if (*p == ']')
    p++;
  if (actual_count)
    *actual_count = n;
  return p;
}

/* Parse an int array from JSON: [0, 1, 2, ...] */
static const char *parse_int_array(const char *p, int32_t *out, int max_count,
                                   int *actual_count) {
  p = gltf_skip_ws(p);
  if (*p != '[')
    return p;
  p++;
  int n = 0;
  while (*p && *p != ']') {
    p = gltf_skip_ws(p);
    if (*p == ',') {
      p++;
      continue;
    }
    if (n < max_count) {
      p = gltf_read_int(p, &out[n]);
      n++;
    } else {
      p = gltf_skip_value(p);
    }
    p = gltf_skip_ws(p);
    if (*p == ',')
      p++;
  }
  if (*p == ']')
    p++;
  if (actual_count)
    *actual_count = n;
  return p;
}

/* Parse buffer views array */
static const char *parse_buffer_views(const char *p, GltfParseCtx *ctx) {
  p = gltf_skip_ws(p);
  if (*p != '[')
    return p;
  p++;

  /* Count first pass — estimate by commas */
  uint32_t cap = 64;
  ctx->buffer_views = (GltfBufferView *)calloc(cap, sizeof(GltfBufferView));
  ctx->buffer_view_count = 0;

  while (*p && *p != ']') {
    p = gltf_skip_ws(p);
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p != '{')
      break;
    p++;

    if (ctx->buffer_view_count >= cap)
      break;
    GltfBufferView *bv = &ctx->buffer_views[ctx->buffer_view_count++];
    memset(bv, 0, sizeof(*bv));

    while (*p && *p != '}') {
      p = gltf_skip_ws(p);
      if (*p == ',') {
        p++;
        continue;
      }
      if (*p != '"')
        break;
      char key[64] = {0};
      p = gltf_read_string(p, key, sizeof(key));
      p = gltf_skip_ws(p);
      if (*p == ':')
        p++;
      p = gltf_skip_ws(p);

      if (strcmp(key, "buffer") == 0) {
        p = gltf_read_int(p, &bv->buffer);
      } else if (strcmp(key, "byteOffset") == 0) {
        int32_t v;
        p = gltf_read_int(p, &v);
        bv->byte_offset = (uint32_t)v;
      } else if (strcmp(key, "byteLength") == 0) {
        int32_t v;
        p = gltf_read_int(p, &v);
        bv->byte_length = (uint32_t)v;
      } else if (strcmp(key, "byteStride") == 0) {
        int32_t v;
        p = gltf_read_int(p, &v);
        bv->byte_stride = (uint32_t)v;
      } else {
        p = gltf_skip_value(p);
      }
    }
    if (*p == '}')
      p++;
    p = gltf_skip_ws(p);
    if (*p == ',')
      p++;
  }
  if (*p == ']')
    p++;
  return p;
}

/* Parse accessors array */
static const char *parse_accessors(const char *p, GltfParseCtx *ctx) {
  p = gltf_skip_ws(p);
  if (*p != '[')
    return p;
  p++;

  uint32_t cap = 128;
  ctx->accessors = (GltfAccessor *)calloc(cap, sizeof(GltfAccessor));
  ctx->accessor_count = 0;

  while (*p && *p != ']') {
    p = gltf_skip_ws(p);
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p != '{')
      break;
    p++;

    if (ctx->accessor_count >= cap)
      break;
    GltfAccessor *acc = &ctx->accessors[ctx->accessor_count++];
    memset(acc, 0, sizeof(*acc));
    acc->buffer_view = -1;

    while (*p && *p != '}') {
      p = gltf_skip_ws(p);
      if (*p == ',') {
        p++;
        continue;
      }
      if (*p != '"')
        break;
      char key[64] = {0};
      p = gltf_read_string(p, key, sizeof(key));
      p = gltf_skip_ws(p);
      if (*p == ':')
        p++;
      p = gltf_skip_ws(p);

      if (strcmp(key, "bufferView") == 0) {
        p = gltf_read_int(p, &acc->buffer_view);
      } else if (strcmp(key, "byteOffset") == 0) {
        int32_t v;
        p = gltf_read_int(p, &v);
        acc->byte_offset = (uint32_t)v;
      } else if (strcmp(key, "count") == 0) {
        int32_t v;
        p = gltf_read_int(p, &v);
        acc->count = (uint32_t)v;
      } else if (strcmp(key, "componentType") == 0) {
        int32_t v;
        p = gltf_read_int(p, &v);
        acc->component_type = (uint32_t)v;
      } else if (strcmp(key, "type") == 0) {
        char tname[16] = {0};
        p = gltf_read_string(p, tname, sizeof(tname));
        acc->type_count = type_name_to_count(tname);
      } else {
        p = gltf_skip_value(p);
      }
    }
    if (*p == '}')
      p++;
    p = gltf_skip_ws(p);
    if (*p == ',')
      p++;
  }
  if (*p == ']')
    p++;
  return p;
}

/* Parse meshes array */
static const char *parse_meshes(const char *p, GltfParseCtx *ctx) {
  MopGltfScene *scene = ctx->scene;
  p = gltf_skip_ws(p);
  if (*p != '[')
    return p;
  p++;

  uint32_t cap = 32;
  scene->meshes = (MopGltfMesh *)calloc(cap, sizeof(MopGltfMesh));
  scene->mesh_count = 0;

  while (*p && *p != ']') {
    p = gltf_skip_ws(p);
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p != '{')
      break;
    p++;

    if (scene->mesh_count >= cap)
      break;
    MopGltfMesh *mesh = &scene->meshes[scene->mesh_count++];
    memset(mesh, 0, sizeof(*mesh));

    while (*p && *p != '}') {
      p = gltf_skip_ws(p);
      if (*p == ',') {
        p++;
        continue;
      }
      if (*p != '"')
        break;
      char key[64] = {0};
      p = gltf_read_string(p, key, sizeof(key));
      p = gltf_skip_ws(p);
      if (*p == ':')
        p++;
      p = gltf_skip_ws(p);

      if (strcmp(key, "name") == 0) {
        p = gltf_read_string(p, mesh->name, sizeof(mesh->name));
      } else if (strcmp(key, "primitives") == 0 && *p == '[') {
        p++;
        uint32_t prim_cap = 8;
        mesh->primitives =
            (MopGltfPrimitive *)calloc(prim_cap, sizeof(MopGltfPrimitive));
        mesh->primitive_count = 0;

        while (*p && *p != ']') {
          p = gltf_skip_ws(p);
          if (*p == ',') {
            p++;
            continue;
          }
          if (*p != '{')
            break;
          p++;

          if (mesh->primitive_count >= prim_cap) {
            p = gltf_skip_value(p);
            continue;
          }

          MopGltfPrimitive *prim = &mesh->primitives[mesh->primitive_count++];
          memset(prim, 0, sizeof(*prim));
          prim->material_index = -1;

          int32_t pos_acc = -1, norm_acc = -1, uv_acc = -1, idx_acc = -1;
          int32_t joint_acc = -1, weight_acc = -1, tangent_acc = -1;

          while (*p && *p != '}') {
            p = gltf_skip_ws(p);
            if (*p == ',') {
              p++;
              continue;
            }
            if (*p != '"')
              break;
            char pkey[64] = {0};
            p = gltf_read_string(p, pkey, sizeof(pkey));
            p = gltf_skip_ws(p);
            if (*p == ':')
              p++;
            p = gltf_skip_ws(p);

            if (strcmp(pkey, "attributes") == 0 && *p == '{') {
              p++;
              while (*p && *p != '}') {
                p = gltf_skip_ws(p);
                if (*p == ',') {
                  p++;
                  continue;
                }
                if (*p != '"')
                  break;
                char akey[64] = {0};
                p = gltf_read_string(p, akey, sizeof(akey));
                p = gltf_skip_ws(p);
                if (*p == ':')
                  p++;
                p = gltf_skip_ws(p);
                int32_t v;
                p = gltf_read_int(p, &v);

                if (strcmp(akey, "POSITION") == 0)
                  pos_acc = v;
                else if (strcmp(akey, "NORMAL") == 0)
                  norm_acc = v;
                else if (strcmp(akey, "TEXCOORD_0") == 0)
                  uv_acc = v;
                else if (strcmp(akey, "JOINTS_0") == 0)
                  joint_acc = v;
                else if (strcmp(akey, "WEIGHTS_0") == 0)
                  weight_acc = v;
                else if (strcmp(akey, "TANGENT") == 0)
                  tangent_acc = v;

                p = gltf_skip_ws(p);
                if (*p == ',')
                  p++;
              }
              if (*p == '}')
                p++;
            } else if (strcmp(pkey, "indices") == 0) {
              p = gltf_read_int(p, &idx_acc);
            } else if (strcmp(pkey, "material") == 0) {
              p = gltf_read_int(p, &prim->material_index);
            } else {
              p = gltf_skip_value(p);
            }
          }
          if (*p == '}')
            p++;

          /* Resolve position accessor */
          if (pos_acc >= 0) {
            uint32_t count, stride;
            const uint8_t *data =
                resolve_accessor(ctx, (uint32_t)pos_acc, &count, &stride);
            if (data && count > 0) {
              prim->vertex_count = count;
              prim->vertices = (MopVertex *)calloc(count, sizeof(MopVertex));
              for (uint32_t vi = 0; vi < count; vi++) {
                const float *fp = (const float *)(data + vi * stride);
                prim->vertices[vi].position = (MopVec3){fp[0], fp[1], fp[2]};
                /* Default color = white */
                prim->vertices[vi].color = (MopColor){1.0f, 1.0f, 1.0f, 1.0f};
              }
            }
          }

          /* Normal accessor */
          if (norm_acc >= 0 && prim->vertices) {
            uint32_t count, stride;
            const uint8_t *data =
                resolve_accessor(ctx, (uint32_t)norm_acc, &count, &stride);
            if (data) {
              uint32_t n =
                  count < prim->vertex_count ? count : prim->vertex_count;
              for (uint32_t vi = 0; vi < n; vi++) {
                const float *fp = (const float *)(data + vi * stride);
                prim->vertices[vi].normal = (MopVec3){fp[0], fp[1], fp[2]};
              }
            }
          }

          /* UV accessor */
          if (uv_acc >= 0 && prim->vertices) {
            uint32_t count, stride;
            const uint8_t *data =
                resolve_accessor(ctx, (uint32_t)uv_acc, &count, &stride);
            if (data) {
              uint32_t n =
                  count < prim->vertex_count ? count : prim->vertex_count;
              for (uint32_t vi = 0; vi < n; vi++) {
                const float *fp = (const float *)(data + vi * stride);
                prim->vertices[vi].u = fp[0];
                prim->vertices[vi].v = fp[1];
              }
            }
          }

          /* Tangent accessor */
          if (tangent_acc >= 0 && prim->vertices) {
            uint32_t count, stride;
            const uint8_t *data =
                resolve_accessor(ctx, (uint32_t)tangent_acc, &count, &stride);
            if (data) {
              uint32_t n =
                  count < prim->vertex_count ? count : prim->vertex_count;
              prim->tangents = (float *)malloc((size_t)n * 4 * sizeof(float));
              if (prim->tangents) {
                for (uint32_t vi = 0; vi < n; vi++) {
                  const float *fp = (const float *)(data + vi * stride);
                  prim->tangents[vi * 4 + 0] = fp[0];
                  prim->tangents[vi * 4 + 1] = fp[1];
                  prim->tangents[vi * 4 + 2] = fp[2];
                  prim->tangents[vi * 4 + 3] = fp[3];
                }
              }
            }
          }

          /* Index accessor */
          if (idx_acc >= 0) {
            uint32_t count, stride;
            const uint8_t *data =
                resolve_accessor(ctx, (uint32_t)idx_acc, &count, &stride);
            if (data && count > 0) {
              prim->indices =
                  (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
              if (prim->indices) {
                prim->index_count = count;
                const GltfAccessor *acc = &ctx->accessors[idx_acc];
                for (uint32_t ii = 0; ii < count; ii++) {
                  const uint8_t *ep = data + ii * stride;
                  if (acc->component_type == 5123) /* UNSIGNED_SHORT */
                    prim->indices[ii] = *(const uint16_t *)ep;
                  else if (acc->component_type == 5125) /* UNSIGNED_INT */
                    prim->indices[ii] = *(const uint32_t *)ep;
                  else if (acc->component_type == 5121) /* UNSIGNED_BYTE */
                    prim->indices[ii] = ep[0];
                  else
                    prim->indices[ii] = 0;
                }
              }
            }
          }

          /* Joint accessor */
          if (joint_acc >= 0 && prim->vertices) {
            uint32_t count, stride;
            const uint8_t *data =
                resolve_accessor(ctx, (uint32_t)joint_acc, &count, &stride);
            if (data) {
              uint32_t n =
                  count < prim->vertex_count ? count : prim->vertex_count;
              prim->joints = (uint8_t *)malloc((size_t)n * 4);
              if (prim->joints) {
                const GltfAccessor *acc = &ctx->accessors[joint_acc];
                for (uint32_t vi = 0; vi < n; vi++) {
                  const uint8_t *ep = data + vi * stride;
                  if (acc->component_type == 5121) { /* UNSIGNED_BYTE */
                    memcpy(&prim->joints[vi * 4], ep, 4);
                  } else if (acc->component_type == 5123) { /* UNSIGNED_SHORT */
                    const uint16_t *sp = (const uint16_t *)ep;
                    for (int j = 0; j < 4; j++)
                      prim->joints[vi * 4 + j] =
                          (uint8_t)(sp[j] < 256 ? sp[j] : 255);
                  }
                }
              }
            }
          }

          /* Weight accessor */
          if (weight_acc >= 0 && prim->vertices) {
            uint32_t count, stride;
            const uint8_t *data =
                resolve_accessor(ctx, (uint32_t)weight_acc, &count, &stride);
            if (data) {
              uint32_t n =
                  count < prim->vertex_count ? count : prim->vertex_count;
              prim->weights = (float *)malloc((size_t)n * 4 * sizeof(float));
              if (prim->weights) {
                for (uint32_t vi = 0; vi < n; vi++) {
                  const float *fp = (const float *)(data + vi * stride);
                  memcpy(&prim->weights[vi * 4], fp, 4 * sizeof(float));
                }
              }
            }
          }

          /* Bounding box from position min/max */
          if (pos_acc >= 0 && prim->vertices && prim->vertex_count > 0) {
            MopVec3 bmin = prim->vertices[0].position;
            MopVec3 bmax = bmin;
            for (uint32_t vi = 1; vi < prim->vertex_count; vi++) {
              MopVec3 vp = prim->vertices[vi].position;
              if (vp.x < bmin.x)
                bmin.x = vp.x;
              if (vp.y < bmin.y)
                bmin.y = vp.y;
              if (vp.z < bmin.z)
                bmin.z = vp.z;
              if (vp.x > bmax.x)
                bmax.x = vp.x;
              if (vp.y > bmax.y)
                bmax.y = vp.y;
              if (vp.z > bmax.z)
                bmax.z = vp.z;
            }
            prim->bbox_min = bmin;
            prim->bbox_max = bmax;
          }

          p = gltf_skip_ws(p);
          if (*p == ',')
            p++;
        }
        if (*p == ']')
          p++;
      } else {
        p = gltf_skip_value(p);
      }
    }
    if (*p == '}')
      p++;
    p = gltf_skip_ws(p);
    if (*p == ',')
      p++;
  }
  if (*p == ']')
    p++;
  return p;
}

/* Parse materials array */
static const char *parse_materials(const char *p, GltfParseCtx *ctx) {
  MopGltfScene *scene = ctx->scene;
  p = gltf_skip_ws(p);
  if (*p != '[')
    return p;
  p++;

  uint32_t cap = 32;
  scene->materials = (MopGltfMaterial *)calloc(cap, sizeof(MopGltfMaterial));
  scene->material_count = 0;

  while (*p && *p != ']') {
    p = gltf_skip_ws(p);
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p != '{')
      break;
    p++;

    if (scene->material_count >= cap) {
      p = gltf_skip_value(p);
      continue;
    }

    MopGltfMaterial *mat = &scene->materials[scene->material_count++];
    memset(mat, 0, sizeof(*mat));
    mat->base_color[0] = mat->base_color[1] = mat->base_color[2] = 1.0f;
    mat->base_color[3] = 1.0f;
    mat->metallic = 1.0f;
    mat->roughness = 1.0f;
    mat->base_color_tex.image_index = -1;
    mat->normal_tex.image_index = -1;
    mat->mr_tex.image_index = -1;
    mat->occlusion_tex.image_index = -1;
    mat->emissive_tex.image_index = -1;
    mat->alpha_cutoff = 0.5f;

    while (*p && *p != '}') {
      p = gltf_skip_ws(p);
      if (*p == ',') {
        p++;
        continue;
      }
      if (*p != '"')
        break;
      char key[64] = {0};
      p = gltf_read_string(p, key, sizeof(key));
      p = gltf_skip_ws(p);
      if (*p == ':')
        p++;
      p = gltf_skip_ws(p);

      if (strcmp(key, "name") == 0) {
        p = gltf_read_string(p, mat->name, sizeof(mat->name));
      } else if (strcmp(key, "doubleSided") == 0) {
        if (*p == 't') {
          mat->double_sided = true;
          p += 4;
        } else {
          mat->double_sided = false;
          p += 5;
        }
      } else if (strcmp(key, "alphaMode") == 0) {
        char mode[16] = {0};
        p = gltf_read_string(p, mode, sizeof(mode));
        if (strcmp(mode, "MASK") == 0)
          mat->alpha_mode = MOP_GLTF_ALPHA_MASK;
        else if (strcmp(mode, "BLEND") == 0)
          mat->alpha_mode = MOP_GLTF_ALPHA_BLEND;
      } else if (strcmp(key, "alphaCutoff") == 0) {
        p = gltf_read_float(p, &mat->alpha_cutoff);
      } else if (strcmp(key, "emissiveFactor") == 0) {
        p = parse_float_array(p, mat->emissive, 3, NULL);
      } else if (strcmp(key, "pbrMetallicRoughness") == 0 && *p == '{') {
        p++;
        while (*p && *p != '}') {
          p = gltf_skip_ws(p);
          if (*p == ',') {
            p++;
            continue;
          }
          if (*p != '"')
            break;
          char pkey[64] = {0};
          p = gltf_read_string(p, pkey, sizeof(pkey));
          p = gltf_skip_ws(p);
          if (*p == ':')
            p++;
          p = gltf_skip_ws(p);

          if (strcmp(pkey, "baseColorFactor") == 0) {
            p = parse_float_array(p, mat->base_color, 4, NULL);
          } else if (strcmp(pkey, "metallicFactor") == 0) {
            p = gltf_read_float(p, &mat->metallic);
          } else if (strcmp(pkey, "roughnessFactor") == 0) {
            p = gltf_read_float(p, &mat->roughness);
          } else if (strcmp(pkey, "baseColorTexture") == 0 && *p == '{') {
            p++;
            while (*p && *p != '}') {
              p = gltf_skip_ws(p);
              if (*p == ',') {
                p++;
                continue;
              }
              if (*p != '"')
                break;
              char tkey[32] = {0};
              p = gltf_read_string(p, tkey, sizeof(tkey));
              p = gltf_skip_ws(p);
              if (*p == ':')
                p++;
              p = gltf_skip_ws(p);
              if (strcmp(tkey, "index") == 0)
                p = gltf_read_int(p, &mat->base_color_tex.image_index);
              else if (strcmp(tkey, "texCoord") == 0)
                p = gltf_read_int(p, &mat->base_color_tex.tex_coord);
              else
                p = gltf_skip_value(p);
            }
            if (*p == '}')
              p++;
          } else if (strcmp(pkey, "metallicRoughnessTexture") == 0 &&
                     *p == '{') {
            p++;
            while (*p && *p != '}') {
              p = gltf_skip_ws(p);
              if (*p == ',') {
                p++;
                continue;
              }
              if (*p != '"')
                break;
              char tkey[32] = {0};
              p = gltf_read_string(p, tkey, sizeof(tkey));
              p = gltf_skip_ws(p);
              if (*p == ':')
                p++;
              p = gltf_skip_ws(p);
              if (strcmp(tkey, "index") == 0)
                p = gltf_read_int(p, &mat->mr_tex.image_index);
              else if (strcmp(tkey, "texCoord") == 0)
                p = gltf_read_int(p, &mat->mr_tex.tex_coord);
              else
                p = gltf_skip_value(p);
            }
            if (*p == '}')
              p++;
          } else {
            p = gltf_skip_value(p);
          }
        }
        if (*p == '}')
          p++;
      } else if (strcmp(key, "normalTexture") == 0 && *p == '{') {
        p++;
        while (*p && *p != '}') {
          p = gltf_skip_ws(p);
          if (*p == ',') {
            p++;
            continue;
          }
          if (*p != '"')
            break;
          char tkey[32] = {0};
          p = gltf_read_string(p, tkey, sizeof(tkey));
          p = gltf_skip_ws(p);
          if (*p == ':')
            p++;
          p = gltf_skip_ws(p);
          if (strcmp(tkey, "index") == 0)
            p = gltf_read_int(p, &mat->normal_tex.image_index);
          else if (strcmp(tkey, "texCoord") == 0)
            p = gltf_read_int(p, &mat->normal_tex.tex_coord);
          else
            p = gltf_skip_value(p);
        }
        if (*p == '}')
          p++;
      } else if (strcmp(key, "occlusionTexture") == 0 && *p == '{') {
        p++;
        while (*p && *p != '}') {
          p = gltf_skip_ws(p);
          if (*p == ',') {
            p++;
            continue;
          }
          if (*p != '"')
            break;
          char tkey[32] = {0};
          p = gltf_read_string(p, tkey, sizeof(tkey));
          p = gltf_skip_ws(p);
          if (*p == ':')
            p++;
          p = gltf_skip_ws(p);
          if (strcmp(tkey, "index") == 0)
            p = gltf_read_int(p, &mat->occlusion_tex.image_index);
          else if (strcmp(tkey, "texCoord") == 0)
            p = gltf_read_int(p, &mat->occlusion_tex.tex_coord);
          else
            p = gltf_skip_value(p);
        }
        if (*p == '}')
          p++;
      } else if (strcmp(key, "emissiveTexture") == 0 && *p == '{') {
        p++;
        while (*p && *p != '}') {
          p = gltf_skip_ws(p);
          if (*p == ',') {
            p++;
            continue;
          }
          if (*p != '"')
            break;
          char tkey[32] = {0};
          p = gltf_read_string(p, tkey, sizeof(tkey));
          p = gltf_skip_ws(p);
          if (*p == ':')
            p++;
          p = gltf_skip_ws(p);
          if (strcmp(tkey, "index") == 0)
            p = gltf_read_int(p, &mat->emissive_tex.image_index);
          else if (strcmp(tkey, "texCoord") == 0)
            p = gltf_read_int(p, &mat->emissive_tex.tex_coord);
          else
            p = gltf_skip_value(p);
        }
        if (*p == '}')
          p++;
      } else {
        p = gltf_skip_value(p);
      }
    }
    if (*p == '}')
      p++;
    p = gltf_skip_ws(p);
    if (*p == ',')
      p++;
  }
  if (*p == ']')
    p++;
  return p;
}

/* Parse images array */
static const char *parse_images(const char *p, GltfParseCtx *ctx) {
  MopGltfScene *scene = ctx->scene;
  p = gltf_skip_ws(p);
  if (*p != '[')
    return p;
  p++;

  uint32_t cap = 32;
  scene->images = (MopGltfImage *)calloc(cap, sizeof(MopGltfImage));
  scene->image_count = 0;

  while (*p && *p != ']') {
    p = gltf_skip_ws(p);
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p != '{')
      break;
    p++;

    if (scene->image_count >= cap) {
      p = gltf_skip_value(p);
      continue;
    }

    MopGltfImage *img = &scene->images[scene->image_count++];
    memset(img, 0, sizeof(*img));
    int32_t bv_index = -1;

    while (*p && *p != '}') {
      p = gltf_skip_ws(p);
      if (*p == ',') {
        p++;
        continue;
      }
      if (*p != '"')
        break;
      char key[64] = {0};
      p = gltf_read_string(p, key, sizeof(key));
      p = gltf_skip_ws(p);
      if (*p == ':')
        p++;
      p = gltf_skip_ws(p);

      if (strcmp(key, "name") == 0) {
        p = gltf_read_string(p, img->name, sizeof(img->name));
      } else if (strcmp(key, "uri") == 0) {
        p = gltf_read_string(p, img->uri, sizeof(img->uri));
      } else if (strcmp(key, "mimeType") == 0) {
        p = gltf_read_string(p, img->mime_type, sizeof(img->mime_type));
      } else if (strcmp(key, "bufferView") == 0) {
        p = gltf_read_int(p, &bv_index);
      } else {
        p = gltf_skip_value(p);
      }
    }
    if (*p == '}')
      p++;

    /* Resolve embedded image data from buffer view */
    if (bv_index >= 0 && (uint32_t)bv_index < ctx->buffer_view_count) {
      const GltfBufferView *bv = &ctx->buffer_views[bv_index];
      if (bv->byte_offset + bv->byte_length <= ctx->bin_size) {
        img->data = ctx->bin_data + bv->byte_offset;
        img->data_size = bv->byte_length;
      }
    }

    p = gltf_skip_ws(p);
    if (*p == ',')
      p++;
  }
  if (*p == ']')
    p++;
  return p;
}

/* Parse nodes array */
static const char *parse_nodes(const char *p, GltfParseCtx *ctx) {
  MopGltfScene *scene = ctx->scene;
  p = gltf_skip_ws(p);
  if (*p != '[')
    return p;
  p++;

  uint32_t cap = 128;
  scene->nodes = (MopGltfNode *)calloc(cap, sizeof(MopGltfNode));
  scene->node_count = 0;

  while (*p && *p != ']') {
    p = gltf_skip_ws(p);
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p != '{')
      break;
    p++;

    if (scene->node_count >= cap) {
      p = gltf_skip_value(p);
      continue;
    }

    MopGltfNode *node = &scene->nodes[scene->node_count++];
    memset(node, 0, sizeof(*node));
    node->mesh_index = -1;
    node->skin_index = -1;
    node->parent_index = -1;
    node->scale[0] = node->scale[1] = node->scale[2] = 1.0f;
    node->rotation[3] = 1.0f; /* identity quaternion */
    /* Identity matrix */
    node->matrix[0] = node->matrix[5] = node->matrix[10] = node->matrix[15] =
        1.0f;

    while (*p && *p != '}') {
      p = gltf_skip_ws(p);
      if (*p == ',') {
        p++;
        continue;
      }
      if (*p != '"')
        break;
      char key[64] = {0};
      p = gltf_read_string(p, key, sizeof(key));
      p = gltf_skip_ws(p);
      if (*p == ':')
        p++;
      p = gltf_skip_ws(p);

      if (strcmp(key, "name") == 0) {
        p = gltf_read_string(p, node->name, sizeof(node->name));
      } else if (strcmp(key, "mesh") == 0) {
        p = gltf_read_int(p, &node->mesh_index);
      } else if (strcmp(key, "skin") == 0) {
        p = gltf_read_int(p, &node->skin_index);
      } else if (strcmp(key, "translation") == 0) {
        p = parse_float_array(p, node->translation, 3, NULL);
      } else if (strcmp(key, "rotation") == 0) {
        p = parse_float_array(p, node->rotation, 4, NULL);
      } else if (strcmp(key, "scale") == 0) {
        p = parse_float_array(p, node->scale, 3, NULL);
      } else if (strcmp(key, "matrix") == 0) {
        p = parse_float_array(p, node->matrix, 16, NULL);
        node->has_matrix = true;
      } else if (strcmp(key, "children") == 0) {
        /* Count children first by scanning */
        const char *save = p;
        int child_count = 0;
        p = parse_int_array(p, NULL, 0, &child_count);
        if (child_count > 0) {
          node->children =
              (int32_t *)malloc((size_t)child_count * sizeof(int32_t));
          node->child_count = 0;
          p = save;
          p = parse_int_array(p, node->children, child_count,
                              (int *)&node->child_count);
        }
      } else {
        p = gltf_skip_value(p);
      }
    }
    if (*p == '}')
      p++;
    p = gltf_skip_ws(p);
    if (*p == ',')
      p++;
  }
  if (*p == ']')
    p++;

  /* Resolve parent indices from children lists */
  for (uint32_t i = 0; i < scene->node_count; i++) {
    MopGltfNode *nd = &scene->nodes[i];
    for (uint32_t c = 0; c < nd->child_count; c++) {
      int32_t ci = nd->children[c];
      if (ci >= 0 && (uint32_t)ci < scene->node_count)
        scene->nodes[ci].parent_index = (int32_t)i;
    }
  }

  return p;
}

/* Parse skins array */
static const char *parse_skins(const char *p, GltfParseCtx *ctx) {
  MopGltfScene *scene = ctx->scene;
  p = gltf_skip_ws(p);
  if (*p != '[')
    return p;
  p++;

  uint32_t cap = 16;
  scene->skins = (MopGltfSkin *)calloc(cap, sizeof(MopGltfSkin));
  scene->skin_count = 0;

  while (*p && *p != ']') {
    p = gltf_skip_ws(p);
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p != '{')
      break;
    p++;

    if (scene->skin_count >= cap) {
      p = gltf_skip_value(p);
      continue;
    }

    MopGltfSkin *skin = &scene->skins[scene->skin_count++];
    memset(skin, 0, sizeof(*skin));
    skin->skeleton_root = -1;
    int32_t ibm_acc = -1;

    while (*p && *p != '}') {
      p = gltf_skip_ws(p);
      if (*p == ',') {
        p++;
        continue;
      }
      if (*p != '"')
        break;
      char key[64] = {0};
      p = gltf_read_string(p, key, sizeof(key));
      p = gltf_skip_ws(p);
      if (*p == ':')
        p++;
      p = gltf_skip_ws(p);

      if (strcmp(key, "name") == 0) {
        p = gltf_read_string(p, skin->name, sizeof(skin->name));
      } else if (strcmp(key, "skeleton") == 0) {
        p = gltf_read_int(p, &skin->skeleton_root);
      } else if (strcmp(key, "inverseBindMatrices") == 0) {
        p = gltf_read_int(p, &ibm_acc);
      } else if (strcmp(key, "joints") == 0) {
        const char *save = p;
        int jcount = 0;
        p = parse_int_array(p, NULL, 0, &jcount);
        if (jcount > 0) {
          skin->joints = (int32_t *)malloc((size_t)jcount * sizeof(int32_t));
          skin->joint_count = 0;
          p = save;
          p = parse_int_array(p, skin->joints, jcount,
                              (int *)&skin->joint_count);
        }
      } else {
        p = gltf_skip_value(p);
      }
    }
    if (*p == '}')
      p++;

    /* Resolve inverse bind matrices */
    if (ibm_acc >= 0 && skin->joint_count > 0) {
      uint32_t count, stride;
      const uint8_t *data =
          resolve_accessor(ctx, (uint32_t)ibm_acc, &count, &stride);
      if (data && count >= skin->joint_count) {
        skin->inverse_bind_matrices =
            (float *)malloc((size_t)skin->joint_count * 16 * sizeof(float));
        if (skin->inverse_bind_matrices) {
          for (uint32_t j = 0; j < skin->joint_count; j++) {
            memcpy(&skin->inverse_bind_matrices[j * 16], data + j * stride,
                   16 * sizeof(float));
          }
        }
      }
    }

    p = gltf_skip_ws(p);
    if (*p == ',')
      p++;
  }
  if (*p == ']')
    p++;
  return p;
}

/* -------------------------------------------------------------------------
 * Main parse entry
 * ------------------------------------------------------------------------- */

static bool parse_gltf_json(GltfParseCtx *ctx) {
  const char *p = gltf_skip_ws(ctx->json);
  if (*p != '{')
    return false;
  p++;

  while (*p && *p != '}') {
    p = gltf_skip_ws(p);
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p != '"') {
      p++;
      continue;
    }

    char key[64] = {0};
    p = gltf_read_string(p, key, sizeof(key));
    p = gltf_skip_ws(p);
    if (*p == ':')
      p++;
    p = gltf_skip_ws(p);

    if (strcmp(key, "bufferViews") == 0) {
      p = parse_buffer_views(p, ctx);
    } else if (strcmp(key, "accessors") == 0) {
      p = parse_accessors(p, ctx);
    } else if (strcmp(key, "meshes") == 0) {
      p = parse_meshes(p, ctx);
    } else if (strcmp(key, "materials") == 0) {
      p = parse_materials(p, ctx);
    } else if (strcmp(key, "images") == 0) {
      p = parse_images(p, ctx);
    } else if (strcmp(key, "nodes") == 0) {
      p = parse_nodes(p, ctx);
    } else if (strcmp(key, "skins") == 0) {
      p = parse_skins(p, ctx);
    } else {
      p = gltf_skip_value(p);
    }

    p = gltf_skip_ws(p);
    if (*p == ',')
      p++;
  }

  return true;
}

/* -------------------------------------------------------------------------
 * Public API: mop_gltf_load
 * ------------------------------------------------------------------------- */

bool mop_gltf_load(const char *path, MopGltfScene *out) {
  if (!path || !out)
    return false;
  memset(out, 0, sizeof(*out));

  uint32_t file_size;
  uint8_t *file_data = read_file(path, &file_size);
  if (!file_data) {
    MOP_ERROR("mop_gltf_load: failed to read '%s'", path);
    return false;
  }

  GltfParseCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.scene = out;

  /* Detect GLB vs glTF by magic number */
  bool is_glb = (file_size >= 12 && read_u32_le(file_data) == GLB_MAGIC);

  if (is_glb) {
    /* Parse GLB header */
    if (file_size < sizeof(GlbHeader)) {
      MOP_ERROR("mop_gltf_load: GLB too small");
      free(file_data);
      return false;
    }

    uint32_t version = read_u32_le(file_data + 4);
    if (version != GLB_VERSION) {
      MOP_ERROR("mop_gltf_load: unsupported GLB version %u", version);
      free(file_data);
      return false;
    }

    uint32_t offset = 12; /* past header */

    /* First chunk: JSON */
    if (offset + 8 > file_size) {
      free(file_data);
      return false;
    }
    uint32_t json_len = read_u32_le(file_data + offset);
    uint32_t json_type = read_u32_le(file_data + offset + 4);
    offset += 8;

    if (json_type != GLB_CHUNK_JSON || offset + json_len > file_size) {
      MOP_ERROR("mop_gltf_load: invalid JSON chunk");
      free(file_data);
      return false;
    }

    /* Null-terminate JSON */
    out->_json_data = (char *)malloc(json_len + 1);
    memcpy(out->_json_data, file_data + offset, json_len);
    out->_json_data[json_len] = '\0';
    ctx.json = out->_json_data;
    offset += json_len;

    /* Second chunk: BIN (optional) */
    if (offset + 8 <= file_size) {
      uint32_t bin_len = read_u32_le(file_data + offset);
      uint32_t bin_type = read_u32_le(file_data + offset + 4);
      offset += 8;
      if (bin_type == GLB_CHUNK_BIN && offset + bin_len <= file_size) {
        out->_buffer_data = (uint8_t *)malloc(bin_len);
        memcpy(out->_buffer_data, file_data + offset, bin_len);
        out->_buffer_size = bin_len;
        ctx.bin_data = out->_buffer_data;
        ctx.bin_size = bin_len;
      }
    }

    free(file_data);
  } else {
    /* Plain .gltf JSON — file_data IS the JSON */
    out->_json_data = (char *)malloc(file_size + 1);
    memcpy(out->_json_data, file_data, file_size);
    out->_json_data[file_size] = '\0';
    ctx.json = out->_json_data;

    /* Look for companion .bin file */
    size_t plen = strlen(path);
    if (plen > 5) {
      char *bin_path = (char *)malloc(plen + 4);
      memcpy(bin_path, path, plen);
      /* Replace .gltf with .bin */
      const char *dot = strrchr(path, '.');
      if (dot) {
        size_t base = (size_t)(dot - path);
        memcpy(bin_path, path, base);
        memcpy(bin_path + base, ".bin", 5);
        uint32_t bin_size;
        uint8_t *bin_data = read_file(bin_path, &bin_size);
        if (bin_data) {
          out->_buffer_data = bin_data;
          out->_buffer_size = bin_size;
          ctx.bin_data = bin_data;
          ctx.bin_size = bin_size;
        }
      }
      free(bin_path);
    }

    free(file_data);
  }

  /* Two-pass parse: first bufferViews+accessors, then everything else.
   * Our single-pass parser handles this because the glTF spec doesn't
   * mandate order, but in practice the important arrays come first. */
  bool ok = parse_gltf_json(&ctx);

  free(ctx.accessors);
  free(ctx.buffer_views);

  if (!ok) {
    mop_gltf_free(out);
    return false;
  }

  MOP_INFO("mop_gltf_load: loaded '%s' (%u meshes, %u materials, %u nodes)",
           path, out->mesh_count, out->material_count, out->node_count);
  return true;
}

/* -------------------------------------------------------------------------
 * Public API: mop_gltf_free
 * ------------------------------------------------------------------------- */

void mop_gltf_free(MopGltfScene *scene) {
  if (!scene)
    return;

  /* Free mesh primitives */
  for (uint32_t i = 0; i < scene->mesh_count; i++) {
    MopGltfMesh *mesh = &scene->meshes[i];
    for (uint32_t p = 0; p < mesh->primitive_count; p++) {
      MopGltfPrimitive *prim = &mesh->primitives[p];
      free(prim->vertices);
      free(prim->indices);
      free(prim->tangents);
      free(prim->joints);
      free(prim->weights);
    }
    free(mesh->primitives);
  }
  free(scene->meshes);
  free(scene->materials);
  free(scene->images);

  /* Free nodes (children arrays) */
  for (uint32_t i = 0; i < scene->node_count; i++)
    free(scene->nodes[i].children);
  free(scene->nodes);

  /* Free skins */
  for (uint32_t i = 0; i < scene->skin_count; i++) {
    free(scene->skins[i].joints);
    free(scene->skins[i].inverse_bind_matrices);
  }
  free(scene->skins);

  /* Free buffers */
  free(scene->_buffer_data);
  free(scene->_json_data);

  memset(scene, 0, sizeof(*scene));
}

/* -------------------------------------------------------------------------
 * Quaternion to euler (ZYX intrinsic = XYZ extrinsic)
 *
 * glTF stores rotations as quaternions (x,y,z,w).
 * MOP uses euler angles in radians (Rz * Ry * Rx order).
 * ------------------------------------------------------------------------- */

static MopVec3 quat_to_euler(const float q[4]) {
  float x = q[0], y = q[1], z = q[2], w = q[3];
  MopVec3 e;

  /* Roll (X) */
  float sinr = 2.0f * (w * x + y * z);
  float cosr = 1.0f - 2.0f * (x * x + y * y);
  e.x = atan2f(sinr, cosr);

  /* Pitch (Y) — clamp to avoid NaN from asinf */
  float sinp = 2.0f * (w * y - z * x);
  if (sinp >= 1.0f)
    e.y = (float)(M_PI / 2.0);
  else if (sinp <= -1.0f)
    e.y = (float)(-M_PI / 2.0);
  else
    e.y = asinf(sinp);

  /* Yaw (Z) */
  float siny = 2.0f * (w * z + x * y);
  float cosy = 1.0f - 2.0f * (y * y + z * z);
  e.z = atan2f(siny, cosy);

  return e;
}

/* -------------------------------------------------------------------------
 * Load a glTF image into a MopTexture via the texture pipeline
 * ------------------------------------------------------------------------- */

static MopTexture *load_gltf_image(const MopGltfImage *img,
                                   MopViewport *viewport) {
  if (!img)
    return NULL;

  /* Try embedded data first (GLB or data URI) */
  if (img->data && img->data_size > 0) {
    int w, h, channels;
    uint8_t *pixels = stbi_load_from_memory(img->data, (int)img->data_size, &w,
                                            &h, &channels, 4);
    if (!pixels) {
      MOP_WARN("gltf_import: failed to decode image '%s'", img->name);
      return NULL;
    }
    MopTextureDesc desc = {
        .width = w,
        .height = h,
        .format = MOP_TEX_FORMAT_RGBA8,
        .data = pixels,
        .data_size = (uint32_t)((size_t)w * (size_t)h * 4),
        .srgb = true,
    };
    MopTexture *tex = mop_tex_create(viewport, &desc);
    stbi_image_free(pixels);
    return tex;
  }

  /* Try external file path */
  if (img->uri[0])
    return mop_tex_load_async(viewport, img->uri);

  return NULL;
}

/* -------------------------------------------------------------------------
 * Convert a MopGltfMaterial to a MopMaterial
 * ------------------------------------------------------------------------- */

static MopMaterial gltf_material_to_mop(const MopGltfMaterial *gmat,
                                        const MopGltfScene *scene,
                                        MopViewport *viewport) {
  MopMaterial mat = mop_material_default();
  mat.base_color = (MopColor){
      .r = gmat->base_color[0],
      .g = gmat->base_color[1],
      .b = gmat->base_color[2],
      .a = gmat->base_color[3],
  };
  mat.metallic = gmat->metallic;
  mat.roughness = gmat->roughness;
  mat.emissive = (MopVec3){
      .x = gmat->emissive[0],
      .y = gmat->emissive[1],
      .z = gmat->emissive[2],
  };

  /* Load texture maps */
  if (gmat->base_color_tex.image_index >= 0 &&
      (uint32_t)gmat->base_color_tex.image_index < scene->image_count) {
    mat.albedo_map = load_gltf_image(
        &scene->images[gmat->base_color_tex.image_index], viewport);
  }
  if (gmat->normal_tex.image_index >= 0 &&
      (uint32_t)gmat->normal_tex.image_index < scene->image_count) {
    mat.normal_map =
        load_gltf_image(&scene->images[gmat->normal_tex.image_index], viewport);
  }
  if (gmat->mr_tex.image_index >= 0 &&
      (uint32_t)gmat->mr_tex.image_index < scene->image_count) {
    mat.metallic_roughness_map =
        load_gltf_image(&scene->images[gmat->mr_tex.image_index], viewport);
  }
  if (gmat->occlusion_tex.image_index >= 0 &&
      (uint32_t)gmat->occlusion_tex.image_index < scene->image_count) {
    mat.ao_map = load_gltf_image(
        &scene->images[gmat->occlusion_tex.image_index], viewport);
  }

  return mat;
}

/* -------------------------------------------------------------------------
 * Public API: mop_gltf_import
 *
 * Creates MOP meshes, textures, and materials from a parsed glTF scene.
 * Walks the node tree to establish parent-child relationships and TRS.
 * Returns the number of mesh primitives created.
 * ------------------------------------------------------------------------- */

uint32_t mop_gltf_import(const MopGltfScene *scene, MopViewport *viewport,
                         uint32_t base_object_id) {
  if (!scene || !viewport)
    return 0;

  uint32_t meshes_created = 0;
  uint32_t next_id = base_object_id;

  /* Pre-convert materials */
  MopMaterial *materials = NULL;
  if (scene->material_count > 0) {
    materials = calloc(scene->material_count, sizeof(MopMaterial));
    if (materials) {
      for (uint32_t i = 0; i < scene->material_count; i++)
        materials[i] =
            gltf_material_to_mop(&scene->materials[i], scene, viewport);
    }
  }

  /* Map from (glTF mesh index, primitive index) to MopMesh*.
   * We flatten all primitives across all glTF meshes into a single array
   * so nodes can reference them by glTF mesh index. */
  typedef struct {
    uint32_t gltf_mesh_idx;
    uint32_t prim_idx;
    MopMesh *mop_mesh;
  } PrimEntry;

  /* Count total primitives */
  uint32_t total_prims = 0;
  for (uint32_t i = 0; i < scene->mesh_count; i++)
    total_prims += scene->meshes[i].primitive_count;

  PrimEntry *prim_map = NULL;
  if (total_prims > 0)
    prim_map = calloc(total_prims, sizeof(PrimEntry));

  /* Create all mesh primitives */
  uint32_t prim_write = 0;
  for (uint32_t mi = 0; mi < scene->mesh_count; mi++) {
    const MopGltfMesh *gmesh = &scene->meshes[mi];
    for (uint32_t pi = 0; pi < gmesh->primitive_count; pi++) {
      const MopGltfPrimitive *prim = &gmesh->primitives[pi];
      if (!prim->vertices || prim->vertex_count == 0)
        continue;

      MopMeshDesc desc = {
          .vertices = prim->vertices,
          .vertex_count = prim->vertex_count,
          .indices = prim->indices,
          .index_count = prim->index_count,
          .object_id = next_id++,
      };

      MopMesh *mesh = mop_viewport_add_mesh(viewport, &desc);
      if (!mesh)
        continue;

      /* Apply material */
      if (prim->material_index >= 0 && materials &&
          (uint32_t)prim->material_index < scene->material_count) {
        mop_mesh_set_material(mesh, &materials[prim->material_index]);

        /* Set albedo texture directly on mesh for the rasterizer */
        if (materials[prim->material_index].albedo_map)
          mop_mesh_set_texture(mesh,
                               materials[prim->material_index].albedo_map);
      }

      /* Record in prim_map */
      if (prim_map && prim_write < total_prims) {
        prim_map[prim_write].gltf_mesh_idx = mi;
        prim_map[prim_write].prim_idx = pi;
        prim_map[prim_write].mop_mesh = mesh;
        prim_write++;
      }

      meshes_created++;
    }
  }

  /* Apply node transforms and hierarchy */
  /* First pass: map glTF node → first MopMesh for that node's mesh */
  MopMesh **node_meshes = NULL;
  if (scene->node_count > 0) {
    node_meshes = calloc(scene->node_count, sizeof(MopMesh *));
    if (node_meshes) {
      for (uint32_t ni = 0; ni < scene->node_count; ni++) {
        const MopGltfNode *node = &scene->nodes[ni];
        if (node->mesh_index < 0)
          continue;

        /* Find the first primitive of this mesh in prim_map */
        for (uint32_t p = 0; p < prim_write; p++) {
          if (prim_map[p].gltf_mesh_idx == (uint32_t)node->mesh_index) {
            node_meshes[ni] = prim_map[p].mop_mesh;
            break;
          }
        }
      }

      /* Apply TRS to each node's mesh */
      for (uint32_t ni = 0; ni < scene->node_count; ni++) {
        MopMesh *mesh = node_meshes[ni];
        if (!mesh)
          continue;

        const MopGltfNode *node = &scene->nodes[ni];

        if (node->has_matrix) {
          /* Direct matrix — copy as column-major MopMat4 */
          MopMat4 m;
          memcpy(m.d, node->matrix, sizeof(float) * 16);
          mop_mesh_set_transform(mesh, &m);
        } else {
          /* TRS decomposition */
          MopVec3 pos = {node->translation[0], node->translation[1],
                         node->translation[2]};
          MopVec3 rot = quat_to_euler(node->rotation);
          MopVec3 scl = {node->scale[0], node->scale[1], node->scale[2]};
          mop_mesh_set_position(mesh, pos);
          mop_mesh_set_rotation(mesh, rot);
          mop_mesh_set_scale(mesh, scl);
        }
      }

      /* Apply parent-child hierarchy */
      for (uint32_t ni = 0; ni < scene->node_count; ni++) {
        const MopGltfNode *node = &scene->nodes[ni];
        MopMesh *child_mesh = node_meshes[ni];
        if (!child_mesh || node->parent_index < 0)
          continue;
        if ((uint32_t)node->parent_index >= scene->node_count)
          continue;

        MopMesh *parent_mesh = node_meshes[node->parent_index];
        if (parent_mesh)
          mop_mesh_set_parent(child_mesh, parent_mesh, viewport);
      }
    }
  }

  free(node_meshes);
  free(prim_map);
  free(materials);

  if (meshes_created > 0)
    MOP_INFO("gltf_import: created %u meshes from %u glTF nodes",
             meshes_created, scene->node_count);

  return meshes_created;
}
