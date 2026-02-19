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

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Internal types
 * ------------------------------------------------------------------------- */

struct MopRhiDevice {
    int placeholder;  /* CPU device has no state beyond existence */
};

struct MopRhiBuffer {
    void  *data;
    size_t size;
};

struct MopRhiFramebuffer {
    MopSwFramebuffer fb;
    uint8_t         *readback;     /* RGBA8 readback copy (same as fb.color) */
};

/* -------------------------------------------------------------------------
 * Device lifecycle
 * ------------------------------------------------------------------------- */

static MopRhiDevice *cpu_device_create(void) {
    MopRhiDevice *dev = calloc(1, sizeof(MopRhiDevice));
    return dev;
}

static void cpu_device_destroy(MopRhiDevice *device) {
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

static void cpu_draw(MopRhiDevice *device, MopRhiFramebuffer *fb,
                     const MopRhiDrawCall *call) {
    (void)device;

    const MopVertex *vertices = (const MopVertex *)call->vertex_buffer->data;
    const uint32_t  *indices  = (const uint32_t *)call->index_buffer->data;

    MopVec3 light_dir = { 0.3f, 1.0f, 0.5f };

    /* Process each triangle */
    for (uint32_t i = 0; i + 2 < call->index_count; i += 3) {
        uint32_t i0 = indices[i + 0];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];

        /* Bounds check indices */
        if (i0 >= call->vertex_count ||
            i1 >= call->vertex_count ||
            i2 >= call->vertex_count) {
            continue;
        }

        const MopVertex *v0 = &vertices[i0];
        const MopVertex *v1 = &vertices[i1];
        const MopVertex *v2 = &vertices[i2];

        /* Transform vertices to clip space */
        MopSwClipVertex tri[3];

        MopVec4 pos0 = { v0->position.x, v0->position.y, v0->position.z, 1.0f };
        MopVec4 pos1 = { v1->position.x, v1->position.y, v1->position.z, 1.0f };
        MopVec4 pos2 = { v2->position.x, v2->position.y, v2->position.z, 1.0f };

        tri[0].position = mop_mat4_mul_vec4(call->mvp, pos0);
        tri[1].position = mop_mat4_mul_vec4(call->mvp, pos1);
        tri[2].position = mop_mat4_mul_vec4(call->mvp, pos2);

        /* Transform normals by model matrix (upper 3x3) */
        MopVec4 n0 = { v0->normal.x, v0->normal.y, v0->normal.z, 0.0f };
        MopVec4 n1 = { v1->normal.x, v1->normal.y, v1->normal.z, 0.0f };
        MopVec4 n2 = { v2->normal.x, v2->normal.y, v2->normal.z, 0.0f };

        MopVec4 tn0 = mop_mat4_mul_vec4(call->model, n0);
        MopVec4 tn1 = mop_mat4_mul_vec4(call->model, n1);
        MopVec4 tn2 = mop_mat4_mul_vec4(call->model, n2);

        tri[0].normal = (MopVec3){ tn0.x, tn0.y, tn0.z };
        tri[1].normal = (MopVec3){ tn1.x, tn1.y, tn1.z };
        tri[2].normal = (MopVec3){ tn2.x, tn2.y, tn2.z };

        tri[0].color = v0->color;
        tri[1].color = v1->color;
        tri[2].color = v2->color;

        mop_sw_rasterize_triangle(tri, call->object_id,
                                   call->wireframe,
                                   call->depth_test,
                                   call->backface_cull,
                                   light_dir,
                                   &fb->fb);
    }
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
};

const MopRhiBackend *mop_rhi_backend_cpu(void) {
    return &CPU_BACKEND;
}
