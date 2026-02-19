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

static float edge_function(float ax, float ay, float bx, float by,
                           float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

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
                                      uint32_t object_id, bool depth_test) {
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

    float area = edge_function(sx0, sy0, sx1, sy1, sx2, sy2);
    if (fabsf(area) < 1e-6f) return;

    /* Handle both CW and CCW winding: if CW (area < 0), negate edge
       values so the standard >= 0 inside test works uniformly. */
    bool flip = (area < 0.0f);
    float inv_area = 1.0f / fabsf(area);

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;

            float w0 = edge_function(sx1, sy1, sx2, sy2, px, py);
            float w1 = edge_function(sx2, sy2, sx0, sy0, px, py);
            float w2 = edge_function(sx0, sy0, sx1, sy1, px, py);

            if (flip) { w0 = -w0; w1 = -w1; w2 = -w2; }

            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                w0 *= inv_area;
                w1 *= inv_area;
                w2 *= inv_area;

                /* Interpolate depth */
                float z = w0 * sz0 + w1 * sz1 + w2 * sz2;

                size_t idx = (size_t)y * (size_t)fb->width + (size_t)x;

                if (!depth_test || z < fb->depth[idx]) {
                    fb->color[idx * 4 + 0] = cr;
                    fb->color[idx * 4 + 1] = cg;
                    fb->color[idx * 4 + 2] = cb;
                    fb->color[idx * 4 + 3] = 255;
                    fb->depth[idx]         = z;
                    fb->object_id[idx]     = object_id;
                }
            }
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
                                MopSwFramebuffer *fb) {
    /* Clip the triangle */
    MopSwClipVertex clipped[MAX_CLIP_VERTICES];
    int clipped_count = mop_sw_clip_polygon(vertices, 3, clipped,
                                             MAX_CLIP_VERTICES);
    if (clipped_count < 3) return;

    /* Process clipped polygon as a triangle fan */
    for (int i = 1; i < clipped_count - 1; i++) {
        MopSwClipVertex v0 = clipped[0];
        MopSwClipVertex v1 = clipped[i];
        MopSwClipVertex v2 = clipped[i + 1];

        /* Perspective division */
        if (fabsf(v0.position.w) < 1e-7f ||
            fabsf(v1.position.w) < 1e-7f ||
            fabsf(v2.position.w) < 1e-7f) {
            continue;
        }

        float inv_w0 = 1.0f / v0.position.w;
        float inv_w1 = 1.0f / v1.position.w;
        float inv_w2 = 1.0f / v2.position.w;

        /* NDC coordinates [-1, 1] */
        float ndc_x0 = v0.position.x * inv_w0;
        float ndc_y0 = v0.position.y * inv_w0;
        float ndc_z0 = v0.position.z * inv_w0;
        float ndc_x1 = v1.position.x * inv_w1;
        float ndc_y1 = v1.position.y * inv_w1;
        float ndc_z1 = v1.position.z * inv_w1;
        float ndc_x2 = v2.position.x * inv_w2;
        float ndc_y2 = v2.position.y * inv_w2;
        float ndc_z2 = v2.position.z * inv_w2;

        /* Viewport transform: NDC -> screen pixels
         * x_screen = (ndc_x + 1) * 0.5 * width
         * y_screen = (1 - ndc_y) * 0.5 * height  (flip Y for top-left origin)
         * z_screen = (ndc_z + 1) * 0.5  (map to [0, 1])
         */
        float half_w = (float)fb->width  * 0.5f;
        float half_h = (float)fb->height * 0.5f;

        float sx0 = (ndc_x0 + 1.0f) * half_w;
        float sy0 = (1.0f - ndc_y0) * half_h;
        float sz0 = (ndc_z0 + 1.0f) * 0.5f;
        float sx1 = (ndc_x1 + 1.0f) * half_w;
        float sy1 = (1.0f - ndc_y1) * half_h;
        float sz1 = (ndc_z1 + 1.0f) * 0.5f;
        float sx2 = (ndc_x2 + 1.0f) * half_w;
        float sy2 = (1.0f - ndc_y2) * half_h;
        float sz2 = (ndc_z2 + 1.0f) * 0.5f;

        /* Backface culling: check screen-space winding order
         * Positive signed area = CCW = front-facing */
        float signed_area = (sx1 - sx0) * (sy2 - sy0) -
                            (sx2 - sx0) * (sy1 - sy0);
        if (cull_back && signed_area >= 0.0f) {
            continue;  /* Back-facing: CW in screen space after Y flip */
        }

        /* Flat shading: compute face normal from first triangle's vertices */
        MopVec3 face_normal = mop_vec3_normalize((MopVec3){
            (v0.normal.x + v1.normal.x + v2.normal.x) / 3.0f,
            (v0.normal.y + v1.normal.y + v2.normal.y) / 3.0f,
            (v0.normal.z + v1.normal.z + v2.normal.z) / 3.0f
        });

        MopVec3 norm_light = mop_vec3_normalize(light_dir);
        float ndotl = mop_vec3_dot(face_normal, norm_light);
        if (ndotl < 0.0f) ndotl = 0.0f;

        /* Ambient + diffuse lighting */
        float ambient  = 0.2f;
        float diffuse  = 0.8f * ndotl;
        float lighting = clamp01(ambient + diffuse);

        /* Average vertex colors for flat shading */
        float avg_r = (v0.color.r + v1.color.r + v2.color.r) / 3.0f;
        float avg_g = (v0.color.g + v1.color.g + v2.color.g) / 3.0f;
        float avg_b = (v0.color.b + v1.color.b + v2.color.b) / 3.0f;

        uint8_t cr = (uint8_t)(clamp01(avg_r * lighting) * 255.0f);
        uint8_t cg = (uint8_t)(clamp01(avg_g * lighting) * 255.0f);
        uint8_t cb = (uint8_t)(clamp01(avg_b * lighting) * 255.0f);

        if (wireframe) {
            /* Draw triangle edges */
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
                cr, cg, cb,
                object_id, depth_test);
        }
    }
}
