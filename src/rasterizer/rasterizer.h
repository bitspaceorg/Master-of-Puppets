/*
 * Master of Puppets — Software Rasterizer
 * rasterizer.h — Shared software triangle rasterization interface
 *
 * This module provides backend-agnostic software rasterization.
 * Any backend that needs a CPU-side rasterizer can reuse these
 * functions instead of reimplementing them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_SW_RASTERIZER_H
#define MOP_SW_RASTERIZER_H

#include <mop/types.h>
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Framebuffer storage for software rasterization
 *
 * All buffers use top-left origin.  Row stride = width.
 * Color buffer is RGBA8 (4 bytes per pixel).
 * Depth buffer stores float values in [0, 1].
 * Object ID buffer stores uint32_t per pixel (0 = background).
 * ------------------------------------------------------------------------- */

typedef struct MopSwFramebuffer {
    int       width;
    int       height;
    uint8_t  *color;       /* RGBA8, size = width * height * 4 */
    float    *depth;       /* float,  size = width * height     */
    uint32_t *object_id;   /* uint32, size = width * height     */
} MopSwFramebuffer;

/* Allocate framebuffer storage.  Returns false on allocation failure. */
bool mop_sw_framebuffer_alloc(MopSwFramebuffer *fb, int width, int height);

/* Free framebuffer storage. */
void mop_sw_framebuffer_free(MopSwFramebuffer *fb);

/* Clear all buffers.  Depth is reset to 1.0, object_id to 0. */
void mop_sw_framebuffer_clear(MopSwFramebuffer *fb, MopColor clear_color);

/* -------------------------------------------------------------------------
 * Clip-space vertex — output of vertex transformation
 * ------------------------------------------------------------------------- */

typedef struct MopSwClipVertex {
    MopVec4  position;   /* clip-space (before perspective divide) */
    MopVec3  normal;     /* world-space normal */
    MopColor color;      /* vertex color */
} MopSwClipVertex;

/* -------------------------------------------------------------------------
 * Triangle rasterization
 *
 * Rasterizes a single triangle into the framebuffer.
 *
 * vertices   : 3 clip-space vertices (output of MVP transform)
 * object_id  : written to the ID buffer for picking
 * wireframe  : if true, draw edges only (Bresenham)
 * depth_test : if true, test and write depth
 * cull_back  : if true, skip back-facing triangles
 * light_dir  : world-space light direction for flat shading
 * fb         : target framebuffer
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle(const MopSwClipVertex vertices[3],
                                uint32_t object_id,
                                bool wireframe,
                                bool depth_test,
                                bool cull_back,
                                MopVec3 light_dir,
                                MopSwFramebuffer *fb);

/* -------------------------------------------------------------------------
 * Sutherland-Hodgman clipping
 *
 * Clips a polygon against the view frustum in clip space.
 * Input: polygon of n vertices.
 * Output: clipped polygon written to out_vertices.
 * Returns the number of output vertices (0 if fully clipped).
 *
 * max_out must be >= n + 6 (each frustum plane can add at most 1 vertex).
 * ------------------------------------------------------------------------- */

int mop_sw_clip_polygon(const MopSwClipVertex *in_vertices, int n,
                         MopSwClipVertex *out_vertices, int max_out);

/* -------------------------------------------------------------------------
 * Wireframe line drawing (Bresenham)
 * ------------------------------------------------------------------------- */

void mop_sw_draw_line(MopSwFramebuffer *fb,
                       int x0, int y0, float z0,
                       int x1, int y1, float z1,
                       uint8_t r, uint8_t g, uint8_t b,
                       uint32_t object_id, bool depth_test);

#endif /* MOP_SW_RASTERIZER_H */
