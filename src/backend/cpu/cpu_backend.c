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

#include "rhi/rhi.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_mt.h"

#include <mop/log.h>
#include <mop/vertex_format.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal types
 * ------------------------------------------------------------------------- */

struct MopRhiDevice {
    MopSwThreadPool *threadpool;  /* tile-based parallel rasterizer */
};

struct MopRhiBuffer {
    void  *data;
    size_t size;
};

struct MopRhiFramebuffer {
    MopSwFramebuffer fb;
    uint8_t         *readback;     /* RGBA8 readback copy (same as fb.color) */
};

struct MopRhiTexture {
    int      width;
    int      height;
    uint8_t *data;   /* RGBA8, row-major */
};

/* -------------------------------------------------------------------------
 * Device lifecycle
 * ------------------------------------------------------------------------- */

static MopRhiDevice *cpu_device_create(void) {
    MopRhiDevice *dev = calloc(1, sizeof(MopRhiDevice));
    if (!dev) return NULL;

    /* Detect core count and create thread pool */
    int num_cores = 1;
#if defined(_SC_NPROCESSORS_ONLN)
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n > 1) num_cores = (int)n;
    }
#endif
    /* Use at most (cores - 1) worker threads (main thread also participates) */
    int workers = num_cores > 1 ? num_cores - 1 : 1;
    dev->threadpool = mop_sw_threadpool_create(workers);
    /* threadpool creation failure is non-fatal — falls back to single-threaded */

    return dev;
}

static void cpu_device_destroy(MopRhiDevice *device) {
    if (!device) return;
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
    if (!buf) return NULL;

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
    if (!buffer) return;
    free(buffer->data);
    free(buffer);
}

/* -------------------------------------------------------------------------
 * Framebuffer management
 * ------------------------------------------------------------------------- */

static MopRhiFramebuffer *cpu_framebuffer_create(MopRhiDevice *device,
                                                  const MopRhiFramebufferDesc *desc) {
    (void)device;

    MopRhiFramebuffer *fb = calloc(1, sizeof(MopRhiFramebuffer));
    if (!fb) return NULL;

    if (!mop_sw_framebuffer_alloc(&fb->fb, desc->width, desc->height)) {
        free(fb);
        return NULL;
    }

    fb->readback = fb->fb.color;  /* Point directly to the color buffer */
    return fb;
}

static void cpu_framebuffer_destroy(MopRhiDevice *device,
                                    MopRhiFramebuffer *fb) {
    (void)device;
    if (!fb) return;
    mop_sw_framebuffer_free(&fb->fb);
    free(fb);
}

static void cpu_framebuffer_resize(MopRhiDevice *device,
                                   MopRhiFramebuffer *fb,
                                   int width, int height) {
    (void)device;
    if (!fb) return;

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
    /* CPU backend: nothing to finalize */
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
                                  const MopVertex *v0,
                                  const MopVertex *v1,
                                  const MopVertex *v2,
                                  MopSwPreparedTri *out) {
    /* Transform vertices to clip space */
    MopVec4 pos0 = { v0->position.x, v0->position.y, v0->position.z, 1.0f };
    MopVec4 pos1 = { v1->position.x, v1->position.y, v1->position.z, 1.0f };
    MopVec4 pos2 = { v2->position.x, v2->position.y, v2->position.z, 1.0f };

    out->vertices[0].position = mop_mat4_mul_vec4(call->mvp, pos0);
    out->vertices[1].position = mop_mat4_mul_vec4(call->mvp, pos1);
    out->vertices[2].position = mop_mat4_mul_vec4(call->mvp, pos2);

    /* Transform normals by model matrix (upper 3x3) */
    MopVec4 n0 = { v0->normal.x, v0->normal.y, v0->normal.z, 0.0f };
    MopVec4 n1 = { v1->normal.x, v1->normal.y, v1->normal.z, 0.0f };
    MopVec4 n2 = { v2->normal.x, v2->normal.y, v2->normal.z, 0.0f };

    MopVec4 tn0 = mop_mat4_mul_vec4(call->model, n0);
    MopVec4 tn1 = mop_mat4_mul_vec4(call->model, n1);
    MopVec4 tn2 = mop_mat4_mul_vec4(call->model, n2);

    out->vertices[0].normal = (MopVec3){ tn0.x, tn0.y, tn0.z };
    out->vertices[1].normal = (MopVec3){ tn1.x, tn1.y, tn1.z };
    out->vertices[2].normal = (MopVec3){ tn2.x, tn2.y, tn2.z };

    out->vertices[0].color = v0->color;
    out->vertices[1].color = v1->color;
    out->vertices[2].color = v2->color;

    out->vertices[0].u = v0->u; out->vertices[0].v = v0->v;
    out->vertices[1].u = v1->u; out->vertices[1].v = v1->v;
    out->vertices[2].u = v2->u; out->vertices[2].v = v2->v;

    /* Nearest-neighbor texture sampling — modulate vertex color */
    if (call->texture && call->texture->width >= 1 && call->texture->height >= 1) {
        MopRhiTexture *tex = call->texture;
        for (int t = 0; t < 3; t++) {
            float tu = out->vertices[t].u - floorf(out->vertices[t].u);
            float tv = out->vertices[t].v - floorf(out->vertices[t].v);
            int tx = (int)(tu * (float)(tex->width  - 1) + 0.5f);
            int ty = (int)(tv * (float)(tex->height - 1) + 0.5f);
            if (tx < 0) tx = 0;
            if (tx >= tex->width) tx = tex->width - 1;
            if (ty < 0) ty = 0;
            if (ty >= tex->height) ty = tex->height - 1;
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

    out->object_id      = call->object_id;
    out->wireframe      = call->wireframe;
    out->depth_test     = call->depth_test;
    out->cull_back      = call->backface_cull;
    out->light_dir      = call->light_dir;
    out->ambient        = call->ambient;
    out->opacity        = call->opacity;
    out->smooth_shading = (call->shading_mode == MOP_SHADING_SMOOTH);
    out->blend_mode     = call->blend_mode;
    out->lights         = call->lights;
    out->light_count    = call->light_count;
    out->cam_eye        = call->cam_eye;

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
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2]; break;
    case MOP_FORMAT_FLOAT2:
        out[0] = f[0]; out[1] = f[1]; out[2] = 0.0f; break;
    case MOP_FORMAT_FLOAT:
        out[0] = f[0]; out[1] = 0.0f; out[2] = 0.0f; break;
    default:
        out[0] = out[1] = out[2] = 0.0f; break;
    }
}

static void read_attrib_float4(const uint8_t *base, uint32_t offset,
                                MopAttribFormat fmt, float out[4]) {
    const float *f = (const float *)(base + offset);
    switch (fmt) {
    case MOP_FORMAT_FLOAT4:
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2]; out[3] = f[3]; break;
    case MOP_FORMAT_FLOAT3:
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2]; out[3] = 1.0f; break;
    case MOP_FORMAT_FLOAT2:
        out[0] = f[0]; out[1] = f[1]; out[2] = 0.0f; out[3] = 1.0f; break;
    case MOP_FORMAT_FLOAT:
        out[0] = f[0]; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f; break;
    default:
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; break;
    }
}

static void read_attrib_float2(const uint8_t *base, uint32_t offset,
                                MopAttribFormat fmt, float out[2]) {
    const float *f = (const float *)(base + offset);
    switch (fmt) {
    case MOP_FORMAT_FLOAT2:
    case MOP_FORMAT_FLOAT3:
    case MOP_FORMAT_FLOAT4:
        out[0] = f[0]; out[1] = f[1]; break;
    case MOP_FORMAT_FLOAT:
        out[0] = f[0]; out[1] = 0.0f; break;
    default:
        out[0] = out[1] = 0.0f; break;
    }
}

static bool cpu_prepare_triangle_ex(const MopRhiDrawCall *call,
                                     const uint8_t *raw,
                                     uint32_t stride,
                                     uint32_t i0, uint32_t i1, uint32_t i2,
                                     MopSwPreparedTri *out) {
    const MopVertexFormat *fmt = call->vertex_format;
    const uint8_t *v[3] = {
        raw + (size_t)i0 * stride,
        raw + (size_t)i1 * stride,
        raw + (size_t)i2 * stride
    };

    const MopVertexAttrib *pos_attr = mop_vertex_format_find(fmt, MOP_ATTRIB_POSITION);
    if (!pos_attr) return false;

    const MopVertexAttrib *nrm_attr = mop_vertex_format_find(fmt, MOP_ATTRIB_NORMAL);
    const MopVertexAttrib *col_attr = mop_vertex_format_find(fmt, MOP_ATTRIB_COLOR);
    const MopVertexAttrib *uv_attr  = mop_vertex_format_find(fmt, MOP_ATTRIB_TEXCOORD0);

    for (int t = 0; t < 3; t++) {
        /* Position → clip space */
        float pos[3];
        read_attrib_float3(v[t], pos_attr->offset, pos_attr->format, pos);
        MopVec4 p = { pos[0], pos[1], pos[2], 1.0f };
        out->vertices[t].position = mop_mat4_mul_vec4(call->mvp, p);

        /* Normal → world space */
        if (nrm_attr) {
            float n[3];
            read_attrib_float3(v[t], nrm_attr->offset, nrm_attr->format, n);
            MopVec4 nv = { n[0], n[1], n[2], 0.0f };
            MopVec4 tn = mop_mat4_mul_vec4(call->model, nv);
            out->vertices[t].normal = (MopVec3){ tn.x, tn.y, tn.z };
        } else {
            out->vertices[t].normal = (MopVec3){ 0.0f, 1.0f, 0.0f };
        }

        /* Color */
        if (col_attr) {
            float c[4];
            read_attrib_float4(v[t], col_attr->offset, col_attr->format, c);
            out->vertices[t].color = (MopColor){ c[0], c[1], c[2], c[3] };
        } else {
            out->vertices[t].color = (MopColor){ 1.0f, 1.0f, 1.0f, 1.0f };
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

        out->vertices[t].tangent = (MopVec3){ 0, 0, 0 };
    }

    /* Texture sampling (same as standard path) */
    if (call->texture && call->texture->width >= 1 && call->texture->height >= 1) {
        MopRhiTexture *tex = call->texture;
        for (int t = 0; t < 3; t++) {
            float tu = out->vertices[t].u - floorf(out->vertices[t].u);
            float tv = out->vertices[t].v - floorf(out->vertices[t].v);
            int tx = (int)(tu * (float)(tex->width  - 1) + 0.5f);
            int ty = (int)(tv * (float)(tex->height - 1) + 0.5f);
            if (tx < 0) tx = 0;
            if (tx >= tex->width) tx = tex->width - 1;
            if (ty < 0) ty = 0;
            if (ty >= tex->height) ty = tex->height - 1;
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

    out->object_id      = call->object_id;
    out->wireframe      = call->wireframe;
    out->depth_test     = call->depth_test;
    out->cull_back      = call->backface_cull;
    out->light_dir      = call->light_dir;
    out->ambient        = call->ambient;
    out->opacity        = call->opacity;
    out->smooth_shading = (call->shading_mode == MOP_SHADING_SMOOTH);
    out->blend_mode     = call->blend_mode;
    out->lights         = call->lights;
    out->light_count    = call->light_count;
    out->cam_eye        = call->cam_eye;

    return true;
}

static void cpu_draw(MopRhiDevice *device, MopRhiFramebuffer *fb,
                     const MopRhiDrawCall *call) {
    const uint8_t   *raw_data = (const uint8_t *)call->vertex_buffer->data;
    const MopVertex *vertices = (const MopVertex *)raw_data;
    const uint32_t  *indices  = (const uint32_t *)call->index_buffer->data;
    uint32_t tri_count = call->index_count / 3;
    bool use_flex = (call->vertex_format != NULL);

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

                if (i0 >= call->vertex_count ||
                    i1 >= call->vertex_count ||
                    i2 >= call->vertex_count) {
                    continue;
                }

                if (use_flex) {
                    cpu_prepare_triangle_ex(call, raw_data,
                                             call->vertex_format->stride,
                                             i0, i1, i2,
                                             &prepared[prepared_count]);
                } else {
                    cpu_prepare_triangle(call,
                                          &vertices[i0],
                                          &vertices[i1],
                                          &vertices[i2],
                                          &prepared[prepared_count]);
                }
                prepared_count++;
            }

            mop_sw_rasterize_tiled(device->threadpool, prepared,
                                    prepared_count, &fb->fb);
            free(prepared);
            return;
        }
        /* malloc failed — fall through to single-threaded path */
        MOP_WARN("tiled rasterizer allocation failed, falling back to single-threaded");
    }

    /* Single-threaded fallback */
    for (uint32_t i = 0; i + 2 < call->index_count; i += 3) {
        uint32_t i0 = indices[i + 0];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];

        if (i0 >= call->vertex_count ||
            i1 >= call->vertex_count ||
            i2 >= call->vertex_count) {
            continue;
        }

        MopSwPreparedTri tri;
        if (use_flex) {
            cpu_prepare_triangle_ex(call, raw_data,
                                     call->vertex_format->stride,
                                     i0, i1, i2, &tri);
        } else {
            cpu_prepare_triangle(call, &vertices[i0], &vertices[i1],
                                  &vertices[i2], &tri);
        }

        if (tri.lights && tri.light_count > 0) {
            mop_sw_rasterize_triangle_full(tri.vertices, tri.object_id,
                                            tri.wireframe, tri.depth_test,
                                            tri.cull_back, tri.light_dir,
                                            tri.ambient, tri.opacity,
                                            tri.smooth_shading, tri.blend_mode,
                                            tri.lights, tri.light_count,
                                            tri.cam_eye, &fb->fb);
        } else {
            mop_sw_rasterize_triangle(tri.vertices, tri.object_id,
                                       tri.wireframe, tri.depth_test,
                                       tri.cull_back, tri.light_dir,
                                       tri.ambient, tri.opacity,
                                       tri.smooth_shading, tri.blend_mode,
                                       &fb->fb);
        }
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
    if (!call || !instance_transforms || instance_count == 0) return;

    for (uint32_t inst = 0; inst < instance_count; inst++) {
        /* Build a per-instance draw call with overridden model/mvp */
        MopRhiDrawCall inst_call = *call;
        inst_call.model = instance_transforms[inst];

        /* Recompute MVP: projection * view * instance_model */
        MopMat4 view_model = mop_mat4_multiply(call->view,
                                                instance_transforms[inst]);
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
    if (x < 0 || x >= fb->fb.width || y < 0 || y >= fb->fb.height) return 0;
    return fb->fb.object_id[(size_t)y * (size_t)fb->fb.width + (size_t)x];
}

static float cpu_pick_read_depth(MopRhiDevice *device, MopRhiFramebuffer *fb,
                                 int x, int y) {
    (void)device;
    if (x < 0 || x >= fb->fb.width || y < 0 || y >= fb->fb.height) return 1.0f;
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
    if (out_width)  *out_width  = fb->fb.width;
    if (out_height) *out_height = fb->fb.height;
    return fb->fb.color;
}

/* -------------------------------------------------------------------------
 * Texture management
 * ------------------------------------------------------------------------- */

static MopRhiTexture *cpu_texture_create(MopRhiDevice *device, int width,
                                          int height,
                                          const uint8_t *rgba_data) {
    (void)device;
    MopRhiTexture *tex = malloc(sizeof(MopRhiTexture));
    if (!tex) return NULL;

    size_t byte_count = (size_t)width * (size_t)height * 4;
    tex->data = malloc(byte_count);
    if (!tex->data) {
        free(tex);
        return NULL;
    }

    memcpy(tex->data, rgba_data, byte_count);
    tex->width  = width;
    tex->height = height;
    return tex;
}

static void cpu_texture_destroy(MopRhiDevice *device, MopRhiTexture *texture) {
    (void)device;
    if (!texture) return;
    free(texture->data);
    free(texture);
}

/* -------------------------------------------------------------------------
 * Buffer read (overlay safety)
 * ------------------------------------------------------------------------- */

static const void *cpu_buffer_read(MopRhiBuffer *buffer) {
    return buffer ? buffer->data : NULL;
}

/* -------------------------------------------------------------------------
 * Backend function table
 * ------------------------------------------------------------------------- */

static const MopRhiBackend CPU_BACKEND = {
    .name                 = "cpu",
    .device_create        = cpu_device_create,
    .device_destroy       = cpu_device_destroy,
    .buffer_create        = cpu_buffer_create,
    .buffer_destroy       = cpu_buffer_destroy,
    .framebuffer_create   = cpu_framebuffer_create,
    .framebuffer_destroy  = cpu_framebuffer_destroy,
    .framebuffer_resize   = cpu_framebuffer_resize,
    .frame_begin          = cpu_frame_begin,
    .frame_end            = cpu_frame_end,
    .draw                 = cpu_draw,
    .pick_read_id         = cpu_pick_read_id,
    .pick_read_depth      = cpu_pick_read_depth,
    .framebuffer_read_color = cpu_framebuffer_read_color,
    .texture_create         = cpu_texture_create,
    .texture_destroy        = cpu_texture_destroy,
    .draw_instanced         = cpu_draw_instanced,
    .buffer_update          = cpu_buffer_update,
    .buffer_read            = cpu_buffer_read,
};

const MopRhiBackend *mop_rhi_backend_cpu(void) {
    return &CPU_BACKEND;
}
