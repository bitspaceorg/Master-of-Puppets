/*
 * Master of Puppets — CPU Backend
 * cpu_backend.c — RHI implementation using software rasterization
 *
 * This backend renders entirely on the CPU.  It implements the full RHI
 * contract using the shared software rasterizer for triangle rasterization.
 *
 * Resources (buffers, framebuffers) are plain heap allocations.
 * No GPU or driver interaction occurs.  Always available on all platforms.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_mt.h"
#include "rhi/rhi.h"

#include <mop/core/vertex_format.h>
#include <mop/util/log.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal types
 * ------------------------------------------------------------------------- */

struct MopRhiDevice {
  MopSwThreadPool *threadpool; /* tile-based parallel rasterizer */
};

struct MopRhiBuffer {
  void *data;
  size_t size;
};

struct MopRhiFramebuffer {
  MopSwFramebuffer fb;
  uint8_t *readback; /* RGBA8 readback copy (same as fb.color) */
};

struct MopRhiTexture {
  int width;
  int height;
  uint8_t *data;   /* RGBA8, row-major (NULL for HDR-only textures) */
  float *hdr_data; /* RGBA float, row-major (NULL for LDR textures) */
  bool is_hdr;
};

/* -------------------------------------------------------------------------
 * Device lifecycle
 * ------------------------------------------------------------------------- */

static MopRhiDevice *cpu_device_create(void) {
  MopRhiDevice *dev = calloc(1, sizeof(MopRhiDevice));
  if (!dev)
    return NULL;

  /* Detect core count and create thread pool */
  int num_cores = 1;
#if defined(_SC_NPROCESSORS_ONLN)
  {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 1)
      num_cores = (int)n;
  }
#endif
  /* Use at most (cores - 1) worker threads (main thread also participates) */
  int workers = num_cores > 1 ? num_cores - 1 : 1;
  dev->threadpool = mop_sw_threadpool_create(workers);
  /* threadpool creation failure is non-fatal — falls back to single-threaded */

  return dev;
}

static void cpu_device_destroy(MopRhiDevice *device) {
  if (!device)
    return;
  if (device->threadpool) {
    mop_sw_threadpool_destroy(device->threadpool);
  }
  free(device);
}

/* -------------------------------------------------------------------------
 * Buffer management
 *
 * CPU buffers simply store a copy of the application data in heap memory.
 * The rasterizer reads directly from these buffers during draw calls.
 * ------------------------------------------------------------------------- */

static MopRhiBuffer *cpu_buffer_create(MopRhiDevice *device,
                                       const MopRhiBufferDesc *desc) {
  (void)device;

  MopRhiBuffer *buf = malloc(sizeof(MopRhiBuffer));
  if (!buf)
    return NULL;

  buf->data = malloc(desc->size);
  if (!buf->data) {
    free(buf);
    return NULL;
  }

  memcpy(buf->data, desc->data, desc->size);
  buf->size = desc->size;
  return buf;
}

static void cpu_buffer_destroy(MopRhiDevice *device, MopRhiBuffer *buffer) {
  (void)device;
  if (!buffer)
    return;
  free(buffer->data);
  free(buffer);
}

/* -------------------------------------------------------------------------
 * Framebuffer management
 * ------------------------------------------------------------------------- */

static MopRhiFramebuffer *
cpu_framebuffer_create(MopRhiDevice *device,
                       const MopRhiFramebufferDesc *desc) {
  (void)device;

  MopRhiFramebuffer *fb = calloc(1, sizeof(MopRhiFramebuffer));
  if (!fb)
    return NULL;

  if (!mop_sw_framebuffer_alloc(&fb->fb, desc->width, desc->height)) {
    free(fb);
    return NULL;
  }

  fb->readback = fb->fb.color; /* Point directly to the color buffer */
  return fb;
}

static void cpu_framebuffer_destroy(MopRhiDevice *device,
                                    MopRhiFramebuffer *fb) {
  (void)device;
  if (!fb)
    return;
  mop_sw_framebuffer_free(&fb->fb);
  free(fb);
}

static void cpu_framebuffer_resize(MopRhiDevice *device, MopRhiFramebuffer *fb,
                                   int width, int height) {
  (void)device;
  if (!fb)
    return;

  mop_sw_framebuffer_free(&fb->fb);
  mop_sw_framebuffer_alloc(&fb->fb, width, height);
  fb->readback = fb->fb.color;
}

/* -------------------------------------------------------------------------
 * Frame commands
 * ------------------------------------------------------------------------- */

static void cpu_frame_begin(MopRhiDevice *device, MopRhiFramebuffer *fb,
                            MopColor clear_color) {
  (void)device;
  mop_sw_framebuffer_clear(&fb->fb, clear_color);
}

static void cpu_frame_end(MopRhiDevice *device, MopRhiFramebuffer *fb) {
  (void)device;
  (void)fb;
}

/* -------------------------------------------------------------------------
 * Draw call
 *
 * Reads vertex and index data from CPU buffers, applies the MVP transform,
 * and feeds each triangle to the rasterizer.
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Transform and prepare a single triangle from a draw call
 * ------------------------------------------------------------------------- */

static bool cpu_prepare_triangle(const MopRhiDrawCall *call,
                                 const MopVertex *v0, const MopVertex *v1,
                                 const MopVertex *v2, MopSwPreparedTri *out) {
  /* Transform vertices to clip space */
  MopVec4 pos0 = {v0->position.x, v0->position.y, v0->position.z, 1.0f};
  MopVec4 pos1 = {v1->position.x, v1->position.y, v1->position.z, 1.0f};
  MopVec4 pos2 = {v2->position.x, v2->position.y, v2->position.z, 1.0f};

  out->vertices[0].position = mop_mat4_mul_vec4(call->mvp, pos0);
  out->vertices[1].position = mop_mat4_mul_vec4(call->mvp, pos1);
  out->vertices[2].position = mop_mat4_mul_vec4(call->mvp, pos2);

  /* Apply depth bias — push clip-space Z slightly farther to avoid Z-fighting
   * with coplanar scene geometry (e.g. grid vs objects at Y=0). */
  if (call->depth_bias != 0.0f) {
    for (int k = 0; k < 3; k++)
      out->vertices[k].position.z +=
          call->depth_bias * out->vertices[k].position.w;
  }

  /* World-space positions for point/spot light attenuation */
  MopVec4 wp0 = mop_mat4_mul_vec4(call->model, pos0);
  MopVec4 wp1 = mop_mat4_mul_vec4(call->model, pos1);
  MopVec4 wp2 = mop_mat4_mul_vec4(call->model, pos2);
  out->vertices[0].world_pos = (MopVec3){wp0.x, wp0.y, wp0.z};
  out->vertices[1].world_pos = (MopVec3){wp1.x, wp1.y, wp1.z};
  out->vertices[2].world_pos = (MopVec3){wp2.x, wp2.y, wp2.z};

  /* Transform normals by model matrix (upper 3x3) */
  MopVec4 n0 = {v0->normal.x, v0->normal.y, v0->normal.z, 0.0f};
  MopVec4 n1 = {v1->normal.x, v1->normal.y, v1->normal.z, 0.0f};
  MopVec4 n2 = {v2->normal.x, v2->normal.y, v2->normal.z, 0.0f};

  MopVec4 tn0 = mop_mat4_mul_vec4(call->model, n0);
  MopVec4 tn1 = mop_mat4_mul_vec4(call->model, n1);
  MopVec4 tn2 = mop_mat4_mul_vec4(call->model, n2);

  out->vertices[0].normal = (MopVec3){tn0.x, tn0.y, tn0.z};
  out->vertices[1].normal = (MopVec3){tn1.x, tn1.y, tn1.z};
  out->vertices[2].normal = (MopVec3){tn2.x, tn2.y, tn2.z};

  out->vertices[0].color = v0->color;
  out->vertices[1].color = v1->color;
  out->vertices[2].color = v2->color;

  out->vertices[0].u = v0->u;
  out->vertices[0].v = v0->v;
  out->vertices[1].u = v1->u;
  out->vertices[1].v = v1->v;
  out->vertices[2].u = v2->u;
  out->vertices[2].v = v2->v;

  /* Nearest-neighbor texture sampling — modulate vertex color */
  if (call->texture && call->texture->width >= 1 &&
      call->texture->height >= 1) {
    MopRhiTexture *tex = call->texture;
    for (int t = 0; t < 3; t++) {
      float tu = out->vertices[t].u - floorf(out->vertices[t].u);
      float tv = out->vertices[t].v - floorf(out->vertices[t].v);
      int tx = (int)(tu * (float)(tex->width - 1) + 0.5f);
      int ty = (int)(tv * (float)(tex->height - 1) + 0.5f);
      if (tx < 0)
        tx = 0;
      if (tx >= tex->width)
        tx = tex->width - 1;
      if (ty < 0)
        ty = 0;
      if (ty >= tex->height)
        ty = tex->height - 1;
      size_t tidx = ((size_t)ty * (size_t)tex->width + (size_t)tx) * 4;
      float tr_f = (float)tex->data[tidx + 0] / 255.0f;
      float tg_f = (float)tex->data[tidx + 1] / 255.0f;
      float tb_f = (float)tex->data[tidx + 2] / 255.0f;
      float ta_f = (float)tex->data[tidx + 3] / 255.0f;
      out->vertices[t].color.r *= tr_f;
      out->vertices[t].color.g *= tg_f;
      out->vertices[t].color.b *= tb_f;
      out->vertices[t].color.a *= ta_f;
    }
  }

  out->object_id = call->object_id;
  out->wireframe = call->wireframe;
  out->depth_test = call->depth_test;
  out->depth_write = call->depth_write;
  out->cull_back = call->backface_cull;
  out->light_dir = call->light_dir;
  out->ambient = call->ambient;
  out->opacity = call->opacity;
  out->smooth_shading = (call->shading_mode == MOP_SHADING_SMOOTH);
  out->blend_mode = call->blend_mode;
  out->lights = call->lights;
  out->light_count = call->light_count;
  out->cam_eye = call->cam_eye;
  out->metallic = call->metallic;
  out->roughness = call->roughness;
  out->line_width = call->line_width;
  out->depth_bias = call->depth_bias;

  return true;
}

/* -------------------------------------------------------------------------
 * Flexible vertex format: read vertex attributes by offset from raw bytes.
 * Falls back to defaults for missing attributes.
 * ------------------------------------------------------------------------- */

static void read_attrib_float3(const uint8_t *base, uint32_t offset,
                               MopAttribFormat fmt, float out[3]) {
  const float *f = (const float *)(base + offset);
  switch (fmt) {
  case MOP_FORMAT_FLOAT3:
  case MOP_FORMAT_FLOAT4:
    out[0] = f[0];
    out[1] = f[1];
    out[2] = f[2];
    break;
  case MOP_FORMAT_FLOAT2:
    out[0] = f[0];
    out[1] = f[1];
    out[2] = 0.0f;
    break;
  case MOP_FORMAT_FLOAT:
    out[0] = f[0];
    out[1] = 0.0f;
    out[2] = 0.0f;
    break;
  default:
    out[0] = out[1] = out[2] = 0.0f;
    break;
  }
}

static void read_attrib_float4(const uint8_t *base, uint32_t offset,
                               MopAttribFormat fmt, float out[4]) {
  const float *f = (const float *)(base + offset);
  switch (fmt) {
  case MOP_FORMAT_FLOAT4:
    out[0] = f[0];
    out[1] = f[1];
    out[2] = f[2];
    out[3] = f[3];
    break;
  case MOP_FORMAT_FLOAT3:
    out[0] = f[0];
    out[1] = f[1];
    out[2] = f[2];
    out[3] = 1.0f;
    break;
  case MOP_FORMAT_FLOAT2:
    out[0] = f[0];
    out[1] = f[1];
    out[2] = 0.0f;
    out[3] = 1.0f;
    break;
  case MOP_FORMAT_FLOAT:
    out[0] = f[0];
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
    break;
  default:
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    break;
  }
}

static void read_attrib_float2(const uint8_t *base, uint32_t offset,
                               MopAttribFormat fmt, float out[2]) {
  const float *f = (const float *)(base + offset);
  switch (fmt) {
  case MOP_FORMAT_FLOAT2:
  case MOP_FORMAT_FLOAT3:
  case MOP_FORMAT_FLOAT4:
    out[0] = f[0];
    out[1] = f[1];
    break;
  case MOP_FORMAT_FLOAT:
    out[0] = f[0];
    out[1] = 0.0f;
    break;
  default:
    out[0] = out[1] = 0.0f;
    break;
  }
}

static bool cpu_prepare_triangle_ex(const MopRhiDrawCall *call,
                                    const uint8_t *raw, uint32_t stride,
                                    uint32_t i0, uint32_t i1, uint32_t i2,
                                    MopSwPreparedTri *out) {
  const MopVertexFormat *fmt = call->vertex_format;
  const uint8_t *v[3] = {raw + (size_t)i0 * stride, raw + (size_t)i1 * stride,
                         raw + (size_t)i2 * stride};

  const MopVertexAttrib *pos_attr =
      mop_vertex_format_find(fmt, MOP_ATTRIB_POSITION);
  if (!pos_attr)
    return false;

  const MopVertexAttrib *nrm_attr =
      mop_vertex_format_find(fmt, MOP_ATTRIB_NORMAL);
  const MopVertexAttrib *col_attr =
      mop_vertex_format_find(fmt, MOP_ATTRIB_COLOR);
  const MopVertexAttrib *uv_attr =
      mop_vertex_format_find(fmt, MOP_ATTRIB_TEXCOORD0);

  for (int t = 0; t < 3; t++) {
    /* Position → clip space + world space */
    float pos[3];
    read_attrib_float3(v[t], pos_attr->offset, pos_attr->format, pos);
    MopVec4 p = {pos[0], pos[1], pos[2], 1.0f};
    out->vertices[t].position = mop_mat4_mul_vec4(call->mvp, p);
    if (call->depth_bias != 0.0f)
      out->vertices[t].position.z +=
          call->depth_bias * out->vertices[t].position.w;
    MopVec4 wp = mop_mat4_mul_vec4(call->model, p);
    out->vertices[t].world_pos = (MopVec3){wp.x, wp.y, wp.z};

    /* Normal → world space */
    if (nrm_attr) {
      float n[3];
      read_attrib_float3(v[t], nrm_attr->offset, nrm_attr->format, n);
      MopVec4 nv = {n[0], n[1], n[2], 0.0f};
      MopVec4 tn = mop_mat4_mul_vec4(call->model, nv);
      out->vertices[t].normal = (MopVec3){tn.x, tn.y, tn.z};
    } else {
      out->vertices[t].normal = (MopVec3){0.0f, 1.0f, 0.0f};
    }

    /* Color */
    if (col_attr) {
      float c[4];
      read_attrib_float4(v[t], col_attr->offset, col_attr->format, c);
      out->vertices[t].color = (MopColor){c[0], c[1], c[2], c[3]};
    } else {
      out->vertices[t].color = (MopColor){1.0f, 1.0f, 1.0f, 1.0f};
    }

    /* UV */
    if (uv_attr) {
      float uv[2];
      read_attrib_float2(v[t], uv_attr->offset, uv_attr->format, uv);
      out->vertices[t].u = uv[0];
      out->vertices[t].v = uv[1];
    } else {
      out->vertices[t].u = 0.0f;
      out->vertices[t].v = 0.0f;
    }

    out->vertices[t].tangent = (MopVec3){0, 0, 0};
  }

  /* Texture sampling (same as standard path) */
  if (call->texture && call->texture->width >= 1 &&
      call->texture->height >= 1) {
    MopRhiTexture *tex = call->texture;
    for (int t = 0; t < 3; t++) {
      float tu = out->vertices[t].u - floorf(out->vertices[t].u);
      float tv = out->vertices[t].v - floorf(out->vertices[t].v);
      int tx = (int)(tu * (float)(tex->width - 1) + 0.5f);
      int ty = (int)(tv * (float)(tex->height - 1) + 0.5f);
      if (tx < 0)
        tx = 0;
      if (tx >= tex->width)
        tx = tex->width - 1;
      if (ty < 0)
        ty = 0;
      if (ty >= tex->height)
        ty = tex->height - 1;
      size_t tidx = ((size_t)ty * (size_t)tex->width + (size_t)tx) * 4;
      float tr_f = (float)tex->data[tidx + 0] / 255.0f;
      float tg_f = (float)tex->data[tidx + 1] / 255.0f;
      float tb_f = (float)tex->data[tidx + 2] / 255.0f;
      float ta_f = (float)tex->data[tidx + 3] / 255.0f;
      out->vertices[t].color.r *= tr_f;
      out->vertices[t].color.g *= tg_f;
      out->vertices[t].color.b *= tb_f;
      out->vertices[t].color.a *= ta_f;
    }
  }

  out->object_id = call->object_id;
  out->wireframe = call->wireframe;
  out->depth_test = call->depth_test;
  out->depth_write = call->depth_write;
  out->cull_back = call->backface_cull;
  out->light_dir = call->light_dir;
  out->ambient = call->ambient;
  out->opacity = call->opacity;
  out->smooth_shading = (call->shading_mode == MOP_SHADING_SMOOTH);
  out->blend_mode = call->blend_mode;
  out->lights = call->lights;
  out->light_count = call->light_count;
  out->cam_eye = call->cam_eye;
  out->metallic = call->metallic;
  out->roughness = call->roughness;
  out->line_width = call->line_width;
  out->depth_bias = call->depth_bias;

  return true;
}

static void cpu_draw(MopRhiDevice *device, MopRhiFramebuffer *fb,
                     const MopRhiDrawCall *call) {
  const uint8_t *raw_data = (const uint8_t *)call->vertex_buffer->data;
  const MopVertex *vertices = (const MopVertex *)raw_data;
  const uint32_t *indices = (const uint32_t *)call->index_buffer->data;
  uint32_t tri_count = call->index_count / 3;
  bool use_flex = (call->vertex_format != NULL);

  /* depth_write=false: save depth buffer, render, restore (read-only depth) */
  float *saved_depth = NULL;
  if (call->depth_test && !call->depth_write) {
    size_t depth_size =
        (size_t)fb->fb.width * (size_t)fb->fb.height * sizeof(float);
    saved_depth = malloc(depth_size);
    if (saved_depth)
      memcpy(saved_depth, fb->fb.depth, depth_size);
  }

  /* Use tiled path if threadpool exists and enough triangles */
  if (device->threadpool && tri_count > 100) {
    /* Prepare all triangles */
    MopSwPreparedTri *prepared = malloc(tri_count * sizeof(MopSwPreparedTri));
    if (prepared) {
      uint32_t prepared_count = 0;
      for (uint32_t i = 0; i + 2 < call->index_count; i += 3) {
        uint32_t i0 = indices[i + 0];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];

        if (i0 >= call->vertex_count || i1 >= call->vertex_count ||
            i2 >= call->vertex_count) {
          continue;
        }

        if (use_flex) {
          cpu_prepare_triangle_ex(call, raw_data, call->vertex_format->stride,
                                  i0, i1, i2, &prepared[prepared_count]);
        } else {
          cpu_prepare_triangle(call, &vertices[i0], &vertices[i1],
                               &vertices[i2], &prepared[prepared_count]);
        }
        prepared_count++;
      }

      mop_sw_rasterize_tiled(device->threadpool, prepared, prepared_count,
                             &fb->fb);
      free(prepared);
      if (saved_depth) {
        size_t depth_size =
            (size_t)fb->fb.width * (size_t)fb->fb.height * sizeof(float);
        memcpy(fb->fb.depth, saved_depth, depth_size);
        free(saved_depth);
      }
      return;
    }
    /* malloc failed — fall through to single-threaded path */
    MOP_WARN(
        "tiled rasterizer allocation failed, falling back to single-threaded");
  }

  /* Single-threaded fallback */
  for (uint32_t i = 0; i + 2 < call->index_count; i += 3) {
    uint32_t i0 = indices[i + 0];
    uint32_t i1 = indices[i + 1];
    uint32_t i2 = indices[i + 2];

    if (i0 >= call->vertex_count || i1 >= call->vertex_count ||
        i2 >= call->vertex_count) {
      continue;
    }

    MopSwPreparedTri tri;
    if (use_flex) {
      cpu_prepare_triangle_ex(call, raw_data, call->vertex_format->stride, i0,
                              i1, i2, &tri);
    } else {
      cpu_prepare_triangle(call, &vertices[i0], &vertices[i1], &vertices[i2],
                           &tri);
    }

    if (tri.lights && tri.light_count > 0) {
      mop_sw_rasterize_triangle_full(
          tri.vertices, tri.object_id, tri.wireframe, tri.depth_test,
          tri.cull_back, tri.light_dir, tri.ambient, tri.opacity,
          tri.smooth_shading, tri.blend_mode, tri.lights, tri.light_count,
          tri.cam_eye, tri.metallic, tri.roughness, &fb->fb);
    } else {
      mop_sw_rasterize_triangle(tri.vertices, tri.object_id, tri.wireframe,
                                tri.depth_test, tri.cull_back, tri.light_dir,
                                tri.ambient, tri.opacity, tri.smooth_shading,
                                tri.blend_mode, &fb->fb);
    }
  }

  /* Restore depth buffer if read-only depth test was requested */
  if (saved_depth) {
    size_t depth_size =
        (size_t)fb->fb.width * (size_t)fb->fb.height * sizeof(float);
    memcpy(fb->fb.depth, saved_depth, depth_size);
    free(saved_depth);
  }
}

/* -------------------------------------------------------------------------
 * Instanced draw call (Phase 6B)
 *
 * Loops over instance transforms, overrides model/mvp per instance,
 * and calls the existing draw logic.
 * ------------------------------------------------------------------------- */

static void cpu_draw_instanced(MopRhiDevice *device, MopRhiFramebuffer *fb,
                               const MopRhiDrawCall *call,
                               const MopMat4 *instance_transforms,
                               uint32_t instance_count) {
  if (!call || !instance_transforms || instance_count == 0)
    return;

  for (uint32_t inst = 0; inst < instance_count; inst++) {
    /* Build a per-instance draw call with overridden model/mvp */
    MopRhiDrawCall inst_call = *call;
    inst_call.model = instance_transforms[inst];

    /* Recompute MVP: projection * view * instance_model */
    MopMat4 view_model =
        mop_mat4_multiply(call->view, instance_transforms[inst]);
    inst_call.mvp = mop_mat4_multiply(call->projection, view_model);

    cpu_draw(device, fb, &inst_call);
  }
}

/* -------------------------------------------------------------------------
 * Dynamic buffer update (Phase 8A)
 * ------------------------------------------------------------------------- */

static void cpu_buffer_update(MopRhiDevice *device, MopRhiBuffer *buffer,
                              const void *data, size_t offset, size_t size) {
  (void)device;
  memcpy((uint8_t *)buffer->data + offset, data, size);
}

/* -------------------------------------------------------------------------
 * Picking readback
 * ------------------------------------------------------------------------- */

static uint32_t cpu_pick_read_id(MopRhiDevice *device, MopRhiFramebuffer *fb,
                                 int x, int y) {
  (void)device;
  if (x < 0 || x >= fb->fb.width || y < 0 || y >= fb->fb.height)
    return 0;
  return fb->fb.object_id[(size_t)y * (size_t)fb->fb.width + (size_t)x];
}

static float cpu_pick_read_depth(MopRhiDevice *device, MopRhiFramebuffer *fb,
                                 int x, int y) {
  (void)device;
  if (x < 0 || x >= fb->fb.width || y < 0 || y >= fb->fb.height)
    return 1.0f;
  return fb->fb.depth[(size_t)y * (size_t)fb->fb.width + (size_t)x];
}

/* -------------------------------------------------------------------------
 * Color buffer readback
 * ------------------------------------------------------------------------- */

static const uint8_t *cpu_framebuffer_read_color(MopRhiDevice *device,
                                                 MopRhiFramebuffer *fb,
                                                 int *out_width,
                                                 int *out_height) {
  (void)device;
  if (out_width)
    *out_width = fb->fb.width;
  if (out_height)
    *out_height = fb->fb.height;
  return fb->fb.color;
}

/* -------------------------------------------------------------------------
 * Object-ID buffer readback
 * ------------------------------------------------------------------------- */

static const uint32_t *cpu_framebuffer_read_object_id(MopRhiDevice *device,
                                                      MopRhiFramebuffer *fb,
                                                      int *out_width,
                                                      int *out_height) {
  (void)device;
  if (out_width)
    *out_width = fb->fb.width;
  if (out_height)
    *out_height = fb->fb.height;
  return fb->fb.object_id;
}

/* -------------------------------------------------------------------------
 * Depth buffer readback
 * ------------------------------------------------------------------------- */

static const float *cpu_framebuffer_read_depth(MopRhiDevice *device,
                                               MopRhiFramebuffer *fb,
                                               int *out_width,
                                               int *out_height) {
  (void)device;
  if (out_width)
    *out_width = fb->fb.width;
  if (out_height)
    *out_height = fb->fb.height;
  return fb->fb.depth;
}

/* -------------------------------------------------------------------------
 * Texture management
 * ------------------------------------------------------------------------- */

static MopRhiTexture *cpu_texture_create(MopRhiDevice *device, int width,
                                         int height, const uint8_t *rgba_data) {
  (void)device;
  MopRhiTexture *tex = malloc(sizeof(MopRhiTexture));
  if (!tex)
    return NULL;

  size_t byte_count = (size_t)width * (size_t)height * 4;
  tex->data = malloc(byte_count);
  if (!tex->data) {
    free(tex);
    return NULL;
  }

  memcpy(tex->data, rgba_data, byte_count);
  tex->width = width;
  tex->height = height;
  return tex;
}

static MopRhiTexture *cpu_texture_create_hdr(MopRhiDevice *device, int width,
                                             int height,
                                             const float *rgba_float_data) {
  (void)device;
  MopRhiTexture *tex = calloc(1, sizeof(MopRhiTexture));
  if (!tex)
    return NULL;

  size_t float_count = (size_t)width * (size_t)height * 4;
  tex->hdr_data = malloc(float_count * sizeof(float));
  if (!tex->hdr_data) {
    free(tex);
    return NULL;
  }

  memcpy(tex->hdr_data, rgba_float_data, float_count * sizeof(float));
  tex->width = width;
  tex->height = height;
  tex->is_hdr = true;
  return tex;
}

static void cpu_texture_destroy(MopRhiDevice *device, MopRhiTexture *texture) {
  (void)device;
  if (!texture)
    return;
  free(texture->data);
  free(texture->hdr_data);
  free(texture);
}

/* -------------------------------------------------------------------------
 * Buffer read (overlay safety)
 * ------------------------------------------------------------------------- */

static const void *cpu_buffer_read(MopRhiBuffer *buffer) {
  return buffer ? buffer->data : NULL;
}

static float cpu_frame_gpu_time_ms(MopRhiDevice *device) {
  (void)device;
  return 0.0f;
}

static void cpu_set_exposure(MopRhiDevice *device, float exposure) {
  (void)device;
  (void)exposure;
  /* CPU backend: exposure handled by mop_sw_hdr_resolve() in viewport core */
}

static void cpu_set_ibl_textures(MopRhiDevice *device,
                                 MopRhiTexture *irradiance,
                                 MopRhiTexture *prefiltered,
                                 MopRhiTexture *brdf_lut) {
  (void)device;
  (void)irradiance;
  (void)prefiltered;
  (void)brdf_lut;
  /* CPU backend: IBL handled via mop_sw_ibl_set() in viewport core */
}

static void cpu_draw_skybox(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                            MopRhiTexture *env_map, const float *inv_vp,
                            const float *cam_pos, float rotation,
                            float intensity) {
  (void)dev;
  (void)fb;
  (void)env_map;
  (void)inv_vp;
  (void)cam_pos;
  (void)rotation;
  (void)intensity;
  /* CPU backend: skybox handled in viewport core pass_background */
}

static void cpu_draw_overlays(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                              const void *prims, uint32_t prim_count,
                              const void *grid_params, int fb_width,
                              int fb_height) {
  (void)dev;
  (void)grid_params; /* CPU grid handled by mop_overlay_builtin_grid */
  if (!fb || !prims || prim_count == 0)
    return;

  MopSwFramebuffer *sw_fb = (MopSwFramebuffer *)fb;
  uint8_t *rgba = sw_fb->color;
  if (!rgba)
    return;

  int w = fb_width;
  int h = fb_height;

  /* Iterate overlay primitives and pixel-write using CPU AA functions.
   * MopOverlayPrim: x0,y0,x1,y1, r,g,b,a, width, radius, type, depth
   * (12 floats = 48 bytes per prim) */
  const uint8_t *src = (const uint8_t *)prims;
  for (uint32_t i = 0; i < prim_count; i++) {
    const float *pf = (const float *)(src + i * 48);
    float x0 = pf[0], y0 = pf[1], x1 = pf[2], y1 = pf[3];
    float cr = pf[4], cg = pf[5], cb = pf[6], opacity = pf[7];
    float line_w = pf[8], radius = pf[9];
    int32_t type;
    memcpy(&type, &pf[10], sizeof(int32_t));
    /* float depth = pf[11]; — CPU path ignores depth for now (handled in
     * overlay_builtin.c directly for CPU) */

    uint8_t r8 = (uint8_t)(cr * 255.0f);
    uint8_t g8 = (uint8_t)(cg * 255.0f);
    uint8_t b8 = (uint8_t)(cb * 255.0f);

    if (type == 0) {
      /* LINE: capsule AA */
      float margin = line_w + 1.5f;
      int bx0 = (int)fmaxf(0, fminf(x0, x1) - margin);
      int by0 = (int)fmaxf(0, fminf(y0, y1) - margin);
      int bx1 = (int)fminf((float)(w - 1), fmaxf(x0, x1) + margin);
      int by1 = (int)fminf((float)(h - 1), fmaxf(y0, y1) + margin);
      float dx = x1 - x0, dy = y1 - y0;
      float seg_len = sqrtf(dx * dx + dy * dy);
      if (seg_len < 0.5f)
        continue;
      float inv_len = 1.0f / seg_len;
      float ux = dx * inv_len, uy = dy * inv_len;
      float hw = line_w * 0.5f;
      for (int py = by0; py <= by1; py++) {
        for (int px = bx0; px <= bx1; px++) {
          float fx = (float)px + 0.5f - x0;
          float fy = (float)py + 0.5f - y0;
          float along = fx * ux + fy * uy;
          if (along < -1.0f || along > seg_len + 1.0f)
            continue;
          float perp = fabsf(fx * (-uy) + fy * ux);
          float alpha;
          if (perp <= hw - 0.5f)
            alpha = 1.0f;
          else if (perp >= hw + 0.5f)
            continue;
          else
            alpha = 1.0f - (perp - (hw - 0.5f));
          if (along < 0.0f)
            alpha *= fmaxf(0.0f, 1.0f + along);
          else if (along > seg_len)
            alpha *= fmaxf(0.0f, 1.0f - (along - seg_len));
          alpha *= opacity;
          if (alpha < 0.004f)
            continue;
          int idx = (py * w + px) * 4;
          rgba[idx + 0] =
              (uint8_t)(rgba[idx + 0] * (1.0f - alpha) + r8 * alpha);
          rgba[idx + 1] =
              (uint8_t)(rgba[idx + 1] * (1.0f - alpha) + g8 * alpha);
          rgba[idx + 2] =
              (uint8_t)(rgba[idx + 2] * (1.0f - alpha) + b8 * alpha);
        }
      }
    } else if (type == 1) {
      /* FILLED_CIRCLE */
      int bx0 = (int)(x0 - radius - 1.5f);
      int by0 = (int)(y0 - radius - 1.5f);
      int bx1 = (int)(x0 + radius + 1.5f);
      int by1 = (int)(y0 + radius + 1.5f);
      if (bx0 < 0)
        bx0 = 0;
      if (by0 < 0)
        by0 = 0;
      if (bx1 >= w)
        bx1 = w - 1;
      if (by1 >= h)
        by1 = h - 1;
      for (int py = by0; py <= by1; py++) {
        for (int px = bx0; px <= bx1; px++) {
          float fx = (float)px + 0.5f - x0;
          float fy = (float)py + 0.5f - y0;
          float d = sqrtf(fx * fx + fy * fy);
          float a = radius + 0.5f - d;
          if (a <= 0.0f)
            continue;
          if (a > 1.0f)
            a = 1.0f;
          a *= opacity;
          if (a < 0.004f)
            continue;
          int idx = (py * w + px) * 4;
          float ia = 1.0f - a;
          rgba[idx + 0] = (uint8_t)((float)rgba[idx + 0] * ia + (float)r8 * a);
          rgba[idx + 1] = (uint8_t)((float)rgba[idx + 1] * ia + (float)g8 * a);
          rgba[idx + 2] = (uint8_t)((float)rgba[idx + 2] * ia + (float)b8 * a);
        }
      }
    } else {
      /* DIAMOND: 4 lines forming a rotated square */
      float cx = x0, cy = y0;
      float r_d = radius;
      /* Top-right, right-bottom, bottom-left, left-top */
      float pts[4][2] = {
          {cx, cy - r_d}, {cx + r_d, cy}, {cx, cy + r_d}, {cx - r_d, cy}};
      for (int e = 0; e < 4; e++) {
        int ne = (e + 1) % 4;
        float lx0 = pts[e][0], ly0 = pts[e][1];
        float lx1 = pts[ne][0], ly1 = pts[ne][1];
        /* Inline AA line */
        float margin = line_w + 1.5f;
        int bbx0 = (int)fmaxf(0, fminf(lx0, lx1) - margin);
        int bby0 = (int)fmaxf(0, fminf(ly0, ly1) - margin);
        int bbx1 = (int)fminf((float)(w - 1), fmaxf(lx0, lx1) + margin);
        int bby1 = (int)fminf((float)(h - 1), fmaxf(ly0, ly1) + margin);
        float dx = lx1 - lx0, dy = ly1 - ly0;
        float seg_len = sqrtf(dx * dx + dy * dy);
        if (seg_len < 0.5f)
          continue;
        float inv_len = 1.0f / seg_len;
        float eux = dx * inv_len, euy = dy * inv_len;
        float hw = line_w * 0.5f;
        for (int py = bby0; py <= bby1; py++) {
          for (int px = bbx0; px <= bbx1; px++) {
            float fx = (float)px + 0.5f - lx0;
            float fy = (float)py + 0.5f - ly0;
            float along = fx * eux + fy * euy;
            if (along < -1.0f || along > seg_len + 1.0f)
              continue;
            float perp = fabsf(fx * (-euy) + fy * eux);
            float alpha;
            if (perp <= hw - 0.5f)
              alpha = 1.0f;
            else if (perp >= hw + 0.5f)
              continue;
            else
              alpha = 1.0f - (perp - (hw - 0.5f));
            if (along < 0.0f)
              alpha *= fmaxf(0.0f, 1.0f + along);
            else if (along > seg_len)
              alpha *= fmaxf(0.0f, 1.0f - (along - seg_len));
            alpha *= opacity;
            if (alpha < 0.004f)
              continue;
            int idx = (py * w + px) * 4;
            rgba[idx + 0] =
                (uint8_t)(rgba[idx + 0] * (1.0f - alpha) + r8 * alpha);
            rgba[idx + 1] =
                (uint8_t)(rgba[idx + 1] * (1.0f - alpha) + g8 * alpha);
            rgba[idx + 2] =
                (uint8_t)(rgba[idx + 2] * (1.0f - alpha) + b8 * alpha);
          }
        }
      }
    }
  }
}

/* -------------------------------------------------------------------------
 * Backend function table
 * ------------------------------------------------------------------------- */

static const MopRhiBackend CPU_BACKEND = {
    .name = "cpu",
    .device_create = cpu_device_create,
    .device_destroy = cpu_device_destroy,
    .buffer_create = cpu_buffer_create,
    .buffer_destroy = cpu_buffer_destroy,
    .framebuffer_create = cpu_framebuffer_create,
    .framebuffer_destroy = cpu_framebuffer_destroy,
    .framebuffer_resize = cpu_framebuffer_resize,
    .frame_begin = cpu_frame_begin,
    .frame_end = cpu_frame_end,
    .draw = cpu_draw,
    .pick_read_id = cpu_pick_read_id,
    .pick_read_depth = cpu_pick_read_depth,
    .framebuffer_read_color = cpu_framebuffer_read_color,
    .framebuffer_read_object_id = cpu_framebuffer_read_object_id,
    .framebuffer_read_depth = cpu_framebuffer_read_depth,
    .texture_create = cpu_texture_create,
    .texture_create_hdr = cpu_texture_create_hdr,
    .texture_destroy = cpu_texture_destroy,
    .draw_instanced = cpu_draw_instanced,
    .buffer_update = cpu_buffer_update,
    .buffer_read = cpu_buffer_read,
    .frame_gpu_time_ms = cpu_frame_gpu_time_ms,
    .set_exposure = cpu_set_exposure,
    .set_bloom = NULL, /* bloom is GPU-only */
    .set_ssao = NULL,  /* SSAO is GPU-only */
    .set_ibl_textures = cpu_set_ibl_textures,
    .draw_skybox = cpu_draw_skybox,
    .draw_overlays = cpu_draw_overlays,
};

const MopRhiBackend *mop_rhi_backend_cpu(void) { return &CPU_BACKEND; }
