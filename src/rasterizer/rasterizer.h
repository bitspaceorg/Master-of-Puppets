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

#include <mop/light.h>
#include <mop/types.h>
#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Framebuffer storage for software rasterization
 *
 * All buffers use top-left origin.  Row stride = width.
 * Color buffer is RGBA8 (4 bytes per pixel).
 * Depth buffer stores float values in [0, 1].
 * Object ID buffer stores uint32_t per pixel (0 = background).
 * ------------------------------------------------------------------------- */

typedef struct MopSwFramebuffer {
  int width;
  int height;
  uint8_t *color;      /* RGBA8, size = width * height * 4 */
  float *depth;        /* float,  size = width * height     */
  uint32_t *object_id; /* uint32, size = width * height     */
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
  MopVec4 position; /* clip-space (before perspective divide) */
  MopVec3 normal;   /* world-space normal */
  MopColor color;   /* vertex color */
  float u, v;       /* texture coordinates */
  MopVec3 tangent;  /* world-space tangent (for normal mapping) */
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

/* Normal map texture data for the rasterizer (optional, NULL = disabled) */
typedef struct MopSwNormalMap {
  const uint8_t *data; /* RGBA8 normal map pixels */
  int width;
  int height;
} MopSwNormalMap;

void mop_sw_rasterize_triangle(const MopSwClipVertex vertices[3],
                               uint32_t object_id, bool wireframe,
                               bool depth_test, bool cull_back,
                               MopVec3 light_dir, float ambient, float opacity,
                               bool smooth_shading, MopBlendMode blend_mode,
                               MopSwFramebuffer *fb);

/* -------------------------------------------------------------------------
 * Screen-space vertex — output of perspective division + viewport transform
 * ------------------------------------------------------------------------- */

typedef struct MopSwScreenVertex {
  float sx, sy, sz; /* screen-space position */
  MopVec3 normal;   /* world-space normal */
  MopColor color;   /* vertex color */
  float u, v;       /* texture coordinates */
  MopVec3 tangent;  /* world-space tangent (for normal mapping) */
} MopSwScreenVertex;

/* -------------------------------------------------------------------------
 * Smooth-shaded triangle rasterization (Gouraud)
 *
 * Interpolates per-vertex normals across the triangle using barycentric
 * coordinates.  Computes per-pixel N.L for smooth lighting.
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle_smooth(const MopSwScreenVertex verts[3],
                                      uint32_t object_id, bool depth_test,
                                      MopVec3 light_dir, float ambient,
                                      float opacity, MopBlendMode blend_mode,
                                      MopSwFramebuffer *fb);

/* Smooth-shaded triangle rasterization with multi-light support.
 * If lights is non-NULL and light_count > 0, accumulates contribution
 * from all active lights (directional, point, spot).
 * If lights is NULL, falls back to single-light (light_dir + ambient). */
void mop_sw_rasterize_triangle_smooth_ml(const MopSwScreenVertex verts[3],
                                         uint32_t object_id, bool depth_test,
                                         MopVec3 light_dir, float ambient,
                                         float opacity, MopBlendMode blend_mode,
                                         const MopLight *lights,
                                         uint32_t light_count, MopVec3 cam_eye,
                                         MopSwFramebuffer *fb);

/* Smooth-shaded triangle rasterization with normal mapping.
 * If normal_map is non-NULL, tangent-space normals are sampled from the
 * normal map and transformed to world space using the TBN matrix. */
void mop_sw_rasterize_triangle_smooth_nm(const MopSwScreenVertex verts[3],
                                         uint32_t object_id, bool depth_test,
                                         MopVec3 light_dir, float ambient,
                                         float opacity, MopBlendMode blend_mode,
                                         const MopSwNormalMap *normal_map,
                                         MopSwFramebuffer *fb);

/* -------------------------------------------------------------------------
 * Full triangle rasterization with multi-light support
 *
 * Same as mop_sw_rasterize_triangle but routes to the multi-light
 * smooth/flat shading paths when lights are available.
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle_full(
    const MopSwClipVertex vertices[3], uint32_t object_id, bool wireframe,
    bool depth_test, bool cull_back, MopVec3 light_dir, float ambient,
    float opacity, bool smooth_shading, MopBlendMode blend_mode,
    const MopLight *lights, uint32_t light_count, MopVec3 cam_eye,
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

void mop_sw_draw_line(MopSwFramebuffer *fb, int x0, int y0, float z0, int x1,
                      int y1, float z1, uint8_t r, uint8_t g, uint8_t b,
                      uint32_t object_id, bool depth_test);

#endif /* MOP_SW_RASTERIZER_H */
