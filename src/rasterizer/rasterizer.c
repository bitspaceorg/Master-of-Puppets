/*
 * Master of Puppets — Software Rasterizer
 * rasterizer.c — Full software triangle rasterization
 *
 * Implements:
 *   - Sutherland-Hodgman frustum clipping
 *   - Perspective division and viewport transform
 *   - Half-space triangle rasterization
 *   - Depth buffering
 *   - Backface culling
 *   - Flat shading with directional light
 *   - Wireframe rendering via Bresenham
 *   - Object ID buffer for picking
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rasterizer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Framebuffer management
 * ------------------------------------------------------------------------- */

bool mop_sw_framebuffer_alloc(MopSwFramebuffer *fb, int width, int height) {
    size_t pixel_count = (size_t)width * (size_t)height;

    fb->width  = width;
    fb->height = height;

    fb->color = calloc(pixel_count * 4, sizeof(uint8_t));
    if (!fb->color) goto fail_color;

    fb->depth = malloc(pixel_count * sizeof(float));
    if (!fb->depth) goto fail_depth;

    fb->object_id = calloc(pixel_count, sizeof(uint32_t));
    if (!fb->object_id) goto fail_id;

    return true;

fail_id:
    free(fb->depth);
    fb->depth = NULL;
fail_depth:
    free(fb->color);
    fb->color = NULL;
fail_color:
    fb->width = 0;
    fb->height = 0;
    return false;
}

void mop_sw_framebuffer_free(MopSwFramebuffer *fb) {
    free(fb->color);
    free(fb->depth);
    free(fb->object_id);
    fb->color     = NULL;
    fb->depth     = NULL;
    fb->object_id = NULL;
    fb->width     = 0;
    fb->height    = 0;
}

void mop_sw_framebuffer_clear(MopSwFramebuffer *fb, MopColor clear_color) {
    size_t pixel_count = (size_t)fb->width * (size_t)fb->height;

    uint8_t r = (uint8_t)(clear_color.r * 255.0f);
    uint8_t g = (uint8_t)(clear_color.g * 255.0f);
    uint8_t b = (uint8_t)(clear_color.b * 255.0f);
    uint8_t a = (uint8_t)(clear_color.a * 255.0f);

    for (size_t i = 0; i < pixel_count; i++) {
        fb->color[i * 4 + 0] = r;
        fb->color[i * 4 + 1] = g;
        fb->color[i * 4 + 2] = b;
        fb->color[i * 4 + 3] = a;
        fb->depth[i]         = 1.0f;
        fb->object_id[i]     = 0;
    }
}

/* -------------------------------------------------------------------------
 * Sutherland-Hodgman clipping against one plane
 *
 * A clip plane is defined by the condition:
 *   dot(plane_normal, clip_pos) + plane_w >= 0
 *
 * For the 6 frustum planes in clip space:
 *   +X:  w + x >= 0     ( 1,  0,  0,  1)
 *   -X:  w - x >= 0     (-1,  0,  0,  1)
 *   +Y:  w + y >= 0     ( 0,  1,  0,  1)
 *   -Y:  w - y >= 0     ( 0, -1,  0,  1)
 *   +Z:  w + z >= 0     ( 0,  0,  1,  1)
 *   -Z:  w - z >= 0     ( 0,  0, -1,  1)
 * ------------------------------------------------------------------------- */

typedef struct {
    float nx, ny, nz, nw;
} ClipPlane;

static const ClipPlane FRUSTUM_PLANES[6] = {
    {  1.0f,  0.0f,  0.0f,  1.0f },  /* +X: w + x >= 0 */
    { -1.0f,  0.0f,  0.0f,  1.0f },  /* -X: w - x >= 0 */
    {  0.0f,  1.0f,  0.0f,  1.0f },  /* +Y: w + y >= 0 */
    {  0.0f, -1.0f,  0.0f,  1.0f },  /* -Y: w - y >= 0 */
    {  0.0f,  0.0f,  1.0f,  1.0f },  /* +Z: w + z >= 0 */
    {  0.0f,  0.0f, -1.0f,  1.0f },  /* -Z: w - z >= 0 */
};

static float clip_distance(const ClipPlane *plane, MopVec4 pos) {
    return plane->nx * pos.x + plane->ny * pos.y +
           plane->nz * pos.z + plane->nw * pos.w;
}

static MopSwClipVertex lerp_vertex(MopSwClipVertex a, MopSwClipVertex b, float t) {
    MopSwClipVertex result;
    result.position.x = a.position.x + t * (b.position.x - a.position.x);
    result.position.y = a.position.y + t * (b.position.y - a.position.y);
    result.position.z = a.position.z + t * (b.position.z - a.position.z);
    result.position.w = a.position.w + t * (b.position.w - a.position.w);
    result.normal.x   = a.normal.x   + t * (b.normal.x   - a.normal.x);
    result.normal.y   = a.normal.y   + t * (b.normal.y   - a.normal.y);
    result.normal.z   = a.normal.z   + t * (b.normal.z   - a.normal.z);
    result.color.r    = a.color.r    + t * (b.color.r    - a.color.r);
    result.color.g    = a.color.g    + t * (b.color.g    - a.color.g);
    result.color.b    = a.color.b    + t * (b.color.b    - a.color.b);
    result.color.a    = a.color.a    + t * (b.color.a    - a.color.a);
    result.u          = a.u          + t * (b.u          - a.u);
    result.v          = a.v          + t * (b.v          - a.v);
    result.tangent.x  = a.tangent.x  + t * (b.tangent.x  - a.tangent.x);
    result.tangent.y  = a.tangent.y  + t * (b.tangent.y  - a.tangent.y);
    result.tangent.z  = a.tangent.z  + t * (b.tangent.z  - a.tangent.z);
    return result;
}

static int clip_against_plane(const MopSwClipVertex *in, int n,
                              MopSwClipVertex *out, int max_out,
                              const ClipPlane *plane) {
    if (n < 1) return 0;

    int out_count = 0;
    MopSwClipVertex prev = in[n - 1];
    float prev_dist = clip_distance(plane, prev.position);

    for (int i = 0; i < n; i++) {
        MopSwClipVertex curr = in[i];
        float curr_dist = clip_distance(plane, curr.position);

        if (curr_dist >= 0.0f) {
            /* Current vertex is inside */
            if (prev_dist < 0.0f) {
                /* Previous was outside: emit intersection */
                float t = prev_dist / (prev_dist - curr_dist);
                if (out_count < max_out) {
                    out[out_count++] = lerp_vertex(prev, curr, t);
                }
            }
            /* Emit current vertex */
            if (out_count < max_out) {
                out[out_count++] = curr;
            }
        } else if (prev_dist >= 0.0f) {
            /* Current is outside, previous was inside: emit intersection */
            float t = prev_dist / (prev_dist - curr_dist);
            if (out_count < max_out) {
                out[out_count++] = lerp_vertex(prev, curr, t);
            }
        }

        prev = curr;
        prev_dist = curr_dist;
    }

    return out_count;
}

/* Maximum vertices after clipping a triangle against 6 planes */
#define MAX_CLIP_VERTICES 24

int mop_sw_clip_polygon(const MopSwClipVertex *in_vertices, int n,
                         MopSwClipVertex *out_vertices, int max_out) {
    MopSwClipVertex buf_a[MAX_CLIP_VERTICES];
    MopSwClipVertex buf_b[MAX_CLIP_VERTICES];

    /* Copy input to buf_a */
    int count = (n > MAX_CLIP_VERTICES) ? MAX_CLIP_VERTICES : n;
    memcpy(buf_a, in_vertices, (size_t)count * sizeof(MopSwClipVertex));

    MopSwClipVertex *src = buf_a;
    MopSwClipVertex *dst = buf_b;

    for (int p = 0; p < 6; p++) {
        count = clip_against_plane(src, count, dst, MAX_CLIP_VERTICES,
                                   &FRUSTUM_PLANES[p]);
        if (count == 0) return 0;

        /* Swap buffers */
        MopSwClipVertex *tmp = src;
        src = dst;
        dst = tmp;
    }

    /* Result is in src */
    int out_count = (count > max_out) ? max_out : count;
    memcpy(out_vertices, src, (size_t)out_count * sizeof(MopSwClipVertex));
    return out_count;
}

/* -------------------------------------------------------------------------
 * Bresenham line drawing
 * ------------------------------------------------------------------------- */

void mop_sw_draw_line(MopSwFramebuffer *fb,
                       int x0, int y0, float z0,
                       int x1, int y1, float z1,
                       uint8_t r, uint8_t g, uint8_t b,
                       uint32_t object_id, bool depth_test) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int steps = (dx > dy) ? dx : dy;
    if (steps == 0) steps = 1;

    for (int i = 0; i <= steps; i++) {
        if (x0 >= 0 && x0 < fb->width && y0 >= 0 && y0 < fb->height) {
            float t = (steps > 0) ? (float)i / (float)steps : 0.0f;
            float z = z0 + t * (z1 - z0);
            size_t idx = (size_t)y0 * (size_t)fb->width + (size_t)x0;

            if (!depth_test || z < fb->depth[idx]) {
                fb->color[idx * 4 + 0] = r;
                fb->color[idx * 4 + 1] = g;
                fb->color[idx * 4 + 2] = b;
                fb->color[idx * 4 + 3] = 255;
                fb->depth[idx]         = z;
                fb->object_id[idx]     = object_id;
            }
        }

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* -------------------------------------------------------------------------
 * Half-space triangle rasterization
 *
 * After perspective division and viewport transform, the three vertices
 * are in screen coordinates.  We compute edge functions and iterate over
 * the bounding box.
 * ------------------------------------------------------------------------- */

static float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static void rasterize_filled_triangle(MopSwFramebuffer *fb,
                                      float sx0, float sy0, float sz0,
                                      float sx1, float sy1, float sz1,
                                      float sx2, float sy2, float sz2,
                                      uint8_t cr, uint8_t cg, uint8_t cb,
                                      uint8_t ca,
                                      uint32_t object_id, bool depth_test,
                                      MopBlendMode blend_mode) {
    /* Bounding box */
    float fmin_x = sx0; if (sx1 < fmin_x) fmin_x = sx1; if (sx2 < fmin_x) fmin_x = sx2;
    float fmin_y = sy0; if (sy1 < fmin_y) fmin_y = sy1; if (sy2 < fmin_y) fmin_y = sy2;
    float fmax_x = sx0; if (sx1 > fmax_x) fmax_x = sx1; if (sx2 > fmax_x) fmax_x = sx2;
    float fmax_y = sy0; if (sy1 > fmax_y) fmax_y = sy1; if (sy2 > fmax_y) fmax_y = sy2;

    int min_x = (int)floorf(fmin_x);
    int min_y = (int)floorf(fmin_y);
    int max_x = (int)ceilf(fmax_x);
    int max_y = (int)ceilf(fmax_y);

    /* Clamp to framebuffer */
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fb->width)  max_x = fb->width - 1;
    if (max_y >= fb->height) max_y = fb->height - 1;

    /* Degenerate check */
    if (min_x > max_x || min_y > max_y) return;

    float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
    if (fabsf(area) < 1e-6f) return;

    /* Handle both CW and CCW winding: if CW (area < 0), negate edge
       values so the standard >= 0 inside test works uniformly. */
    bool flip = (area < 0.0f);
    float inv_area = 1.0f / fabsf(area);

    /* Incremental edge function coefficients:
     *   e0 = edge(v1→v2), e1 = edge(v2→v0), e2 = edge(v0→v1)
     *   dx = ∂edge/∂x, dy = ∂edge/∂y */
    float e0_dx = sy1 - sy2, e0_dy = sx2 - sx1;
    float e1_dx = sy2 - sy0, e1_dy = sx0 - sx2;
    float e2_dx = sy0 - sy1, e2_dy = sx1 - sx0;

    if (flip) {
        e0_dx = -e0_dx; e0_dy = -e0_dy;
        e1_dx = -e1_dx; e1_dy = -e1_dy;
        e2_dx = -e2_dx; e2_dy = -e2_dy;
    }

    /* Evaluate edge functions at top-left pixel center */
    float px0 = (float)min_x + 0.5f;
    float py0 = (float)min_y + 0.5f;

    float w0_row = (sx2 - sx1) * (py0 - sy1) - (sy2 - sy1) * (px0 - sx1);
    float w1_row = (sx0 - sx2) * (py0 - sy2) - (sy0 - sy2) * (px0 - sx2);
    float w2_row = (sx1 - sx0) * (py0 - sy0) - (sy1 - sy0) * (px0 - sx0);

    if (flip) { w0_row = -w0_row; w1_row = -w1_row; w2_row = -w2_row; }

    /* Incremental depth: z = (w0*sz0 + w1*sz1 + w2*sz2) * inv_area */
    float z_dx  = (e0_dx * sz0 + e1_dx * sz1 + e2_dx * sz2) * inv_area;
    float z_dy  = (e0_dy * sz0 + e1_dy * sz1 + e2_dy * sz2) * inv_area;
    float z_row = (w0_row * sz0 + w1_row * sz1 + w2_row * sz2) * inv_area;

    int width = fb->width;

    if (blend_mode == MOP_BLEND_OPAQUE && ca == 255) {
        /* ── Opaque fast path ── */
        for (int y = min_y; y <= max_y; y++) {
            float w0 = w0_row, w1 = w1_row, w2 = w2_row;
            float z = z_row;
            size_t row = (size_t)y * (size_t)width;

            for (int x = min_x; x <= max_x; x++) {
                if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                    size_t idx = row + (size_t)x;
                    if (!depth_test || z < fb->depth[idx]) {
                        fb->color[idx * 4 + 0] = cr;
                        fb->color[idx * 4 + 1] = cg;
                        fb->color[idx * 4 + 2] = cb;
                        fb->color[idx * 4 + 3] = 255;
                        fb->depth[idx]     = z;
                        fb->object_id[idx] = object_id;
                    }
                }
                w0 += e0_dx; w1 += e1_dx; w2 += e2_dx;
                z += z_dx;
            }
            w0_row += e0_dy; w1_row += e1_dy; w2_row += e2_dy;
            z_row += z_dy;
        }
    } else {
        /* ── Blended path: ALPHA, ADDITIVE, MULTIPLY ── */
        float a_f   = (float)ca / 255.0f;
        float inv_a = 1.0f - a_f;

        for (int y = min_y; y <= max_y; y++) {
            float w0 = w0_row, w1 = w1_row, w2 = w2_row;
            float z = z_row;
            size_t row = (size_t)y * (size_t)width;

            for (int x = min_x; x <= max_x; x++) {
                if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                    size_t idx = row + (size_t)x;
                    if (!depth_test || z < fb->depth[idx]) {
                        size_t ci = idx * 4;
                        uint8_t dr = fb->color[ci + 0];
                        uint8_t dg = fb->color[ci + 1];
                        uint8_t db = fb->color[ci + 2];
                        uint8_t or_, og, ob;

                        switch (blend_mode) {
                        case MOP_BLEND_ADDITIVE: {
                            int ar = (int)dr + (int)(cr * a_f);
                            int ag = (int)dg + (int)(cg * a_f);
                            int ab = (int)db + (int)(cb * a_f);
                            or_ = (uint8_t)(ar > 255 ? 255 : ar);
                            og  = (uint8_t)(ag > 255 ? 255 : ag);
                            ob  = (uint8_t)(ab > 255 ? 255 : ab);
                            break;
                        }
                        case MOP_BLEND_MULTIPLY:
                            or_ = (uint8_t)((dr * cr) / 255);
                            og  = (uint8_t)((dg * cg) / 255);
                            ob  = (uint8_t)((db * cb) / 255);
                            break;
                        default: /* MOP_BLEND_ALPHA / MOP_BLEND_OPAQUE with alpha < 255 */
                            or_ = (uint8_t)(cr * a_f + dr * inv_a);
                            og  = (uint8_t)(cg * a_f + dg * inv_a);
                            ob  = (uint8_t)(cb * a_f + db * inv_a);
                            break;
                        }

                        fb->color[ci + 0] = or_;
                        fb->color[ci + 1] = og;
                        fb->color[ci + 2] = ob;
                        fb->color[ci + 3] = 255;
                    }
                }
                w0 += e0_dx; w1 += e1_dx; w2 += e2_dx;
                z += z_dx;
            }
            w0_row += e0_dy; w1_row += e1_dy; w2_row += e2_dy;
            z_row += z_dy;
        }
    }
}

/* -------------------------------------------------------------------------
 * Triangle rasterization entry point
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle(const MopSwClipVertex vertices[3],
                                uint32_t object_id,
                                bool wireframe,
                                bool depth_test,
                                bool cull_back,
                                MopVec3 light_dir,
                                float ambient,
                                float opacity,
                                bool smooth_shading,
                                MopBlendMode blend_mode,
                                MopSwFramebuffer *fb) {
    MopVec4 a = vertices[0].position;
    MopVec4 b = vertices[1].position;
    MopVec4 c = vertices[2].position;

    /* Trivial frustum reject: all 3 vertices outside the same plane */
    if ((a.x < -a.w && b.x < -b.w && c.x < -c.w) ||
        (a.x >  a.w && b.x >  b.w && c.x >  c.w) ||
        (a.y < -a.w && b.y < -b.w && c.y < -c.w) ||
        (a.y >  a.w && b.y >  b.w && c.y >  c.w) ||
        (a.z < -a.w && b.z < -b.w && c.z < -c.w) ||
        (a.z >  a.w && b.z >  b.w && c.z >  c.w))
        return;

    /* Early backface cull in clip space (before expensive clipping).
     * When all w > 0, the homogeneous cross product has the same sign as
     * the NDC cross product.  Front-facing = CCW in NDC = positive cross.
     * Back-facing = CW in NDC = negative cross.  Reject <= 0. */
    if (cull_back && a.w > 0.0f && b.w > 0.0f && c.w > 0.0f) {
        float ex = b.x * a.w - a.x * b.w;
        float ey = b.y * a.w - a.y * b.w;
        float fx = c.x * a.w - a.x * c.w;
        float fy = c.y * a.w - a.y * c.w;
        if (ex * fy - ey * fx <= 0.0f) return;
    }

    /* Trivial accept: all 3 vertices inside all 6 frustum planes.
     * When true, skip the expensive Sutherland-Hodgman clipping.
     * This is the common case for most visible triangles. */
    const MopSwClipVertex *poly;
    MopSwClipVertex clipped[MAX_CLIP_VERTICES];
    int poly_count;

    if (a.w > 0.0f && b.w > 0.0f && c.w > 0.0f &&
        a.x >= -a.w && a.x <= a.w &&
        a.y >= -a.w && a.y <= a.w &&
        a.z >= -a.w && a.z <= a.w &&
        b.x >= -b.w && b.x <= b.w &&
        b.y >= -b.w && b.y <= b.w &&
        b.z >= -b.w && b.z <= b.w &&
        c.x >= -c.w && c.x <= c.w &&
        c.y >= -c.w && c.y <= c.w &&
        c.z >= -c.w && c.z <= c.w) {
        poly = vertices;
        poly_count = 3;
    } else {
        poly_count = mop_sw_clip_polygon(vertices, 3, clipped,
                                          MAX_CLIP_VERTICES);
        if (poly_count < 3) return;
        poly = clipped;
    }

    /* Hoist invariants out of the triangle fan loop */
    MopVec3 norm_light = mop_vec3_normalize(light_dir);
    float half_w = (float)fb->width  * 0.5f;
    float half_h = (float)fb->height * 0.5f;
    uint8_t ca = (uint8_t)(clamp01(opacity) * 255.0f);

    /* Process polygon as a triangle fan */
    for (int i = 1; i < poly_count - 1; i++) {
        const MopSwClipVertex *v0 = &poly[0];
        const MopSwClipVertex *v1 = &poly[i];
        const MopSwClipVertex *v2 = &poly[i + 1];

        /* Perspective division */
        if (fabsf(v0->position.w) < 1e-7f ||
            fabsf(v1->position.w) < 1e-7f ||
            fabsf(v2->position.w) < 1e-7f) {
            continue;
        }

        float inv_w0 = 1.0f / v0->position.w;
        float inv_w1 = 1.0f / v1->position.w;
        float inv_w2 = 1.0f / v2->position.w;

        /* NDC + viewport transform (combined) */
        float sx0 = (v0->position.x * inv_w0 + 1.0f) * half_w;
        float sy0 = (1.0f - v0->position.y * inv_w0) * half_h;
        float sz0 = (v0->position.z * inv_w0 + 1.0f) * 0.5f;
        float sx1 = (v1->position.x * inv_w1 + 1.0f) * half_w;
        float sy1 = (1.0f - v1->position.y * inv_w1) * half_h;
        float sz1 = (v1->position.z * inv_w1 + 1.0f) * 0.5f;
        float sx2 = (v2->position.x * inv_w2 + 1.0f) * half_w;
        float sy2 = (1.0f - v2->position.y * inv_w2) * half_h;
        float sz2 = (v2->position.z * inv_w2 + 1.0f) * 0.5f;

        /* Backface culling in screen space */
        float signed_area = (sx1 - sx0) * (sy2 - sy0) -
                            (sx2 - sx0) * (sy1 - sy0);
        if (cull_back && signed_area >= 0.0f) {
            continue;
        }

        /* Smooth shading path — dispatch to per-pixel normal interpolation */
        if (smooth_shading && !wireframe) {
            MopSwScreenVertex sv[3] = {
                { sx0, sy0, sz0, v0->normal, v0->color, v0->u, v0->v,
                  v0->tangent },
                { sx1, sy1, sz1, v1->normal, v1->color, v1->u, v1->v,
                  v1->tangent },
                { sx2, sy2, sz2, v2->normal, v2->color, v2->u, v2->v,
                  v2->tangent },
            };
            mop_sw_rasterize_triangle_smooth(sv, object_id, depth_test,
                                              light_dir, ambient, opacity,
                                              blend_mode, fb);
            continue;
        }

        /* Flat shading */
        MopVec3 face_normal = mop_vec3_normalize((MopVec3){
            (v0->normal.x + v1->normal.x + v2->normal.x),
            (v0->normal.y + v1->normal.y + v2->normal.y),
            (v0->normal.z + v1->normal.z + v2->normal.z)
        });

        float ndotl = mop_vec3_dot(face_normal, norm_light);
        if (ndotl < 0.0f) ndotl = 0.0f;
        float lighting = clamp01(ambient + (1.0f - ambient) * ndotl);

        float avg_r = (v0->color.r + v1->color.r + v2->color.r) * (1.0f / 3.0f);
        float avg_g = (v0->color.g + v1->color.g + v2->color.g) * (1.0f / 3.0f);
        float avg_b = (v0->color.b + v1->color.b + v2->color.b) * (1.0f / 3.0f);

        uint8_t cr = (uint8_t)(clamp01(avg_r * lighting) * 255.0f);
        uint8_t cg = (uint8_t)(clamp01(avg_g * lighting) * 255.0f);
        uint8_t cb = (uint8_t)(clamp01(avg_b * lighting) * 255.0f);

        if (wireframe) {
            mop_sw_draw_line(fb,
                (int)sx0, (int)sy0, sz0,
                (int)sx1, (int)sy1, sz1,
                cr, cg, cb, object_id, depth_test);
            mop_sw_draw_line(fb,
                (int)sx1, (int)sy1, sz1,
                (int)sx2, (int)sy2, sz2,
                cr, cg, cb, object_id, depth_test);
            mop_sw_draw_line(fb,
                (int)sx2, (int)sy2, sz2,
                (int)sx0, (int)sy0, sz0,
                cr, cg, cb, object_id, depth_test);
        } else {
            rasterize_filled_triangle(fb,
                sx0, sy0, sz0,
                sx1, sy1, sz1,
                sx2, sy2, sz2,
                cr, cg, cb, ca,
                object_id, depth_test, blend_mode);
        }
    }
}

/* -------------------------------------------------------------------------
 * Smooth-shaded triangle rasterization (Gouraud)
 *
 * Like rasterize_filled_triangle but interpolates normals and colors
 * per-pixel using barycentric coordinates.
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle_smooth(const MopSwScreenVertex verts[3],
                                       uint32_t object_id,
                                       bool depth_test,
                                       MopVec3 light_dir,
                                       float ambient,
                                       float opacity,
                                       MopBlendMode blend_mode,
                                       MopSwFramebuffer *fb) {
    float sx0 = verts[0].sx, sy0 = verts[0].sy, sz0 = verts[0].sz;
    float sx1 = verts[1].sx, sy1 = verts[1].sy, sz1 = verts[1].sz;
    float sx2 = verts[2].sx, sy2 = verts[2].sy, sz2 = verts[2].sz;

    /* Bounding box */
    float fmin_x = sx0; if (sx1 < fmin_x) fmin_x = sx1; if (sx2 < fmin_x) fmin_x = sx2;
    float fmin_y = sy0; if (sy1 < fmin_y) fmin_y = sy1; if (sy2 < fmin_y) fmin_y = sy2;
    float fmax_x = sx0; if (sx1 > fmax_x) fmax_x = sx1; if (sx2 > fmax_x) fmax_x = sx2;
    float fmax_y = sy0; if (sy1 > fmax_y) fmax_y = sy1; if (sy2 > fmax_y) fmax_y = sy2;

    int min_x = (int)floorf(fmin_x);
    int min_y = (int)floorf(fmin_y);
    int max_x = (int)ceilf(fmax_x);
    int max_y = (int)ceilf(fmax_y);

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fb->width)  max_x = fb->width - 1;
    if (max_y >= fb->height) max_y = fb->height - 1;
    if (min_x > max_x || min_y > max_y) return;

    float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
    if (fabsf(area) < 1e-6f) return;

    bool flip = (area < 0.0f);
    float inv_area = 1.0f / fabsf(area);

    /* Edge function increments */
    float e0_dx = sy1 - sy2, e0_dy = sx2 - sx1;
    float e1_dx = sy2 - sy0, e1_dy = sx0 - sx2;
    float e2_dx = sy0 - sy1, e2_dy = sx1 - sx0;

    if (flip) {
        e0_dx = -e0_dx; e0_dy = -e0_dy;
        e1_dx = -e1_dx; e1_dy = -e1_dy;
        e2_dx = -e2_dx; e2_dy = -e2_dy;
    }

    float px0 = (float)min_x + 0.5f;
    float py0 = (float)min_y + 0.5f;

    float w0_row = (sx2 - sx1) * (py0 - sy1) - (sy2 - sy1) * (px0 - sx1);
    float w1_row = (sx0 - sx2) * (py0 - sy2) - (sy0 - sy2) * (px0 - sx2);
    float w2_row = (sx1 - sx0) * (py0 - sy0) - (sy1 - sy0) * (px0 - sx0);

    if (flip) { w0_row = -w0_row; w1_row = -w1_row; w2_row = -w2_row; }

    MopVec3 nl = mop_vec3_normalize(light_dir);
    uint8_t ca = (uint8_t)(clamp01(opacity) * 255.0f);
    float a_f   = (float)ca / 255.0f;
    float inv_a = 1.0f - a_f;
    int width = fb->width;

    for (int y = min_y; y <= max_y; y++) {
        float w0 = w0_row, w1 = w1_row, w2 = w2_row;

        for (int x = min_x; x <= max_x; x++) {
            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                float b0 = w0 * inv_area;
                float b1 = w1 * inv_area;
                float b2 = w2 * inv_area;

                float z = b0 * sz0 + b1 * sz1 + b2 * sz2;
                size_t idx = (size_t)y * (size_t)width + (size_t)x;

                if (!depth_test || z < fb->depth[idx]) {
                    /* Interpolate normal */
                    MopVec3 n = {
                        b0 * verts[0].normal.x + b1 * verts[1].normal.x + b2 * verts[2].normal.x,
                        b0 * verts[0].normal.y + b1 * verts[1].normal.y + b2 * verts[2].normal.y,
                        b0 * verts[0].normal.z + b1 * verts[1].normal.z + b2 * verts[2].normal.z
                    };
                    n = mop_vec3_normalize(n);

                    /* Interpolate color */
                    float cr = b0*verts[0].color.r + b1*verts[1].color.r + b2*verts[2].color.r;
                    float cg = b0*verts[0].color.g + b1*verts[1].color.g + b2*verts[2].color.g;
                    float cb = b0*verts[0].color.b + b1*verts[1].color.b + b2*verts[2].color.b;

                    float ndotl = mop_vec3_dot(n, nl);
                    if (ndotl < 0.0f) ndotl = 0.0f;
                    float lit = clamp01(ambient + (1.0f - ambient) * ndotl);

                    uint8_t pr = (uint8_t)(clamp01(cr * lit) * 255.0f);
                    uint8_t pg = (uint8_t)(clamp01(cg * lit) * 255.0f);
                    uint8_t pb = (uint8_t)(clamp01(cb * lit) * 255.0f);

                    size_t ci = idx * 4;
                    if (blend_mode == MOP_BLEND_OPAQUE && ca == 255) {
                        fb->color[ci + 0] = pr;
                        fb->color[ci + 1] = pg;
                        fb->color[ci + 2] = pb;
                        fb->color[ci + 3] = 255;
                        fb->depth[idx]     = z;
                        fb->object_id[idx] = object_id;
                    } else {
                        uint8_t dr = fb->color[ci + 0];
                        uint8_t dg = fb->color[ci + 1];
                        uint8_t db = fb->color[ci + 2];
                        switch (blend_mode) {
                        case MOP_BLEND_ADDITIVE: {
                            int ar = (int)dr + (int)(pr * a_f);
                            int ag = (int)dg + (int)(pg * a_f);
                            int ab = (int)db + (int)(pb * a_f);
                            fb->color[ci + 0] = (uint8_t)(ar > 255 ? 255 : ar);
                            fb->color[ci + 1] = (uint8_t)(ag > 255 ? 255 : ag);
                            fb->color[ci + 2] = (uint8_t)(ab > 255 ? 255 : ab);
                            break;
                        }
                        case MOP_BLEND_MULTIPLY:
                            fb->color[ci + 0] = (uint8_t)((dr * pr) / 255);
                            fb->color[ci + 1] = (uint8_t)((dg * pg) / 255);
                            fb->color[ci + 2] = (uint8_t)((db * pb) / 255);
                            break;
                        default:
                            fb->color[ci + 0] = (uint8_t)(pr * a_f + dr * inv_a);
                            fb->color[ci + 1] = (uint8_t)(pg * a_f + dg * inv_a);
                            fb->color[ci + 2] = (uint8_t)(pb * a_f + db * inv_a);
                            break;
                        }
                        fb->color[ci + 3] = 255;
                    }
                }
            }
            w0 += e0_dx; w1 += e1_dx; w2 += e2_dx;
        }
        w0_row += e0_dy; w1_row += e1_dy; w2_row += e2_dy;
    }
}

/* -------------------------------------------------------------------------
 * Multi-light contribution helper
 *
 * Accumulates diffuse lighting from all active lights in the array.
 * Returns a total light intensity multiplier.
 * ------------------------------------------------------------------------- */

static float compute_multi_light(MopVec3 normal, MopVec3 world_pos,
                                  const MopLight *lights, uint32_t light_count,
                                  float ambient) {
    float total = ambient;

    for (uint32_t i = 0; i < light_count; i++) {
        if (!lights[i].active) continue;

        float ndotl = 0.0f;
        float attenuation = 1.0f;
        float spot_factor = 1.0f;

        switch (lights[i].type) {
        case MOP_LIGHT_DIRECTIONAL: {
            MopVec3 dir = mop_vec3_normalize(lights[i].direction);
            ndotl = mop_vec3_dot(normal, dir);
            break;
        }
        case MOP_LIGHT_POINT: {
            MopVec3 to_light = mop_vec3_sub(lights[i].position, world_pos);
            float dist = mop_vec3_length(to_light);
            if (dist < 1e-6f) dist = 1e-6f;
            MopVec3 dir = mop_vec3_scale(to_light, 1.0f / dist);
            ndotl = mop_vec3_dot(normal, dir);

            /* Distance attenuation */
            if (lights[i].range > 0.0f) {
                float r = dist / lights[i].range;
                attenuation = 1.0f - r;
                if (attenuation < 0.0f) attenuation = 0.0f;
                attenuation *= attenuation;
            } else {
                /* Inverse-square falloff */
                attenuation = 1.0f / (1.0f + dist * dist);
            }
            break;
        }
        case MOP_LIGHT_SPOT: {
            MopVec3 to_light = mop_vec3_sub(lights[i].position, world_pos);
            float dist = mop_vec3_length(to_light);
            if (dist < 1e-6f) dist = 1e-6f;
            MopVec3 dir = mop_vec3_scale(to_light, 1.0f / dist);
            ndotl = mop_vec3_dot(normal, dir);

            /* Spot cone */
            MopVec3 spot_dir = mop_vec3_normalize(lights[i].direction);
            float cos_angle = -mop_vec3_dot(dir, spot_dir);
            if (cos_angle < lights[i].spot_outer_cos) {
                spot_factor = 0.0f;
            } else if (cos_angle < lights[i].spot_inner_cos) {
                float range = lights[i].spot_inner_cos - lights[i].spot_outer_cos;
                if (range > 1e-6f) {
                    spot_factor = (cos_angle - lights[i].spot_outer_cos) / range;
                }
            }

            /* Distance attenuation */
            if (lights[i].range > 0.0f) {
                float r = dist / lights[i].range;
                attenuation = 1.0f - r;
                if (attenuation < 0.0f) attenuation = 0.0f;
                attenuation *= attenuation;
            } else {
                attenuation = 1.0f / (1.0f + dist * dist);
            }
            break;
        }
        }

        if (ndotl < 0.0f) ndotl = 0.0f;
        total += ndotl * lights[i].intensity * attenuation * spot_factor;
    }

    return clamp01(total);
}

/* -------------------------------------------------------------------------
 * Smooth-shaded triangle rasterization with multi-light support
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle_smooth_ml(const MopSwScreenVertex verts[3],
                                          uint32_t object_id,
                                          bool depth_test,
                                          MopVec3 light_dir,
                                          float ambient,
                                          float opacity,
                                          MopBlendMode blend_mode,
                                          const MopLight *lights,
                                          uint32_t light_count,
                                          MopVec3 cam_eye,
                                          MopSwFramebuffer *fb) {
    /* If no multi-light, fall back to standard smooth */
    if (!lights || light_count == 0) {
        mop_sw_rasterize_triangle_smooth(verts, object_id, depth_test,
                                          light_dir, ambient, opacity,
                                          blend_mode, fb);
        return;
    }

    float sx0 = verts[0].sx, sy0 = verts[0].sy, sz0 = verts[0].sz;
    float sx1 = verts[1].sx, sy1 = verts[1].sy, sz1 = verts[1].sz;
    float sx2 = verts[2].sx, sy2 = verts[2].sy, sz2 = verts[2].sz;

    /* Bounding box */
    float fmin_x = sx0; if (sx1 < fmin_x) fmin_x = sx1; if (sx2 < fmin_x) fmin_x = sx2;
    float fmin_y = sy0; if (sy1 < fmin_y) fmin_y = sy1; if (sy2 < fmin_y) fmin_y = sy2;
    float fmax_x = sx0; if (sx1 > fmax_x) fmax_x = sx1; if (sx2 > fmax_x) fmax_x = sx2;
    float fmax_y = sy0; if (sy1 > fmax_y) fmax_y = sy1; if (sy2 > fmax_y) fmax_y = sy2;

    int min_x = (int)floorf(fmin_x);
    int min_y = (int)floorf(fmin_y);
    int max_x = (int)ceilf(fmax_x);
    int max_y = (int)ceilf(fmax_y);

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fb->width)  max_x = fb->width - 1;
    if (max_y >= fb->height) max_y = fb->height - 1;
    if (min_x > max_x || min_y > max_y) return;

    float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
    if (fabsf(area) < 1e-6f) return;

    bool flip = (area < 0.0f);
    float inv_area = 1.0f / fabsf(area);

    float e0_dx = sy1 - sy2, e0_dy = sx2 - sx1;
    float e1_dx = sy2 - sy0, e1_dy = sx0 - sx2;
    float e2_dx = sy0 - sy1, e2_dy = sx1 - sx0;

    if (flip) {
        e0_dx = -e0_dx; e0_dy = -e0_dy;
        e1_dx = -e1_dx; e1_dy = -e1_dy;
        e2_dx = -e2_dx; e2_dy = -e2_dy;
    }

    float px0 = (float)min_x + 0.5f;
    float py0 = (float)min_y + 0.5f;

    float w0_row = (sx2 - sx1) * (py0 - sy1) - (sy2 - sy1) * (px0 - sx1);
    float w1_row = (sx0 - sx2) * (py0 - sy2) - (sy0 - sy2) * (px0 - sx2);
    float w2_row = (sx1 - sx0) * (py0 - sy0) - (sy1 - sy0) * (px0 - sx0);

    if (flip) { w0_row = -w0_row; w1_row = -w1_row; w2_row = -w2_row; }

    uint8_t ca = (uint8_t)(clamp01(opacity) * 255.0f);
    float a_f   = (float)ca / 255.0f;
    float inv_a = 1.0f - a_f;
    int width = fb->width;

    (void)cam_eye; /* reserved for specular in Phase 2 */

    for (int y = min_y; y <= max_y; y++) {
        float w0 = w0_row, w1 = w1_row, w2 = w2_row;

        for (int x = min_x; x <= max_x; x++) {
            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                float b0 = w0 * inv_area;
                float b1 = w1 * inv_area;
                float b2 = w2 * inv_area;

                float z = b0 * sz0 + b1 * sz1 + b2 * sz2;
                size_t idx = (size_t)y * (size_t)width + (size_t)x;

                if (!depth_test || z < fb->depth[idx]) {
                    /* Interpolate normal */
                    MopVec3 n = {
                        b0 * verts[0].normal.x + b1 * verts[1].normal.x + b2 * verts[2].normal.x,
                        b0 * verts[0].normal.y + b1 * verts[1].normal.y + b2 * verts[2].normal.y,
                        b0 * verts[0].normal.z + b1 * verts[1].normal.z + b2 * verts[2].normal.z
                    };
                    n = mop_vec3_normalize(n);

                    /* Interpolate color */
                    float cr = b0*verts[0].color.r + b1*verts[1].color.r + b2*verts[2].color.r;
                    float cg = b0*verts[0].color.g + b1*verts[1].color.g + b2*verts[2].color.g;
                    float cb = b0*verts[0].color.b + b1*verts[1].color.b + b2*verts[2].color.b;

                    /* Multi-light accumulation */
                    MopVec3 world_pos = { 0, 0, 0 }; /* approximate — fine for directional */
                    float lit = compute_multi_light(n, world_pos, lights,
                                                    light_count, ambient);

                    uint8_t pr = (uint8_t)(clamp01(cr * lit) * 255.0f);
                    uint8_t pg = (uint8_t)(clamp01(cg * lit) * 255.0f);
                    uint8_t pb = (uint8_t)(clamp01(cb * lit) * 255.0f);

                    size_t ci = idx * 4;
                    if (blend_mode == MOP_BLEND_OPAQUE && ca == 255) {
                        fb->color[ci + 0] = pr;
                        fb->color[ci + 1] = pg;
                        fb->color[ci + 2] = pb;
                        fb->color[ci + 3] = 255;
                        fb->depth[idx]     = z;
                        fb->object_id[idx] = object_id;
                    } else {
                        uint8_t dr = fb->color[ci + 0];
                        uint8_t dg = fb->color[ci + 1];
                        uint8_t db = fb->color[ci + 2];
                        switch (blend_mode) {
                        case MOP_BLEND_ADDITIVE: {
                            int ar = (int)dr + (int)(pr * a_f);
                            int ag = (int)dg + (int)(pg * a_f);
                            int ab = (int)db + (int)(pb * a_f);
                            fb->color[ci + 0] = (uint8_t)(ar > 255 ? 255 : ar);
                            fb->color[ci + 1] = (uint8_t)(ag > 255 ? 255 : ag);
                            fb->color[ci + 2] = (uint8_t)(ab > 255 ? 255 : ab);
                            break;
                        }
                        case MOP_BLEND_MULTIPLY:
                            fb->color[ci + 0] = (uint8_t)((dr * pr) / 255);
                            fb->color[ci + 1] = (uint8_t)((dg * pg) / 255);
                            fb->color[ci + 2] = (uint8_t)((db * pb) / 255);
                            break;
                        default:
                            fb->color[ci + 0] = (uint8_t)(pr * a_f + dr * inv_a);
                            fb->color[ci + 1] = (uint8_t)(pg * a_f + dg * inv_a);
                            fb->color[ci + 2] = (uint8_t)(pb * a_f + db * inv_a);
                            break;
                        }
                        fb->color[ci + 3] = 255;
                    }
                }
            }
            w0 += e0_dx; w1 += e1_dx; w2 += e2_dx;
        }
        w0_row += e0_dy; w1_row += e1_dy; w2_row += e2_dy;
    }
}

/* -------------------------------------------------------------------------
 * Smooth-shaded triangle rasterization with normal mapping
 *
 * Same as mop_sw_rasterize_triangle_smooth, but additionally constructs
 * a TBN (tangent-bitangent-normal) matrix per pixel, samples the normal
 * map, and transforms the tangent-space normal to world space for lighting.
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle_smooth_nm(const MopSwScreenVertex verts[3],
                                          uint32_t object_id,
                                          bool depth_test,
                                          MopVec3 light_dir,
                                          float ambient,
                                          float opacity,
                                          MopBlendMode blend_mode,
                                          const MopSwNormalMap *normal_map,
                                          MopSwFramebuffer *fb) {
    /* If no normal map, fall back to standard smooth shading */
    if (!normal_map || !normal_map->data) {
        mop_sw_rasterize_triangle_smooth(verts, object_id, depth_test,
                                          light_dir, ambient, opacity,
                                          blend_mode, fb);
        return;
    }

    float sx0 = verts[0].sx, sy0 = verts[0].sy, sz0 = verts[0].sz;
    float sx1 = verts[1].sx, sy1 = verts[1].sy, sz1 = verts[1].sz;
    float sx2 = verts[2].sx, sy2 = verts[2].sy, sz2 = verts[2].sz;

    /* Bounding box */
    float fmin_x = sx0; if (sx1 < fmin_x) fmin_x = sx1; if (sx2 < fmin_x) fmin_x = sx2;
    float fmin_y = sy0; if (sy1 < fmin_y) fmin_y = sy1; if (sy2 < fmin_y) fmin_y = sy2;
    float fmax_x = sx0; if (sx1 > fmax_x) fmax_x = sx1; if (sx2 > fmax_x) fmax_x = sx2;
    float fmax_y = sy0; if (sy1 > fmax_y) fmax_y = sy1; if (sy2 > fmax_y) fmax_y = sy2;

    int min_x = (int)floorf(fmin_x);
    int min_y = (int)floorf(fmin_y);
    int max_x = (int)ceilf(fmax_x);
    int max_y = (int)ceilf(fmax_y);

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fb->width)  max_x = fb->width - 1;
    if (max_y >= fb->height) max_y = fb->height - 1;
    if (min_x > max_x || min_y > max_y) return;

    float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
    if (fabsf(area) < 1e-6f) return;

    bool flip = (area < 0.0f);
    float inv_area = 1.0f / fabsf(area);

    float e0_dx = sy1 - sy2, e0_dy = sx2 - sx1;
    float e1_dx = sy2 - sy0, e1_dy = sx0 - sx2;
    float e2_dx = sy0 - sy1, e2_dy = sx1 - sx0;

    if (flip) {
        e0_dx = -e0_dx; e0_dy = -e0_dy;
        e1_dx = -e1_dx; e1_dy = -e1_dy;
        e2_dx = -e2_dx; e2_dy = -e2_dy;
    }

    float px0 = (float)min_x + 0.5f;
    float py0 = (float)min_y + 0.5f;

    float w0_row = (sx2 - sx1) * (py0 - sy1) - (sy2 - sy1) * (px0 - sx1);
    float w1_row = (sx0 - sx2) * (py0 - sy2) - (sy0 - sy2) * (px0 - sx2);
    float w2_row = (sx1 - sx0) * (py0 - sy0) - (sy1 - sy0) * (px0 - sx0);

    if (flip) { w0_row = -w0_row; w1_row = -w1_row; w2_row = -w2_row; }

    MopVec3 nl = mop_vec3_normalize(light_dir);
    uint8_t ca = (uint8_t)(clamp01(opacity) * 255.0f);
    float a_f   = (float)ca / 255.0f;
    float inv_a = 1.0f - a_f;
    int width = fb->width;
    int nm_w = normal_map->width;
    int nm_h = normal_map->height;

    for (int y = min_y; y <= max_y; y++) {
        float w0 = w0_row, w1 = w1_row, w2 = w2_row;

        for (int x = min_x; x <= max_x; x++) {
            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                float b0 = w0 * inv_area;
                float b1 = w1 * inv_area;
                float b2 = w2 * inv_area;

                float z = b0 * sz0 + b1 * sz1 + b2 * sz2;
                size_t idx = (size_t)y * (size_t)width + (size_t)x;

                if (!depth_test || z < fb->depth[idx]) {
                    /* Interpolate normal */
                    MopVec3 n = {
                        b0 * verts[0].normal.x + b1 * verts[1].normal.x + b2 * verts[2].normal.x,
                        b0 * verts[0].normal.y + b1 * verts[1].normal.y + b2 * verts[2].normal.y,
                        b0 * verts[0].normal.z + b1 * verts[1].normal.z + b2 * verts[2].normal.z
                    };
                    n = mop_vec3_normalize(n);

                    /* Interpolate tangent */
                    MopVec3 t_vec = {
                        b0 * verts[0].tangent.x + b1 * verts[1].tangent.x + b2 * verts[2].tangent.x,
                        b0 * verts[0].tangent.y + b1 * verts[1].tangent.y + b2 * verts[2].tangent.y,
                        b0 * verts[0].tangent.z + b1 * verts[1].tangent.z + b2 * verts[2].tangent.z
                    };
                    t_vec = mop_vec3_normalize(t_vec);

                    /* Compute bitangent = cross(normal, tangent) */
                    MopVec3 bitan = mop_vec3_cross(n, t_vec);
                    bitan = mop_vec3_normalize(bitan);

                    /* Sample normal map at interpolated UV */
                    float uv_u = b0 * verts[0].u + b1 * verts[1].u + b2 * verts[2].u;
                    float uv_v = b0 * verts[0].v + b1 * verts[1].v + b2 * verts[2].v;
                    uv_u = uv_u - floorf(uv_u);
                    uv_v = uv_v - floorf(uv_v);

                    int nm_x = (int)(uv_u * (float)(nm_w - 1) + 0.5f);
                    int nm_y = (int)(uv_v * (float)(nm_h - 1) + 0.5f);
                    if (nm_x < 0) nm_x = 0;
                    if (nm_x >= nm_w) nm_x = nm_w - 1;
                    if (nm_y < 0) nm_y = 0;
                    if (nm_y >= nm_h) nm_y = nm_h - 1;

                    size_t nm_idx = ((size_t)nm_y * (size_t)nm_w + (size_t)nm_x) * 4;
                    /* Decode normal map: [0,255] -> [-1,1] */
                    float nm_nx = (float)normal_map->data[nm_idx + 0] / 127.5f - 1.0f;
                    float nm_ny = (float)normal_map->data[nm_idx + 1] / 127.5f - 1.0f;
                    float nm_nz = (float)normal_map->data[nm_idx + 2] / 127.5f - 1.0f;

                    /* Transform tangent-space normal to world space via TBN matrix
                     * world_normal = T * nm_nx + B * nm_ny + N * nm_nz */
                    MopVec3 perturbed = {
                        t_vec.x * nm_nx + bitan.x * nm_ny + n.x * nm_nz,
                        t_vec.y * nm_nx + bitan.y * nm_ny + n.y * nm_nz,
                        t_vec.z * nm_nx + bitan.z * nm_ny + n.z * nm_nz
                    };
                    perturbed = mop_vec3_normalize(perturbed);

                    /* Interpolate color */
                    float cr = b0*verts[0].color.r + b1*verts[1].color.r + b2*verts[2].color.r;
                    float cg = b0*verts[0].color.g + b1*verts[1].color.g + b2*verts[2].color.g;
                    float cb = b0*verts[0].color.b + b1*verts[1].color.b + b2*verts[2].color.b;

                    /* Lighting with perturbed normal */
                    float ndotl = mop_vec3_dot(perturbed, nl);
                    if (ndotl < 0.0f) ndotl = 0.0f;
                    float lit = clamp01(ambient + (1.0f - ambient) * ndotl);

                    uint8_t pr = (uint8_t)(clamp01(cr * lit) * 255.0f);
                    uint8_t pg = (uint8_t)(clamp01(cg * lit) * 255.0f);
                    uint8_t pb = (uint8_t)(clamp01(cb * lit) * 255.0f);

                    size_t ci = idx * 4;
                    if (blend_mode == MOP_BLEND_OPAQUE && ca == 255) {
                        fb->color[ci + 0] = pr;
                        fb->color[ci + 1] = pg;
                        fb->color[ci + 2] = pb;
                        fb->color[ci + 3] = 255;
                        fb->depth[idx]     = z;
                        fb->object_id[idx] = object_id;
                    } else {
                        uint8_t dr = fb->color[ci + 0];
                        uint8_t dg = fb->color[ci + 1];
                        uint8_t db = fb->color[ci + 2];
                        switch (blend_mode) {
                        case MOP_BLEND_ADDITIVE: {
                            int ar = (int)dr + (int)(pr * a_f);
                            int ag = (int)dg + (int)(pg * a_f);
                            int ab = (int)db + (int)(pb * a_f);
                            fb->color[ci + 0] = (uint8_t)(ar > 255 ? 255 : ar);
                            fb->color[ci + 1] = (uint8_t)(ag > 255 ? 255 : ag);
                            fb->color[ci + 2] = (uint8_t)(ab > 255 ? 255 : ab);
                            break;
                        }
                        case MOP_BLEND_MULTIPLY:
                            fb->color[ci + 0] = (uint8_t)((dr * pr) / 255);
                            fb->color[ci + 1] = (uint8_t)((dg * pg) / 255);
                            fb->color[ci + 2] = (uint8_t)((db * pb) / 255);
                            break;
                        default:
                            fb->color[ci + 0] = (uint8_t)(pr * a_f + dr * inv_a);
                            fb->color[ci + 1] = (uint8_t)(pg * a_f + dg * inv_a);
                            fb->color[ci + 2] = (uint8_t)(pb * a_f + db * inv_a);
                            break;
                        }
                        fb->color[ci + 3] = 255;
                    }
                }
            }
            w0 += e0_dx; w1 += e1_dx; w2 += e2_dx;
        }
        w0_row += e0_dy; w1_row += e1_dy; w2_row += e2_dy;
    }
}

/* -------------------------------------------------------------------------
 * Full triangle rasterization with multi-light support
 *
 * Same structure as mop_sw_rasterize_triangle but dispatches to the
 * multi-light smooth shading path when lights are available, and uses
 * compute_multi_light() for flat shading with multiple lights.
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle_full(const MopSwClipVertex vertices[3],
                                     uint32_t object_id,
                                     bool wireframe, bool depth_test,
                                     bool cull_back,
                                     MopVec3 light_dir, float ambient,
                                     float opacity,
                                     bool smooth_shading,
                                     MopBlendMode blend_mode,
                                     const MopLight *lights,
                                     uint32_t light_count,
                                     MopVec3 cam_eye,
                                     MopSwFramebuffer *fb) {
    MopVec4 a = vertices[0].position;
    MopVec4 b = vertices[1].position;
    MopVec4 c = vertices[2].position;

    /* Trivial frustum reject */
    if ((a.x < -a.w && b.x < -b.w && c.x < -c.w) ||
        (a.x >  a.w && b.x >  b.w && c.x >  c.w) ||
        (a.y < -a.w && b.y < -b.w && c.y < -c.w) ||
        (a.y >  a.w && b.y >  b.w && c.y >  c.w) ||
        (a.z < -a.w && b.z < -b.w && c.z < -c.w) ||
        (a.z >  a.w && b.z >  b.w && c.z >  c.w))
        return;

    /* Early backface cull in clip space */
    if (cull_back && a.w > 0.0f && b.w > 0.0f && c.w > 0.0f) {
        float ex = b.x * a.w - a.x * b.w;
        float ey = b.y * a.w - a.y * b.w;
        float fx = c.x * a.w - a.x * c.w;
        float fy = c.y * a.w - a.y * c.w;
        if (ex * fy - ey * fx <= 0.0f) return;
    }

    /* Clip */
    const MopSwClipVertex *poly;
    MopSwClipVertex clipped[MAX_CLIP_VERTICES];
    int poly_count;

    if (a.w > 0.0f && b.w > 0.0f && c.w > 0.0f &&
        a.x >= -a.w && a.x <= a.w &&
        a.y >= -a.w && a.y <= a.w &&
        a.z >= -a.w && a.z <= a.w &&
        b.x >= -b.w && b.x <= b.w &&
        b.y >= -b.w && b.y <= b.w &&
        b.z >= -b.w && b.z <= b.w &&
        c.x >= -c.w && c.x <= c.w &&
        c.y >= -c.w && c.y <= c.w &&
        c.z >= -c.w && c.z <= c.w) {
        poly = vertices;
        poly_count = 3;
    } else {
        poly_count = mop_sw_clip_polygon(vertices, 3, clipped,
                                          MAX_CLIP_VERTICES);
        if (poly_count < 3) return;
        poly = clipped;
    }

    MopVec3 norm_light = mop_vec3_normalize(light_dir);
    float half_w = (float)fb->width  * 0.5f;
    float half_h = (float)fb->height * 0.5f;
    uint8_t ca = (uint8_t)(clamp01(opacity) * 255.0f);

    for (int i = 1; i < poly_count - 1; i++) {
        const MopSwClipVertex *v0 = &poly[0];
        const MopSwClipVertex *v1 = &poly[i];
        const MopSwClipVertex *v2 = &poly[i + 1];

        if (fabsf(v0->position.w) < 1e-7f ||
            fabsf(v1->position.w) < 1e-7f ||
            fabsf(v2->position.w) < 1e-7f) {
            continue;
        }

        float inv_w0 = 1.0f / v0->position.w;
        float inv_w1 = 1.0f / v1->position.w;
        float inv_w2 = 1.0f / v2->position.w;

        float sx0 = (v0->position.x * inv_w0 + 1.0f) * half_w;
        float sy0 = (1.0f - v0->position.y * inv_w0) * half_h;
        float sz0 = (v0->position.z * inv_w0 + 1.0f) * 0.5f;
        float sx1 = (v1->position.x * inv_w1 + 1.0f) * half_w;
        float sy1 = (1.0f - v1->position.y * inv_w1) * half_h;
        float sz1 = (v1->position.z * inv_w1 + 1.0f) * 0.5f;
        float sx2 = (v2->position.x * inv_w2 + 1.0f) * half_w;
        float sy2 = (1.0f - v2->position.y * inv_w2) * half_h;
        float sz2 = (v2->position.z * inv_w2 + 1.0f) * 0.5f;

        float signed_area = (sx1 - sx0) * (sy2 - sy0) -
                            (sx2 - sx0) * (sy1 - sy0);
        if (cull_back && signed_area >= 0.0f) {
            continue;
        }

        /* Smooth shading with multi-light */
        if (smooth_shading && !wireframe) {
            MopSwScreenVertex sv[3] = {
                { sx0, sy0, sz0, v0->normal, v0->color, v0->u, v0->v,
                  v0->tangent },
                { sx1, sy1, sz1, v1->normal, v1->color, v1->u, v1->v,
                  v1->tangent },
                { sx2, sy2, sz2, v2->normal, v2->color, v2->u, v2->v,
                  v2->tangent },
            };
            mop_sw_rasterize_triangle_smooth_ml(sv, object_id, depth_test,
                                                 light_dir, ambient, opacity,
                                                 blend_mode, lights,
                                                 light_count, cam_eye, fb);
            continue;
        }

        /* Flat shading with multi-light */
        MopVec3 face_normal = mop_vec3_normalize((MopVec3){
            (v0->normal.x + v1->normal.x + v2->normal.x),
            (v0->normal.y + v1->normal.y + v2->normal.y),
            (v0->normal.z + v1->normal.z + v2->normal.z)
        });

        float lighting;
        if (lights && light_count > 0) {
            MopVec3 world_pos = { 0, 0, 0 }; /* approximate for flat */
            lighting = compute_multi_light(face_normal, world_pos,
                                            lights, light_count, ambient);
        } else {
            float ndotl = mop_vec3_dot(face_normal, norm_light);
            if (ndotl < 0.0f) ndotl = 0.0f;
            lighting = clamp01(ambient + (1.0f - ambient) * ndotl);
        }

        float avg_r = (v0->color.r + v1->color.r + v2->color.r) * (1.0f / 3.0f);
        float avg_g = (v0->color.g + v1->color.g + v2->color.g) * (1.0f / 3.0f);
        float avg_b = (v0->color.b + v1->color.b + v2->color.b) * (1.0f / 3.0f);

        uint8_t cr = (uint8_t)(clamp01(avg_r * lighting) * 255.0f);
        uint8_t cg = (uint8_t)(clamp01(avg_g * lighting) * 255.0f);
        uint8_t cb = (uint8_t)(clamp01(avg_b * lighting) * 255.0f);

        if (wireframe) {
            mop_sw_draw_line(fb,
                (int)sx0, (int)sy0, sz0,
                (int)sx1, (int)sy1, sz1,
                cr, cg, cb, object_id, depth_test);
            mop_sw_draw_line(fb,
                (int)sx1, (int)sy1, sz1,
                (int)sx2, (int)sy2, sz2,
                cr, cg, cb, object_id, depth_test);
            mop_sw_draw_line(fb,
                (int)sx2, (int)sy2, sz2,
                (int)sx0, (int)sy0, sz0,
                cr, cg, cb, object_id, depth_test);
        } else {
            rasterize_filled_triangle(fb,
                sx0, sy0, sz0,
                sx1, sy1, sz1,
                sx2, sy2, sz2,
                cr, cg, cb, ca,
                object_id, depth_test, blend_mode);
        }
    }
}
