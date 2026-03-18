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
#include <math.h>
#include <mop/util/log.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------
 * Framebuffer management
 * ------------------------------------------------------------------------- */

bool mop_sw_framebuffer_alloc(MopSwFramebuffer *fb, int width, int height) {
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
    MOP_ERROR("framebuffer dimensions out of range: %dx%d", width, height);
    return false;
  }

  size_t pixel_count = (size_t)width * (size_t)height;

  fb->width = width;
  fb->height = height;

  fb->color = calloc(pixel_count * 4, sizeof(uint8_t));
  if (!fb->color)
    goto fail_color;

  fb->color_hdr = calloc(pixel_count * 4, sizeof(float));
  if (!fb->color_hdr)
    goto fail_hdr;

  fb->depth = malloc(pixel_count * sizeof(float));
  if (!fb->depth)
    goto fail_depth;

  fb->object_id = calloc(pixel_count, sizeof(uint32_t));
  if (!fb->object_id)
    goto fail_id;

  fb->fxaa_scratch = malloc(pixel_count * 4 * sizeof(uint8_t));
  if (!fb->fxaa_scratch)
    goto fail_fxaa;

  return true;

fail_fxaa:
  free(fb->object_id);
  fb->object_id = NULL;

fail_id:
  free(fb->depth);
  fb->depth = NULL;
fail_depth:
  free(fb->color_hdr);
  fb->color_hdr = NULL;
fail_hdr:
  free(fb->color);
  fb->color = NULL;
fail_color:
  fb->width = 0;
  fb->height = 0;
  return false;
}

void mop_sw_framebuffer_free(MopSwFramebuffer *fb) {
  free(fb->color);
  free(fb->color_hdr);
  free(fb->depth);
  free(fb->object_id);
  free(fb->fxaa_scratch);
  fb->color = NULL;
  fb->color_hdr = NULL;
  fb->depth = NULL;
  fb->object_id = NULL;
  fb->fxaa_scratch = NULL;
  fb->width = 0;
  fb->height = 0;
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
    fb->color_hdr[i * 4 + 0] = clear_color.r;
    fb->color_hdr[i * 4 + 1] = clear_color.g;
    fb->color_hdr[i * 4 + 2] = clear_color.b;
    fb->color_hdr[i * 4 + 3] = clear_color.a;
    fb->depth[i] = 1.0f;
    fb->object_id[i] = 0;
  }
}

/* -------------------------------------------------------------------------
 * Shadow map state — set before the main render so that lighting functions
 * can automatically test directional-light shadows.
 * ------------------------------------------------------------------------- */

static const float *s_shadow_depth = NULL;
static int s_shadow_w = 0;
static int s_shadow_h = 0;
static MopMat4 s_shadow_vp;

void mop_sw_shadow_set(const float *depth, int w, int h, MopMat4 light_vp) {
  s_shadow_depth = depth;
  s_shadow_w = w;
  s_shadow_h = h;
  s_shadow_vp = light_vp;
}

void mop_sw_shadow_clear(void) {
  s_shadow_depth = NULL;
  s_shadow_w = 0;
  s_shadow_h = 0;
}

/* ---- IBL (Image-Based Lighting) state ---- */

typedef struct MopSwIBLState {
  const float *irradiance; /* RGBA float, equirectangular */
  int irr_w, irr_h;
  const float *prefiltered; /* RGBA float, mip levels concatenated */
  int pf_w, pf_h, pf_levels;
  const float *brdf_lut; /* RGBA float, NdotV x roughness */
  int brdf_size;
  float rotation;  /* environment Y-axis rotation */
  float intensity; /* brightness multiplier */
} MopSwIBLState;

static MopSwIBLState s_ibl = {0};

void mop_sw_ibl_set(const float *irradiance, int irr_w, int irr_h,
                    const float *prefiltered, int pf_w, int pf_h, int pf_levels,
                    const float *brdf_lut, int brdf_size, float rotation,
                    float intensity) {
  s_ibl.irradiance = irradiance;
  s_ibl.irr_w = irr_w;
  s_ibl.irr_h = irr_h;
  s_ibl.prefiltered = prefiltered;
  s_ibl.pf_w = pf_w;
  s_ibl.pf_h = pf_h;
  s_ibl.pf_levels = pf_levels;
  s_ibl.brdf_lut = brdf_lut;
  s_ibl.brdf_size = brdf_size;
  s_ibl.rotation = rotation;
  s_ibl.intensity = intensity;
}

void mop_sw_ibl_clear(void) { memset(&s_ibl, 0, sizeof(s_ibl)); }

/* Sample equirectangular map bilinearly */
static void ibl_sample(const float *data, int w, int h, float u, float v,
                       float out[3]) {
  u = u - floorf(u);
  v = v - floorf(v);
  float fx = u * (float)(w - 1);
  float fy = v * (float)(h - 1);
  int x0 = (int)fx, y0 = (int)fy;
  int x1 = (x0 + 1) % w;
  int y1 = y0 + 1 < h ? y0 + 1 : h - 1;
  float sx = fx - (float)x0, sy = fy - (float)y0;
  const float *p00 = &data[((size_t)y0 * w + x0) * 4];
  const float *p10 = &data[((size_t)y0 * w + x1) * 4];
  const float *p01 = &data[((size_t)y1 * w + x0) * 4];
  const float *p11 = &data[((size_t)y1 * w + x1) * 4];
  for (int c = 0; c < 3; c++) {
    float top = p00[c] * (1.0f - sx) + p10[c] * sx;
    float bot = p01[c] * (1.0f - sx) + p11[c] * sx;
    out[c] = top * (1.0f - sy) + bot * sy;
  }
}

/* Direction → equirectangular UV */
static void dir_to_uv(float dx, float dy, float dz, float rot, float *u,
                      float *v) {
  float phi = atan2f(dz, dx) + rot;
  float cdy = dy < -1.0f ? -1.0f : (dy > 1.0f ? 1.0f : dy);
  *u = phi / (2.0f * 3.14159265f) + 0.5f;
  *v = asinf(cdy) / 3.14159265f + 0.5f;
}

/* Sample irradiance at world-space normal */
static void ibl_irradiance(MopVec3 n, float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  if (!s_ibl.irradiance)
    return;
  float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
  if (len < 1e-6f)
    return;
  float u, v;
  dir_to_uv(n.x / len, n.y / len, n.z / len, s_ibl.rotation, &u, &v);
  ibl_sample(s_ibl.irradiance, s_ibl.irr_w, s_ibl.irr_h, u, v, out);
  out[0] *= s_ibl.intensity;
  out[1] *= s_ibl.intensity;
  out[2] *= s_ibl.intensity;
}

/* Sample prefiltered env at reflection direction + roughness */
static void ibl_prefiltered(MopVec3 refl, float roughness, float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  if (!s_ibl.prefiltered)
    return;
  float len = sqrtf(refl.x * refl.x + refl.y * refl.y + refl.z * refl.z);
  if (len < 1e-6f)
    return;

  int level = (int)(roughness * (float)(s_ibl.pf_levels - 1));
  if (level >= s_ibl.pf_levels)
    level = s_ibl.pf_levels - 1;

  size_t offset = 0;
  for (int l = 0; l < level; l++) {
    int lw = s_ibl.pf_w >> l;
    if (lw < 1)
      lw = 1;
    int lh = s_ibl.pf_h >> l;
    if (lh < 1)
      lh = 1;
    offset += (size_t)lw * lh;
  }
  int lw = s_ibl.pf_w >> level;
  if (lw < 1)
    lw = 1;
  int lh = s_ibl.pf_h >> level;
  if (lh < 1)
    lh = 1;

  float u, v;
  dir_to_uv(refl.x / len, refl.y / len, refl.z / len, s_ibl.rotation, &u, &v);
  ibl_sample(s_ibl.prefiltered + offset * 4, lw, lh, u, v, out);
  out[0] *= s_ibl.intensity;
  out[1] *= s_ibl.intensity;
  out[2] *= s_ibl.intensity;
}

/* Sample BRDF LUT (NdotV, roughness) → (scale, bias) */
static void ibl_brdf(float ndotv, float roughness, float *scale, float *bias) {
  *scale = 1.0f;
  *bias = 0.0f;
  if (!s_ibl.brdf_lut)
    return;
  if (ndotv < 0.0f)
    ndotv = 0.0f;
  if (ndotv > 1.0f)
    ndotv = 1.0f;
  if (roughness < 0.0f)
    roughness = 0.0f;
  if (roughness > 1.0f)
    roughness = 1.0f;
  int x = (int)(ndotv * (float)(s_ibl.brdf_size - 1) + 0.5f);
  int y = (int)(roughness * (float)(s_ibl.brdf_size - 1) + 0.5f);
  if (x >= s_ibl.brdf_size)
    x = s_ibl.brdf_size - 1;
  if (y >= s_ibl.brdf_size)
    y = s_ibl.brdf_size - 1;
  size_t idx = ((size_t)y * s_ibl.brdf_size + x) * 4;
  *scale = s_ibl.brdf_lut[idx + 0];
  *bias = s_ibl.brdf_lut[idx + 1];
}

/* PCF (Percentage Closer Filtering) shadow test — 3x3 kernel for soft edges.
 * Returns value in [0, 1]. */
static float shadow_test_pcf(MopVec3 world_pos) {
  if (!s_shadow_depth)
    return 1.0f;

  MopVec4 p = {world_pos.x, world_pos.y, world_pos.z, 1.0f};
  MopVec4 lp = mop_mat4_mul_vec4(s_shadow_vp, p);

  if (lp.w <= 0.0f)
    return 1.0f;

  float inv_w = 1.0f / lp.w;
  float ndcx = lp.x * inv_w;
  float ndcy = lp.y * inv_w;
  float ndcz = (lp.z * inv_w + 1.0f) * 0.5f;

  float u = (ndcx + 1.0f) * 0.5f * (float)s_shadow_w;
  float v = (1.0f - ndcy) * 0.5f * (float)s_shadow_h;

  int cx = (int)u;
  int cy = (int)v;

  float bias = 0.005f;
  int lit = 0;
  int total = 0;

  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      int sx = cx + dx;
      int sy = cy + dy;
      if (sx < 0 || sx >= s_shadow_w || sy < 0 || sy >= s_shadow_h) {
        lit++;
        total++;
        continue;
      }
      float shadow_z =
          s_shadow_depth[(size_t)sy * (size_t)s_shadow_w + (size_t)sx];
      if (ndcz <= shadow_z + bias)
        lit++;
      total++;
    }
  }

  return (float)lit / (float)total;
}

/* -------------------------------------------------------------------------
 * Shadow map depth-only rendering
 *
 * Rasterizes mesh geometry into a depth-only framebuffer from the light's
 * perspective.  No color, no object ID, no shading — just depth writes.
 * ------------------------------------------------------------------------- */

void mop_sw_shadow_render_mesh(const MopVertex *vertices, uint32_t vertex_count,
                               const uint32_t *indices, uint32_t index_count,
                               MopMat4 model, MopMat4 light_vp,
                               MopSwFramebuffer *shadow_fb) {
  MopMat4 mvp = mop_mat4_multiply(light_vp, model);
  float half_w = (float)shadow_fb->width * 0.5f;
  float half_h = (float)shadow_fb->height * 0.5f;

  for (uint32_t i = 0; i + 2 < index_count; i += 3) {
    uint32_t i0 = indices[i + 0];
    uint32_t i1 = indices[i + 1];
    uint32_t i2 = indices[i + 2];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
      continue;

    MopVec4 p0 = {vertices[i0].position.x, vertices[i0].position.y,
                  vertices[i0].position.z, 1.0f};
    MopVec4 p1 = {vertices[i1].position.x, vertices[i1].position.y,
                  vertices[i1].position.z, 1.0f};
    MopVec4 p2 = {vertices[i2].position.x, vertices[i2].position.y,
                  vertices[i2].position.z, 1.0f};

    MopVec4 c0 = mop_mat4_mul_vec4(mvp, p0);
    MopVec4 c1 = mop_mat4_mul_vec4(mvp, p1);
    MopVec4 c2 = mop_mat4_mul_vec4(mvp, p2);

    /* Trivial reject */
    if ((c0.x < -c0.w && c1.x < -c1.w && c2.x < -c2.w) ||
        (c0.x > c0.w && c1.x > c1.w && c2.x > c2.w) ||
        (c0.y < -c0.w && c1.y < -c1.w && c2.y < -c2.w) ||
        (c0.y > c0.w && c1.y > c1.w && c2.y > c2.w) ||
        (c0.z < -c0.w && c1.z < -c1.w && c2.z < -c2.w) ||
        (c0.z > c0.w && c1.z > c1.w && c2.z > c2.w))
      continue;

    /* W guard */
    if (c0.w <= 1e-6f || c1.w <= 1e-6f || c2.w <= 1e-6f)
      continue;

    /* Perspective divide + viewport transform */
    float inv0 = 1.0f / c0.w, inv1 = 1.0f / c1.w, inv2 = 1.0f / c2.w;

    float sx0 = (c0.x * inv0 + 1.0f) * half_w;
    float sy0 = (1.0f - c0.y * inv0) * half_h;
    float sz0 = (c0.z * inv0 + 1.0f) * 0.5f;
    float sx1 = (c1.x * inv1 + 1.0f) * half_w;
    float sy1 = (1.0f - c1.y * inv1) * half_h;
    float sz1 = (c1.z * inv1 + 1.0f) * 0.5f;
    float sx2 = (c2.x * inv2 + 1.0f) * half_w;
    float sy2 = (1.0f - c2.y * inv2) * half_h;
    float sz2 = (c2.z * inv2 + 1.0f) * 0.5f;

    /* Bounding box */
    float fmin_x = sx0, fmin_y = sy0;
    float fmax_x = sx0, fmax_y = sy0;
    if (sx1 < fmin_x)
      fmin_x = sx1;
    if (sx1 > fmax_x)
      fmax_x = sx1;
    if (sx2 < fmin_x)
      fmin_x = sx2;
    if (sx2 > fmax_x)
      fmax_x = sx2;
    if (sy1 < fmin_y)
      fmin_y = sy1;
    if (sy1 > fmax_y)
      fmax_y = sy1;
    if (sy2 < fmin_y)
      fmin_y = sy2;
    if (sy2 > fmax_y)
      fmax_y = sy2;

    int min_x = (int)floorf(fmin_x), min_y = (int)floorf(fmin_y);
    int max_x = (int)ceilf(fmax_x), max_y = (int)ceilf(fmax_y);
    if (min_x < 0)
      min_x = 0;
    if (min_y < 0)
      min_y = 0;
    if (max_x >= shadow_fb->width)
      max_x = shadow_fb->width - 1;
    if (max_y >= shadow_fb->height)
      max_y = shadow_fb->height - 1;
    if (min_x > max_x || min_y > max_y)
      continue;

    float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
    if (fabsf(area) < 1e-6f)
      continue;
    float inv_area = 1.0f / fabsf(area);

    /* Edge function for depth-only rasterization (no winding cull) */
    float e0_dx = sy1 - sy2, e0_dy = sx2 - sx1;
    float e1_dx = sy2 - sy0, e1_dy = sx0 - sx2;
    float e2_dx = sy0 - sy1, e2_dy = sx1 - sx0;

    bool flip = (area < 0.0f);
    if (flip) {
      e0_dx = -e0_dx;
      e0_dy = -e0_dy;
      e1_dx = -e1_dx;
      e1_dy = -e1_dy;
      e2_dx = -e2_dx;
      e2_dy = -e2_dy;
    }

    float px0 = (float)min_x + 0.5f;
    float py0 = (float)min_y + 0.5f;
    float w0_row = (sx2 - sx1) * (py0 - sy1) - (sy2 - sy1) * (px0 - sx1);
    float w1_row = (sx0 - sx2) * (py0 - sy2) - (sy0 - sy2) * (px0 - sx2);
    float w2_row = (sx1 - sx0) * (py0 - sy0) - (sy1 - sy0) * (px0 - sx0);
    if (flip) {
      w0_row = -w0_row;
      w1_row = -w1_row;
      w2_row = -w2_row;
    }

    int w = shadow_fb->width;
    for (int y = min_y; y <= max_y; y++) {
      float w0 = w0_row, w1 = w1_row, w2 = w2_row;
      for (int x = min_x; x <= max_x; x++) {
        if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
          float b0 = w0 * inv_area, b1 = w1 * inv_area, b2 = w2 * inv_area;
          float z = b0 * sz0 + b1 * sz1 + b2 * sz2;
          size_t idx = (size_t)y * (size_t)w + (size_t)x;
          if (z < shadow_fb->depth[idx])
            shadow_fb->depth[idx] = z;
        }
        w0 += e0_dx;
        w1 += e1_dx;
        w2 += e2_dx;
      }
      w0_row += e0_dy;
      w1_row += e1_dy;
      w2_row += e2_dy;
    }
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
    {1.0f, 0.0f, 0.0f, 1.0f},  /* +X: w + x >= 0 */
    {-1.0f, 0.0f, 0.0f, 1.0f}, /* -X: w - x >= 0 */
    {0.0f, 1.0f, 0.0f, 1.0f},  /* +Y: w + y >= 0 */
    {0.0f, -1.0f, 0.0f, 1.0f}, /* -Y: w - y >= 0 */
    {0.0f, 0.0f, 1.0f, 1.0f},  /* +Z: w + z >= 0 */
    {0.0f, 0.0f, -1.0f, 1.0f}, /* -Z: w - z >= 0 */
};

static float clip_distance(const ClipPlane *plane, MopVec4 pos) {
  return plane->nx * pos.x + plane->ny * pos.y + plane->nz * pos.z +
         plane->nw * pos.w;
}

static MopSwClipVertex lerp_vertex(MopSwClipVertex a, MopSwClipVertex b,
                                   float t) {
  MopSwClipVertex result;
  result.position.x = a.position.x + t * (b.position.x - a.position.x);
  result.position.y = a.position.y + t * (b.position.y - a.position.y);
  result.position.z = a.position.z + t * (b.position.z - a.position.z);
  result.position.w = a.position.w + t * (b.position.w - a.position.w);
  result.normal.x = a.normal.x + t * (b.normal.x - a.normal.x);
  result.normal.y = a.normal.y + t * (b.normal.y - a.normal.y);
  result.normal.z = a.normal.z + t * (b.normal.z - a.normal.z);
  result.world_pos.x = a.world_pos.x + t * (b.world_pos.x - a.world_pos.x);
  result.world_pos.y = a.world_pos.y + t * (b.world_pos.y - a.world_pos.y);
  result.world_pos.z = a.world_pos.z + t * (b.world_pos.z - a.world_pos.z);
  result.color.r = a.color.r + t * (b.color.r - a.color.r);
  result.color.g = a.color.g + t * (b.color.g - a.color.g);
  result.color.b = a.color.b + t * (b.color.b - a.color.b);
  result.color.a = a.color.a + t * (b.color.a - a.color.a);
  result.u = a.u + t * (b.u - a.u);
  result.v = a.v + t * (b.v - a.v);
  result.tangent.x = a.tangent.x + t * (b.tangent.x - a.tangent.x);
  result.tangent.y = a.tangent.y + t * (b.tangent.y - a.tangent.y);
  result.tangent.z = a.tangent.z + t * (b.tangent.z - a.tangent.z);
  return result;
}

static int clip_against_plane(const MopSwClipVertex *in, int n,
                              MopSwClipVertex *out, int max_out,
                              const ClipPlane *plane) {
  if (n < 1)
    return 0;

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
    if (count == 0)
      return 0;

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

void mop_sw_draw_line(MopSwFramebuffer *fb, int x0, int y0, float z0, int x1,
                      int y1, float z1, uint8_t r, uint8_t g, uint8_t b,
                      uint32_t object_id, bool depth_test) {
  int dx = abs(x1 - x0);
  int dy = abs(y1 - y0);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx - dy;
  int steps = (dx > dy) ? dx : dy;
  if (steps == 0)
    steps = 1;

  for (int i = 0; i <= steps; i++) {
    if (x0 >= 0 && x0 < fb->width && y0 >= 0 && y0 < fb->height) {
      float t = (steps > 0) ? (float)i / (float)steps : 0.0f;
      float z = z0 + t * (z1 - z0);
      size_t idx = (size_t)y0 * (size_t)fb->width + (size_t)x0;

      if (!depth_test || z < fb->depth[idx]) {
        size_t ci = idx * 4;
        fb->color[ci + 0] = r;
        fb->color[ci + 1] = g;
        fb->color[ci + 2] = b;
        fb->color[ci + 3] = 255;
        fb->color_hdr[ci + 0] = (float)r / 255.0f;
        fb->color_hdr[ci + 1] = (float)g / 255.0f;
        fb->color_hdr[ci + 2] = (float)b / 255.0f;
        fb->color_hdr[ci + 3] = 1.0f;
        fb->depth[idx] = z;
        fb->object_id[idx] = object_id;
      }
    }

    if (x0 == x1 && y0 == y1)
      break;
    int e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

/* -------------------------------------------------------------------------
 * Anti-aliased line drawing (Xiaolin Wu algorithm)
 *
 * For thickness <= 1.0, uses Xiaolin Wu's algorithm with fractional
 * coverage blending for smooth edges.
 * For thickness > 1.0, expands the line into a quad and rasterizes with
 * edge feathering for anti-aliased thick lines.
 * ------------------------------------------------------------------------- */

static inline void aa_plot(MopSwFramebuffer *fb, int x, int y, float z,
                           uint8_t r, uint8_t g, uint8_t b, float coverage,
                           uint32_t object_id, bool depth_test) {
  if (x < 0 || x >= fb->width || y < 0 || y >= fb->height)
    return;
  if (coverage < 0.004f)
    return;

  size_t idx = (size_t)y * (size_t)fb->width + (size_t)x;

  if (depth_test && z >= fb->depth[idx])
    return;

  /* Alpha-blend with existing pixel */
  size_t ci = idx * 4;
  float fr = (float)r / 255.0f;
  float fg = (float)g / 255.0f;
  float fbb = (float)b / 255.0f;
  float dr = fb->color_hdr[ci + 0];
  float dg = fb->color_hdr[ci + 1];
  float db = fb->color_hdr[ci + 2];

  float inv = 1.0f - coverage;
  float or_ = fr * coverage + dr * inv;
  float og = fg * coverage + dg * inv;
  float ob = fbb * coverage + db * inv;
  fb->color_hdr[ci + 0] = or_;
  fb->color_hdr[ci + 1] = og;
  fb->color_hdr[ci + 2] = ob;
  fb->color_hdr[ci + 3] = 1.0f;
  float cr = or_ < 0.0f ? 0.0f : (or_ > 1.0f ? 1.0f : or_);
  float cg = og < 0.0f ? 0.0f : (og > 1.0f ? 1.0f : og);
  float cb = ob < 0.0f ? 0.0f : (ob > 1.0f ? 1.0f : ob);
  fb->color[ci + 0] = (uint8_t)(cr * 255.0f);
  fb->color[ci + 1] = (uint8_t)(cg * 255.0f);
  fb->color[ci + 2] = (uint8_t)(cb * 255.0f);
  fb->color[ci + 3] = 255;

  if (coverage > 0.5f) {
    fb->depth[idx] = z;
    fb->object_id[idx] = object_id;
  }
}

static float fpart(float x) { return x - floorf(x); }
static float rfpart(float x) { return 1.0f - fpart(x); }

/* Forward declaration — rasterize_filled_triangle is defined below */
static void rasterize_filled_triangle(MopSwFramebuffer *fb, float sx0,
                                      float sy0, float sz0, float sx1,
                                      float sy1, float sz1, float sx2,
                                      float sy2, float sz2, float cr, float cg,
                                      float cb, float ca, uint32_t object_id,
                                      bool depth_test, MopBlendMode blend_mode);

void mop_sw_draw_line_aa(MopSwFramebuffer *fb, float x0, float y0, float z0,
                         float x1, float y1, float z1, uint8_t r, uint8_t g,
                         uint8_t b, uint32_t object_id, bool depth_test,
                         float thickness) {
  if (thickness > 1.5f) {
    /* Thick line: expand to quad and draw as two triangles.
     * Compute perpendicular direction in screen space. */
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f)
      return;

    float inv_len = 1.0f / len;
    float px = -dy * inv_len * thickness * 0.5f;
    float py = dx * inv_len * thickness * 0.5f;

    /* 4 corners of the quad */
    float qx0 = x0 + px, qy0 = y0 + py;
    float qx1 = x0 - px, qy1 = y0 - py;
    float qx2 = x1 - px, qy2 = y1 - py;
    float qx3 = x1 + px, qy3 = y1 + py;

    /* Rasterize as two filled triangles with alpha blending.
     * Use the line color as a flat color and MOP_BLEND_OPAQUE. */
    float fr = (float)r / 255.0f;
    float fg = (float)g / 255.0f;
    float fbb = (float)b / 255.0f;
    rasterize_filled_triangle(fb, qx0, qy0, z0, qx1, qy1, z0, qx2, qy2, z1, fr,
                              fg, fbb, 1.0f, object_id, depth_test,
                              MOP_BLEND_OPAQUE);
    rasterize_filled_triangle(fb, qx0, qy0, z0, qx2, qy2, z1, qx3, qy3, z1, fr,
                              fg, fbb, 1.0f, object_id, depth_test,
                              MOP_BLEND_OPAQUE);
    return;
  }

  /* Xiaolin Wu anti-aliased line algorithm */
  bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);

  if (steep) {
    /* Swap x and y coordinates */
    float tmp;
    tmp = x0;
    x0 = y0;
    y0 = tmp;
    tmp = x1;
    x1 = y1;
    y1 = tmp;
  }

  if (x0 > x1) {
    float tmp;
    tmp = x0;
    x0 = x1;
    x1 = tmp;
    tmp = y0;
    y0 = y1;
    y1 = tmp;
    tmp = z0;
    z0 = z1;
    z1 = tmp;
  }

  float dx = x1 - x0;
  float dy = y1 - y0;
  float gradient = (dx < 0.001f) ? 1.0f : dy / dx;

  /* First endpoint */
  float xend = roundf(x0);
  float yend = y0 + gradient * (xend - x0);
  float xgap = rfpart(x0 + 0.5f);
  int xpxl1 = (int)xend;
  int ypxl1 = (int)floorf(yend);
  float z_at = z0;

  if (steep) {
    aa_plot(fb, ypxl1, xpxl1, z_at, r, g, b, rfpart(yend) * xgap, object_id,
            depth_test);
    aa_plot(fb, ypxl1 + 1, xpxl1, z_at, r, g, b, fpart(yend) * xgap, object_id,
            depth_test);
  } else {
    aa_plot(fb, xpxl1, ypxl1, z_at, r, g, b, rfpart(yend) * xgap, object_id,
            depth_test);
    aa_plot(fb, xpxl1, ypxl1 + 1, z_at, r, g, b, fpart(yend) * xgap, object_id,
            depth_test);
  }

  float intery = yend + gradient;

  /* Second endpoint */
  xend = roundf(x1);
  yend = y1 + gradient * (xend - x1);
  xgap = fpart(x1 + 0.5f);
  int xpxl2 = (int)xend;
  int ypxl2 = (int)floorf(yend);
  float z_end = z1;

  if (steep) {
    aa_plot(fb, ypxl2, xpxl2, z_end, r, g, b, rfpart(yend) * xgap, object_id,
            depth_test);
    aa_plot(fb, ypxl2 + 1, xpxl2, z_end, r, g, b, fpart(yend) * xgap, object_id,
            depth_test);
  } else {
    aa_plot(fb, xpxl2, ypxl2, z_end, r, g, b, rfpart(yend) * xgap, object_id,
            depth_test);
    aa_plot(fb, xpxl2, ypxl2 + 1, z_end, r, g, b, fpart(yend) * xgap, object_id,
            depth_test);
  }

  /* Main loop */
  float total_steps = (float)(xpxl2 - xpxl1);
  if (total_steps < 1.0f)
    total_steps = 1.0f;

  for (int x = xpxl1 + 1; x < xpxl2; x++) {
    float t = (float)(x - xpxl1) / total_steps;
    float z = z0 + t * (z1 - z0);
    int iy = (int)floorf(intery);

    if (steep) {
      aa_plot(fb, iy, x, z, r, g, b, rfpart(intery), object_id, depth_test);
      aa_plot(fb, iy + 1, x, z, r, g, b, fpart(intery), object_id, depth_test);
    } else {
      aa_plot(fb, x, iy, z, r, g, b, rfpart(intery), object_id, depth_test);
      aa_plot(fb, x, iy + 1, z, r, g, b, fpart(intery), object_id, depth_test);
    }
    intery += gradient;
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
  if (x < 0.0f)
    return 0.0f;
  if (x > 1.0f)
    return 1.0f;
  return x;
}

static void rasterize_filled_triangle(
    MopSwFramebuffer *fb, float sx0, float sy0, float sz0, float sx1, float sy1,
    float sz1, float sx2, float sy2, float sz2, float cr, float cg, float cb,
    float ca, uint32_t object_id, bool depth_test, MopBlendMode blend_mode) {
  /* Bounding box */
  float fmin_x = sx0;
  if (sx1 < fmin_x)
    fmin_x = sx1;
  if (sx2 < fmin_x)
    fmin_x = sx2;
  float fmin_y = sy0;
  if (sy1 < fmin_y)
    fmin_y = sy1;
  if (sy2 < fmin_y)
    fmin_y = sy2;
  float fmax_x = sx0;
  if (sx1 > fmax_x)
    fmax_x = sx1;
  if (sx2 > fmax_x)
    fmax_x = sx2;
  float fmax_y = sy0;
  if (sy1 > fmax_y)
    fmax_y = sy1;
  if (sy2 > fmax_y)
    fmax_y = sy2;

  int min_x = (int)floorf(fmin_x);
  int min_y = (int)floorf(fmin_y);
  int max_x = (int)ceilf(fmax_x);
  int max_y = (int)ceilf(fmax_y);

  /* Clamp to framebuffer */
  if (min_x < 0)
    min_x = 0;
  if (min_y < 0)
    min_y = 0;
  if (max_x >= fb->width)
    max_x = fb->width - 1;
  if (max_y >= fb->height)
    max_y = fb->height - 1;

  /* Degenerate check */
  if (min_x > max_x || min_y > max_y)
    return;

  float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
  if (fabsf(area) < 1e-6f)
    return;

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
    e0_dx = -e0_dx;
    e0_dy = -e0_dy;
    e1_dx = -e1_dx;
    e1_dy = -e1_dy;
    e2_dx = -e2_dx;
    e2_dy = -e2_dy;
  }

  /* Evaluate edge functions at top-left pixel center */
  float px0 = (float)min_x + 0.5f;
  float py0 = (float)min_y + 0.5f;

  float w0_row = (sx2 - sx1) * (py0 - sy1) - (sy2 - sy1) * (px0 - sx1);
  float w1_row = (sx0 - sx2) * (py0 - sy2) - (sy0 - sy2) * (px0 - sx2);
  float w2_row = (sx1 - sx0) * (py0 - sy0) - (sy1 - sy0) * (px0 - sx0);

  if (flip) {
    w0_row = -w0_row;
    w1_row = -w1_row;
    w2_row = -w2_row;
  }

  /* Incremental depth: z = (w0*sz0 + w1*sz1 + w2*sz2) * inv_area */
  float z_dx = (e0_dx * sz0 + e1_dx * sz1 + e2_dx * sz2) * inv_area;
  float z_dy = (e0_dy * sz0 + e1_dy * sz1 + e2_dy * sz2) * inv_area;
  float z_row = (w0_row * sz0 + w1_row * sz1 + w2_row * sz2) * inv_area;

  int width = fb->width;

  /* Clamp source color for uint8 output (HDR values stay unclamped) */
  uint8_t cr8 =
      (uint8_t)((cr < 0.0f ? 0.0f : (cr > 1.0f ? 1.0f : cr)) * 255.0f);
  uint8_t cg8 =
      (uint8_t)((cg < 0.0f ? 0.0f : (cg > 1.0f ? 1.0f : cg)) * 255.0f);
  uint8_t cb8 =
      (uint8_t)((cb < 0.0f ? 0.0f : (cb > 1.0f ? 1.0f : cb)) * 255.0f);

  if (blend_mode == MOP_BLEND_OPAQUE && ca >= 1.0f) {
    /* ── Opaque path — strict interior test ──
     * No per-triangle edge AA; FXAA post-process handles silhouette
     * anti-aliasing.  This avoids seam artifacts at shared mesh edges. */
    for (int y = min_y; y <= max_y; y++) {
      float w0 = w0_row, w1 = w1_row, w2 = w2_row;
      float z = z_row;
      size_t row = (size_t)y * (size_t)width;

      for (int x = min_x; x <= max_x; x++) {
        if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
          size_t idx = row + (size_t)x;
          if (!depth_test || z < fb->depth[idx]) {
            size_t ci = idx * 4;
            fb->color_hdr[ci + 0] = cr;
            fb->color_hdr[ci + 1] = cg;
            fb->color_hdr[ci + 2] = cb;
            fb->color_hdr[ci + 3] = 1.0f;
            fb->color[ci + 0] = cr8;
            fb->color[ci + 1] = cg8;
            fb->color[ci + 2] = cb8;
            fb->color[ci + 3] = 255;
            fb->depth[idx] = z;
            fb->object_id[idx] = object_id;
          }
        }
        w0 += e0_dx;
        w1 += e1_dx;
        w2 += e2_dx;
        z += z_dx;
      }
      w0_row += e0_dy;
      w1_row += e1_dy;
      w2_row += e2_dy;
      z_row += z_dy;
    }
  } else {
    /* ── Blended path: ALPHA, ADDITIVE, MULTIPLY ── */
    float a_f = ca;
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
            float dr = fb->color_hdr[ci + 0];
            float dg = fb->color_hdr[ci + 1];
            float db = fb->color_hdr[ci + 2];
            float or_, og, ob;

            switch (blend_mode) {
            case MOP_BLEND_ADDITIVE:
              or_ = dr + cr * a_f;
              og = dg + cg * a_f;
              ob = db + cb * a_f;
              break;
            case MOP_BLEND_MULTIPLY:
              or_ = dr * cr;
              og = dg * cg;
              ob = db * cb;
              break;
            default: /* MOP_BLEND_ALPHA / MOP_BLEND_OPAQUE with alpha < 1 */
              or_ = cr * a_f + dr * inv_a;
              og = cg * a_f + dg * inv_a;
              ob = cb * a_f + db * inv_a;
              break;
            }

            fb->color_hdr[ci + 0] = or_;
            fb->color_hdr[ci + 1] = og;
            fb->color_hdr[ci + 2] = ob;
            fb->color_hdr[ci + 3] = 1.0f;
            float c0 = or_ < 0.0f ? 0.0f : (or_ > 1.0f ? 1.0f : or_);
            float c1 = og < 0.0f ? 0.0f : (og > 1.0f ? 1.0f : og);
            float c2 = ob < 0.0f ? 0.0f : (ob > 1.0f ? 1.0f : ob);
            fb->color[ci + 0] = (uint8_t)(c0 * 255.0f);
            fb->color[ci + 1] = (uint8_t)(c1 * 255.0f);
            fb->color[ci + 2] = (uint8_t)(c2 * 255.0f);
            fb->color[ci + 3] = 255;
          }
        }
        w0 += e0_dx;
        w1 += e1_dx;
        w2 += e2_dx;
        z += z_dx;
      }
      w0_row += e0_dy;
      w1_row += e1_dy;
      w2_row += e2_dy;
      z_row += z_dy;
    }
  }
}

/* -------------------------------------------------------------------------
 * Triangle rasterization entry point
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle(const MopSwClipVertex vertices[3],
                               uint32_t object_id, bool wireframe,
                               bool depth_test, bool cull_back,
                               MopVec3 light_dir, float ambient, float opacity,
                               bool smooth_shading, MopBlendMode blend_mode,
                               MopSwFramebuffer *fb) {
  MopVec4 a = vertices[0].position;
  MopVec4 b = vertices[1].position;
  MopVec4 c = vertices[2].position;

  /* Trivial frustum reject: all 3 vertices outside the same plane */
  if ((a.x < -a.w && b.x < -b.w && c.x < -c.w) ||
      (a.x > a.w && b.x > b.w && c.x > c.w) ||
      (a.y < -a.w && b.y < -b.w && c.y < -c.w) ||
      (a.y > a.w && b.y > b.w && c.y > c.w) ||
      (a.z < -a.w && b.z < -b.w && c.z < -c.w) ||
      (a.z > a.w && b.z > b.w && c.z > c.w))
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
    if (ex * fy - ey * fx <= 0.0f)
      return;
  }

  /* Trivial accept: all 3 vertices inside all 6 frustum planes.
   * When true, skip the expensive Sutherland-Hodgman clipping.
   * This is the common case for most visible triangles. */
  const MopSwClipVertex *poly;
  MopSwClipVertex clipped[MAX_CLIP_VERTICES];
  int poly_count;

  if (a.w > 0.0f && b.w > 0.0f && c.w > 0.0f && a.x >= -a.w && a.x <= a.w &&
      a.y >= -a.w && a.y <= a.w && a.z >= -a.w && a.z <= a.w && b.x >= -b.w &&
      b.x <= b.w && b.y >= -b.w && b.y <= b.w && b.z >= -b.w && b.z <= b.w &&
      c.x >= -c.w && c.x <= c.w && c.y >= -c.w && c.y <= c.w && c.z >= -c.w &&
      c.z <= c.w) {
    poly = vertices;
    poly_count = 3;
  } else {
    poly_count = mop_sw_clip_polygon(vertices, 3, clipped, MAX_CLIP_VERTICES);
    if (poly_count < 3)
      return;
    poly = clipped;
  }

  /* Hoist invariants out of the triangle fan loop */
  MopVec3 norm_light = mop_vec3_normalize(light_dir);
  float half_w = (float)fb->width * 0.5f;
  float half_h = (float)fb->height * 0.5f;
  uint8_t ca = (uint8_t)(clamp01(opacity) * 255.0f);

  /* Process polygon as a triangle fan */
  for (int i = 1; i < poly_count - 1; i++) {
    const MopSwClipVertex *v0 = &poly[0];
    const MopSwClipVertex *v1 = &poly[i];
    const MopSwClipVertex *v2 = &poly[i + 1];

    /* Perspective division */
    if (fabsf(v0->position.w) < 1e-7f || fabsf(v1->position.w) < 1e-7f ||
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
    float signed_area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
    if (cull_back && signed_area >= 0.0f) {
      continue;
    }

    /* Smooth shading path — dispatch to per-pixel normal interpolation */
    if (smooth_shading && !wireframe) {
      MopSwScreenVertex sv[3] = {
          {sx0, sy0, sz0, inv_w0, v0->normal, v0->world_pos, v0->color, v0->u,
           v0->v, v0->tangent},
          {sx1, sy1, sz1, inv_w1, v1->normal, v1->world_pos, v1->color, v1->u,
           v1->v, v1->tangent},
          {sx2, sy2, sz2, inv_w2, v2->normal, v2->world_pos, v2->color, v2->u,
           v2->v, v2->tangent},
      };
      mop_sw_rasterize_triangle_smooth(sv, object_id, depth_test, light_dir,
                                       ambient, opacity, blend_mode, fb);
      continue;
    }

    /* Flat shading */
    MopVec3 face_normal = mop_vec3_normalize(
        (MopVec3){(v0->normal.x + v1->normal.x + v2->normal.x),
                  (v0->normal.y + v1->normal.y + v2->normal.y),
                  (v0->normal.z + v1->normal.z + v2->normal.z)});

    float ndotl = mop_vec3_dot(face_normal, norm_light);
    if (ndotl < 0.0f)
      ndotl = 0.0f;
    float lighting = clamp01(ambient + (1.0f - ambient) * ndotl);

    float avg_r = (v0->color.r + v1->color.r + v2->color.r) * (1.0f / 3.0f);
    float avg_g = (v0->color.g + v1->color.g + v2->color.g) * (1.0f / 3.0f);
    float avg_b = (v0->color.b + v1->color.b + v2->color.b) * (1.0f / 3.0f);

    float crf = avg_r * lighting;
    float cgf = avg_g * lighting;
    float cbf = avg_b * lighting;
    if (crf < 0.0f)
      crf = 0.0f;
    if (cgf < 0.0f)
      cgf = 0.0f;
    if (cbf < 0.0f)
      cbf = 0.0f;

    if (wireframe) {
      uint8_t cr8 = (uint8_t)((crf > 1.0f ? 1.0f : crf) * 255.0f);
      uint8_t cg8 = (uint8_t)((cgf > 1.0f ? 1.0f : cgf) * 255.0f);
      uint8_t cb8 = (uint8_t)((cbf > 1.0f ? 1.0f : cbf) * 255.0f);
      mop_sw_draw_line_aa(fb, sx0, sy0, sz0, sx1, sy1, sz1, cr8, cg8, cb8,
                          object_id, depth_test, 1.0f);
      mop_sw_draw_line_aa(fb, sx1, sy1, sz1, sx2, sy2, sz2, cr8, cg8, cb8,
                          object_id, depth_test, 1.0f);
      mop_sw_draw_line_aa(fb, sx2, sy2, sz2, sx0, sy0, sz0, cr8, cg8, cb8,
                          object_id, depth_test, 1.0f);
    } else {
      float caf = (float)ca / 255.0f;
      rasterize_filled_triangle(fb, sx0, sy0, sz0, sx1, sy1, sz1, sx2, sy2, sz2,
                                crf, cgf, cbf, caf, object_id, depth_test,
                                blend_mode);
    }
  }
}

/* -------------------------------------------------------------------------
 * Smooth-shaded triangle rasterization (Phong)
 *
 * Per-pixel lighting with:
 *   - Perspective-correct interpolation of normals, colors, UVs
 *   - Edge anti-aliasing (coverage-based blend at triangle edges)
 *   - Blinn-Phong specular highlights
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle_smooth(const MopSwScreenVertex verts[3],
                                      uint32_t object_id, bool depth_test,
                                      MopVec3 light_dir, float ambient,
                                      float opacity, MopBlendMode blend_mode,
                                      MopSwFramebuffer *fb) {
  float sx0 = verts[0].sx, sy0 = verts[0].sy, sz0 = verts[0].sz;
  float sx1 = verts[1].sx, sy1 = verts[1].sy, sz1 = verts[1].sz;
  float sx2 = verts[2].sx, sy2 = verts[2].sy, sz2 = verts[2].sz;

  /* Bounding box */
  float fmin_x = sx0;
  if (sx1 < fmin_x)
    fmin_x = sx1;
  if (sx2 < fmin_x)
    fmin_x = sx2;
  float fmin_y = sy0;
  if (sy1 < fmin_y)
    fmin_y = sy1;
  if (sy2 < fmin_y)
    fmin_y = sy2;
  float fmax_x = sx0;
  if (sx1 > fmax_x)
    fmax_x = sx1;
  if (sx2 > fmax_x)
    fmax_x = sx2;
  float fmax_y = sy0;
  if (sy1 > fmax_y)
    fmax_y = sy1;
  if (sy2 > fmax_y)
    fmax_y = sy2;

  int min_x = (int)floorf(fmin_x);
  int min_y = (int)floorf(fmin_y);
  int max_x = (int)ceilf(fmax_x);
  int max_y = (int)ceilf(fmax_y);

  if (min_x < 0)
    min_x = 0;
  if (min_y < 0)
    min_y = 0;
  if (max_x >= fb->width)
    max_x = fb->width - 1;
  if (max_y >= fb->height)
    max_y = fb->height - 1;
  if (min_x > max_x || min_y > max_y)
    return;

  float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
  if (fabsf(area) < 1e-6f)
    return;

  bool flip = (area < 0.0f);
  float inv_area = 1.0f / fabsf(area);

  /* Edge function increments */
  float e0_dx = sy1 - sy2, e0_dy = sx2 - sx1;
  float e1_dx = sy2 - sy0, e1_dy = sx0 - sx2;
  float e2_dx = sy0 - sy1, e2_dy = sx1 - sx0;

  if (flip) {
    e0_dx = -e0_dx;
    e0_dy = -e0_dy;
    e1_dx = -e1_dx;
    e1_dy = -e1_dy;
    e2_dx = -e2_dx;
    e2_dy = -e2_dy;
  }

  /* Edge-distance scaling for AA */
  float e0_len = sqrtf(e0_dx * e0_dx + e0_dy * e0_dy);
  float e1_len = sqrtf(e1_dx * e1_dx + e1_dy * e1_dy);
  float e2_len = sqrtf(e2_dx * e2_dx + e2_dy * e2_dy);
  float inv_e0 = (e0_len > 1e-6f) ? 1.0f / e0_len : 0.0f;
  float inv_e1 = (e1_len > 1e-6f) ? 1.0f / e1_len : 0.0f;
  float inv_e2 = (e2_len > 1e-6f) ? 1.0f / e2_len : 0.0f;

  float px0 = (float)min_x + 0.5f;
  float py0 = (float)min_y + 0.5f;

  float w0_row = (sx2 - sx1) * (py0 - sy1) - (sy2 - sy1) * (px0 - sx1);
  float w1_row = (sx0 - sx2) * (py0 - sy2) - (sy0 - sy2) * (px0 - sx2);
  float w2_row = (sx1 - sx0) * (py0 - sy0) - (sy1 - sy0) * (px0 - sx0);

  if (flip) {
    w0_row = -w0_row;
    w1_row = -w1_row;
    w2_row = -w2_row;
  }

  MopVec3 nl = mop_vec3_normalize(light_dir);
  float op = clamp01(opacity);
  int width = fb->width;

  /* Per-vertex 1/w for perspective-correct interpolation */
  float iw0 = verts[0].inv_w, iw1 = verts[1].inv_w, iw2 = verts[2].inv_w;

  for (int y = min_y; y <= max_y; y++) {
    float w0 = w0_row, w1 = w1_row, w2 = w2_row;

    for (int x = min_x; x <= max_x; x++) {
      if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
        /* Compute edge distances for AA */
        float d0 = w0 * inv_e0;
        float d1 = w1 * inv_e1;
        float d2 = w2 * inv_e2;
        float min_d = d0;
        if (d1 < min_d)
          min_d = d1;
        if (d2 < min_d)
          min_d = d2;

        if (min_d >= 0.0f) {
          float b0 = w0 * inv_area;
          float b1 = w1 * inv_area;
          float b2 = w2 * inv_area;

          /* Depth (screen-space linear is correct for z/w) */
          float z = b0 * sz0 + b1 * sz1 + b2 * sz2;
          size_t idx = (size_t)y * (size_t)width + (size_t)x;

          if (!depth_test || z < fb->depth[idx]) {
            /* Perspective-correct barycentric weights */
            float pc0 = b0 * iw0;
            float pc1 = b1 * iw1;
            float pc2 = b2 * iw2;
            float inv_pc = 1.0f / (pc0 + pc1 + pc2);
            pc0 *= inv_pc;
            pc1 *= inv_pc;
            pc2 *= inv_pc;

            /* Interpolate normal (perspective-correct) */
            MopVec3 n = {pc0 * verts[0].normal.x + pc1 * verts[1].normal.x +
                             pc2 * verts[2].normal.x,
                         pc0 * verts[0].normal.y + pc1 * verts[1].normal.y +
                             pc2 * verts[2].normal.y,
                         pc0 * verts[0].normal.z + pc1 * verts[1].normal.z +
                             pc2 * verts[2].normal.z};
            n = mop_vec3_normalize(n);

            /* Interpolate color (perspective-correct) */
            float fr = pc0 * verts[0].color.r + pc1 * verts[1].color.r +
                       pc2 * verts[2].color.r;
            float fg = pc0 * verts[0].color.g + pc1 * verts[1].color.g +
                       pc2 * verts[2].color.g;
            float fb_ = pc0 * verts[0].color.b + pc1 * verts[1].color.b +
                        pc2 * verts[2].color.b;

            /* Diffuse lighting */
            float ndotl = mop_vec3_dot(n, nl);
            if (ndotl < 0.0f)
              ndotl = 0.0f;
            float lit = ambient + (1.0f - ambient) * ndotl;

            /* Blinn-Phong specular (view-independent approximation:
             * half-vector between light and up/view) */
            MopVec3 half_v =
                mop_vec3_normalize((MopVec3){nl.x, nl.y + 1.0f, nl.z});
            float ndoth = mop_vec3_dot(n, half_v);
            if (ndoth < 0.0f)
              ndoth = 0.0f;
            /* Specular power 32, intensity 0.25 */
            float spec = ndoth * ndoth;
            spec *= spec; /* ^4 */
            spec *= spec; /* ^8 */
            spec *= spec; /* ^16 */
            spec *= spec; /* ^32 */
            spec *= 0.25f;

            float pr = fr * lit + spec;
            float pg = fg * lit + spec;
            float pb = fb_ * lit + spec;
            if (pr < 0.0f)
              pr = 0.0f;
            if (pg < 0.0f)
              pg = 0.0f;
            if (pb < 0.0f)
              pb = 0.0f;

            /* Edge AA coverage — suppress at interior mesh edges to
             * avoid seam artifacts (only blend at silhouette edges). */
            float existing_z = fb->depth[idx];
            bool silhouette = (existing_z - z) > 0.005f;
            float cov = (min_d < 1.0f && silhouette) ? min_d : 1.0f;
            float final_alpha = op * cov;

            size_t ci = idx * 4;
            if (blend_mode == MOP_BLEND_OPAQUE && final_alpha >= 1.0f) {
              fb->color_hdr[ci + 0] = pr;
              fb->color_hdr[ci + 1] = pg;
              fb->color_hdr[ci + 2] = pb;
              fb->color_hdr[ci + 3] = 1.0f;
              fb->depth[idx] = z;
              fb->object_id[idx] = object_id;
            } else {
              float inv_fa = 1.0f - final_alpha;
              float dr = fb->color_hdr[ci + 0];
              float dg = fb->color_hdr[ci + 1];
              float db = fb->color_hdr[ci + 2];

              switch (blend_mode) {
              case MOP_BLEND_ADDITIVE:
                pr = dr + pr * final_alpha;
                pg = dg + pg * final_alpha;
                pb = db + pb * final_alpha;
                break;
              case MOP_BLEND_MULTIPLY:
                pr = dr * pr;
                pg = dg * pg;
                pb = db * pb;
                break;
              default: /* ALPHA / OPAQUE with edge AA */
                pr = pr * final_alpha + dr * inv_fa;
                pg = pg * final_alpha + dg * inv_fa;
                pb = pb * final_alpha + db * inv_fa;
                break;
              }
              fb->color_hdr[ci + 0] = pr;
              fb->color_hdr[ci + 1] = pg;
              fb->color_hdr[ci + 2] = pb;
              fb->color_hdr[ci + 3] = 1.0f;
              if (final_alpha > 0.5f) {
                fb->depth[idx] = z;
                fb->object_id[idx] = object_id;
              }
            }
          }
        }
      }
      w0 += e0_dx;
      w1 += e1_dx;
      w2 += e2_dx;
    }
    w0_row += e0_dy;
    w1_row += e1_dy;
    w2_row += e2_dy;
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
    if (!lights[i].active)
      continue;

    float ndotl = 0.0f;
    float attenuation = 1.0f;
    float spot_factor = 1.0f;

    switch (lights[i].type) {
    case MOP_LIGHT_DIRECTIONAL: {
      MopVec3 dir = mop_vec3_normalize(lights[i].direction);
      /* direction = where light shines; negate for surface-to-light */
      ndotl = -mop_vec3_dot(normal, dir);
      break;
    }
    case MOP_LIGHT_POINT: {
      MopVec3 to_light = mop_vec3_sub(lights[i].position, world_pos);
      float dist = mop_vec3_length(to_light);
      if (dist < 1e-6f)
        dist = 1e-6f;
      MopVec3 dir = mop_vec3_scale(to_light, 1.0f / dist);
      ndotl = mop_vec3_dot(normal, dir);

      /* Distance attenuation */
      if (lights[i].range > 0.0f) {
        float r = dist / lights[i].range;
        attenuation = 1.0f - r;
        if (attenuation < 0.0f)
          attenuation = 0.0f;
        attenuation *= attenuation;
      } else {
        /* Physical inverse-square falloff */
        float d2 = dist * dist;
        if (d2 < 0.01f)
          d2 = 0.01f;
        attenuation = 1.0f / d2;
      }
      break;
    }
    case MOP_LIGHT_SPOT: {
      MopVec3 to_light = mop_vec3_sub(lights[i].position, world_pos);
      float dist = mop_vec3_length(to_light);
      if (dist < 1e-6f)
        dist = 1e-6f;
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
          float t = (cos_angle - lights[i].spot_outer_cos) / range;
          spot_factor = t * t * (3.0f - 2.0f * t); /* smoothstep */
        }
      }

      /* Distance attenuation */
      if (lights[i].range > 0.0f) {
        float r = dist / lights[i].range;
        attenuation = 1.0f - r;
        if (attenuation < 0.0f)
          attenuation = 0.0f;
        attenuation *= attenuation;
      } else {
        attenuation = 1.0f / (1.0f + dist * dist);
      }
      break;
    }
    }

    if (ndotl < 0.0f)
      ndotl = 0.0f;

    /* Shadow test for directional lights */
    float shadow = 1.0f;
    if (lights[i].type == MOP_LIGHT_DIRECTIONAL && s_shadow_depth)
      shadow = shadow_test_pcf(world_pos);

    total += ndotl * lights[i].intensity * attenuation * spot_factor * shadow;
  }

  return clamp01(total);
}

/* -------------------------------------------------------------------------
 * Per-channel multi-light diffuse (applies light color per RGB channel).
 * Used by the smooth per-pixel shading path for accurate color tinting.
 * ------------------------------------------------------------------------- */

static void compute_multi_light_rgb(MopVec3 normal, MopVec3 world_pos,
                                    const MopLight *lights,
                                    uint32_t light_count, float ambient,
                                    float *out_r, float *out_g, float *out_b) {
  *out_r = ambient;
  *out_g = ambient;
  *out_b = ambient;

  for (uint32_t i = 0; i < light_count; i++) {
    if (!lights[i].active)
      continue;

    float ndotl = 0.0f;
    float attenuation = 1.0f;
    float spot_factor = 1.0f;

    switch (lights[i].type) {
    case MOP_LIGHT_DIRECTIONAL: {
      MopVec3 dir = mop_vec3_normalize(lights[i].direction);
      /* direction = where light shines; negate for surface-to-light */
      ndotl = -mop_vec3_dot(normal, dir);
      break;
    }
    case MOP_LIGHT_POINT: {
      MopVec3 to_light = mop_vec3_sub(lights[i].position, world_pos);
      float dist = mop_vec3_length(to_light);
      if (dist < 1e-6f)
        dist = 1e-6f;
      MopVec3 dir = mop_vec3_scale(to_light, 1.0f / dist);
      ndotl = mop_vec3_dot(normal, dir);
      if (lights[i].range > 0.0f) {
        float r = dist / lights[i].range;
        attenuation = 1.0f - r;
        if (attenuation < 0.0f)
          attenuation = 0.0f;
        attenuation *= attenuation;
      } else {
        float d2 = dist * dist;
        if (d2 < 0.01f)
          d2 = 0.01f;
        attenuation = 1.0f / d2;
      }
      break;
    }
    case MOP_LIGHT_SPOT: {
      MopVec3 to_light = mop_vec3_sub(lights[i].position, world_pos);
      float dist = mop_vec3_length(to_light);
      if (dist < 1e-6f)
        dist = 1e-6f;
      MopVec3 dir = mop_vec3_scale(to_light, 1.0f / dist);
      ndotl = mop_vec3_dot(normal, dir);
      MopVec3 spot_dir = mop_vec3_normalize(lights[i].direction);
      float cos_angle = -mop_vec3_dot(dir, spot_dir);
      if (cos_angle < lights[i].spot_outer_cos) {
        spot_factor = 0.0f;
      } else if (cos_angle < lights[i].spot_inner_cos) {
        float range_s = lights[i].spot_inner_cos - lights[i].spot_outer_cos;
        if (range_s > 1e-6f) {
          float t = (cos_angle - lights[i].spot_outer_cos) / range_s;
          spot_factor = t * t * (3.0f - 2.0f * t); /* smoothstep */
        }
      }
      if (lights[i].range > 0.0f) {
        float r = dist / lights[i].range;
        attenuation = 1.0f - r;
        if (attenuation < 0.0f)
          attenuation = 0.0f;
        attenuation *= attenuation;
      } else {
        float d2 = dist * dist;
        if (d2 < 0.01f)
          d2 = 0.01f;
        attenuation = 1.0f / d2;
      }
      break;
    }
    }

    if (ndotl < 0.0f)
      ndotl = 0.0f;

    /* Shadow test for directional lights */
    float shadow = 1.0f;
    if (lights[i].type == MOP_LIGHT_DIRECTIONAL && s_shadow_depth)
      shadow = shadow_test_pcf(world_pos);

    float contrib =
        ndotl * lights[i].intensity * attenuation * spot_factor * shadow;
    *out_r += contrib * lights[i].color.r;
    *out_g += contrib * lights[i].color.g;
    *out_b += contrib * lights[i].color.b;
  }
}

/* -------------------------------------------------------------------------
 * Multi-light GGX specular helper (Cook-Torrance microfacet model)
 *
 * Accumulates physically-based specular from all active lights using:
 *   - GGX normal distribution function (Trowbridge-Reitz)
 *   - Smith-Schlick geometry term
 *   - Schlick Fresnel approximation
 *
 * Returns per-channel specular via output pointers (includes Fresnel/F0).
 * ------------------------------------------------------------------------- */

static void compute_multi_specular_ggx(MopVec3 normal, MopVec3 world_pos,
                                       MopVec3 view_dir, const MopLight *lights,
                                       uint32_t light_count, float roughness,
                                       float metallic, float base_r,
                                       float base_g, float base_b,
                                       float *out_spec_r, float *out_spec_g,
                                       float *out_spec_b) {
  *out_spec_r = *out_spec_g = *out_spec_b = 0.0f;

  float alpha = roughness * roughness;
  float alpha2 = alpha * alpha;
  if (alpha2 < 1e-7f)
    alpha2 = 1e-7f;

  float ndotv = mop_vec3_dot(normal, view_dir);
  if (ndotv < 1e-4f)
    ndotv = 1e-4f;

  /* F0: dielectric=0.04, metallic=base_color */
  float f0_r = 0.04f * (1.0f - metallic) + base_r * metallic;
  float f0_g = 0.04f * (1.0f - metallic) + base_g * metallic;
  float f0_b = 0.04f * (1.0f - metallic) + base_b * metallic;

  /* Smith-Schlick G1 for view direction (constant across lights) */
  float k = alpha * 0.5f;
  float g1v = ndotv / (ndotv * (1.0f - k) + k);

  for (uint32_t i = 0; i < light_count; i++) {
    if (!lights[i].active)
      continue;

    MopVec3 l_dir = {0, 0, 0};
    float attenuation = 1.0f;

    switch (lights[i].type) {
    case MOP_LIGHT_DIRECTIONAL: {
      MopVec3 d = mop_vec3_normalize(lights[i].direction);
      l_dir = (MopVec3){-d.x, -d.y, -d.z}; /* negate: shines→to-light */
      break;
    }
    case MOP_LIGHT_POINT:
    case MOP_LIGHT_SPOT: {
      MopVec3 to_light = mop_vec3_sub(lights[i].position, world_pos);
      float dist = mop_vec3_length(to_light);
      if (dist < 1e-6f)
        dist = 1e-6f;
      l_dir = mop_vec3_scale(to_light, 1.0f / dist);
      if (lights[i].range > 0.0f) {
        float r = dist / lights[i].range;
        attenuation = 1.0f - r;
        if (attenuation < 0.0f)
          attenuation = 0.0f;
        attenuation *= attenuation;
      } else {
        float d2 = dist * dist;
        if (d2 < 0.01f)
          d2 = 0.01f;
        attenuation = 1.0f / d2;
      }
      break;
    }
    }

    float ndotl = mop_vec3_dot(normal, l_dir);
    if (ndotl <= 0.0f)
      continue;

    /* Half vector */
    MopVec3 h = mop_vec3_normalize(mop_vec3_add(l_dir, view_dir));
    float ndoth = mop_vec3_dot(normal, h);
    if (ndoth < 0.0f)
      ndoth = 0.0f;
    float vdoth = mop_vec3_dot(view_dir, h);
    if (vdoth < 0.0f)
      vdoth = 0.0f;

    /* GGX Normal Distribution (Trowbridge-Reitz) */
    float ndoth2 = ndoth * ndoth;
    float denom_d = ndoth2 * (alpha2 - 1.0f) + 1.0f;
    float D = alpha2 / ((float)M_PI * denom_d * denom_d);

    /* Smith-Schlick Geometry: G = G1(v) * G1(l) */
    float g1l = ndotl / (ndotl * (1.0f - k) + k);
    float G = g1v * g1l;

    /* Schlick Fresnel: F = F0 + (1 - F0)(1 - VdotH)^5 */
    float omv = 1.0f - vdoth;
    float omv2 = omv * omv;
    float omv5 = omv2 * omv2 * omv;
    float fr_r = f0_r + (1.0f - f0_r) * omv5;
    float fr_g = f0_g + (1.0f - f0_g) * omv5;
    float fr_b = f0_b + (1.0f - f0_b) * omv5;

    /* Cook-Torrance: D * G * F / (4 * NdotL * NdotV)
       Multiply by π to match energy scale: MOP's diffuse omits the 1/π
       Lambertian factor, so the scene intensity is π× physical irradiance.
       Diffuse absorbs this, but specular (no 1/π) needs explicit π. */
    float spec_denom = 4.0f * ndotl * ndotv;
    if (spec_denom < 1e-6f)
      spec_denom = 1e-6f;
    float spec_term = D * G / spec_denom;
    /* Shadow test for directional lights */
    float shadow = 1.0f;
    if (lights[i].type == MOP_LIGHT_DIRECTIONAL && s_shadow_depth)
      shadow = shadow_test_pcf(world_pos);

    float weight = spec_term * lights[i].intensity * attenuation * ndotl *
                   shadow * (float)M_PI;

    *out_spec_r += weight * fr_r * lights[i].color.r;
    *out_spec_g += weight * fr_g * lights[i].color.g;
    *out_spec_b += weight * fr_b * lights[i].color.b;
  }
}

/* -------------------------------------------------------------------------
 * Smooth-shaded triangle rasterization with multi-light support
 *
 * Per-pixel lighting with:
 *   - Perspective-correct interpolation
 *   - Edge anti-aliasing
 *   - Multi-light diffuse + GGX Cook-Torrance specular
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle_smooth_ml(
    const MopSwScreenVertex verts[3], uint32_t object_id, bool depth_test,
    MopVec3 light_dir, float ambient, float opacity, MopBlendMode blend_mode,
    const MopLight *lights, uint32_t light_count, MopVec3 cam_eye,
    float metallic, float roughness, MopSwFramebuffer *fb) {
  /* If no multi-light, fall back to standard smooth */
  if (!lights || light_count == 0) {
    mop_sw_rasterize_triangle_smooth(verts, object_id, depth_test, light_dir,
                                     ambient, opacity, blend_mode, fb);
    return;
  }

  float sx0 = verts[0].sx, sy0 = verts[0].sy, sz0 = verts[0].sz;
  float sx1 = verts[1].sx, sy1 = verts[1].sy, sz1 = verts[1].sz;
  float sx2 = verts[2].sx, sy2 = verts[2].sy, sz2 = verts[2].sz;

  /* Bounding box */
  float fmin_x = sx0;
  if (sx1 < fmin_x)
    fmin_x = sx1;
  if (sx2 < fmin_x)
    fmin_x = sx2;
  float fmin_y = sy0;
  if (sy1 < fmin_y)
    fmin_y = sy1;
  if (sy2 < fmin_y)
    fmin_y = sy2;
  float fmax_x = sx0;
  if (sx1 > fmax_x)
    fmax_x = sx1;
  if (sx2 > fmax_x)
    fmax_x = sx2;
  float fmax_y = sy0;
  if (sy1 > fmax_y)
    fmax_y = sy1;
  if (sy2 > fmax_y)
    fmax_y = sy2;

  int min_x = (int)floorf(fmin_x);
  int min_y = (int)floorf(fmin_y);
  int max_x = (int)ceilf(fmax_x);
  int max_y = (int)ceilf(fmax_y);

  if (min_x < 0)
    min_x = 0;
  if (min_y < 0)
    min_y = 0;
  if (max_x >= fb->width)
    max_x = fb->width - 1;
  if (max_y >= fb->height)
    max_y = fb->height - 1;
  if (min_x > max_x || min_y > max_y)
    return;

  float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
  if (fabsf(area) < 1e-6f)
    return;

  bool flip = (area < 0.0f);
  float inv_area = 1.0f / fabsf(area);

  float e0_dx = sy1 - sy2, e0_dy = sx2 - sx1;
  float e1_dx = sy2 - sy0, e1_dy = sx0 - sx2;
  float e2_dx = sy0 - sy1, e2_dy = sx1 - sx0;

  if (flip) {
    e0_dx = -e0_dx;
    e0_dy = -e0_dy;
    e1_dx = -e1_dx;
    e1_dy = -e1_dy;
    e2_dx = -e2_dx;
    e2_dy = -e2_dy;
  }

  /* Edge-distance scaling for AA */
  float e0_len = sqrtf(e0_dx * e0_dx + e0_dy * e0_dy);
  float e1_len = sqrtf(e1_dx * e1_dx + e1_dy * e1_dy);
  float e2_len = sqrtf(e2_dx * e2_dx + e2_dy * e2_dy);
  float inv_e0 = (e0_len > 1e-6f) ? 1.0f / e0_len : 0.0f;
  float inv_e1 = (e1_len > 1e-6f) ? 1.0f / e1_len : 0.0f;
  float inv_e2 = (e2_len > 1e-6f) ? 1.0f / e2_len : 0.0f;

  float px0 = (float)min_x + 0.5f;
  float py0 = (float)min_y + 0.5f;

  float w0_row = (sx2 - sx1) * (py0 - sy1) - (sy2 - sy1) * (px0 - sx1);
  float w1_row = (sx0 - sx2) * (py0 - sy2) - (sy0 - sy2) * (px0 - sx2);
  float w2_row = (sx1 - sx0) * (py0 - sy0) - (sy1 - sy0) * (px0 - sx0);

  if (flip) {
    w0_row = -w0_row;
    w1_row = -w1_row;
    w2_row = -w2_row;
  }

  float op = clamp01(opacity);
  int width = fb->width;

  /* Per-vertex 1/w for perspective-correct interpolation */
  float iw0 = verts[0].inv_w, iw1 = verts[1].inv_w, iw2 = verts[2].inv_w;

  for (int y = min_y; y <= max_y; y++) {
    float w0 = w0_row, w1 = w1_row, w2 = w2_row;

    for (int x = min_x; x <= max_x; x++) {
      if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
        float d0 = w0 * inv_e0;
        float d1 = w1 * inv_e1;
        float d2 = w2 * inv_e2;
        float min_d = d0;
        if (d1 < min_d)
          min_d = d1;
        if (d2 < min_d)
          min_d = d2;

        if (min_d >= 0.0f) {
          float b0 = w0 * inv_area;
          float b1 = w1 * inv_area;
          float b2 = w2 * inv_area;

          float z = b0 * sz0 + b1 * sz1 + b2 * sz2;
          size_t idx = (size_t)y * (size_t)width + (size_t)x;

          if (!depth_test || z < fb->depth[idx]) {
            /* Perspective-correct weights */
            float pc0 = b0 * iw0;
            float pc1 = b1 * iw1;
            float pc2 = b2 * iw2;
            float inv_pc = 1.0f / (pc0 + pc1 + pc2);
            pc0 *= inv_pc;
            pc1 *= inv_pc;
            pc2 *= inv_pc;

            /* Interpolate normal */
            MopVec3 n = {pc0 * verts[0].normal.x + pc1 * verts[1].normal.x +
                             pc2 * verts[2].normal.x,
                         pc0 * verts[0].normal.y + pc1 * verts[1].normal.y +
                             pc2 * verts[2].normal.y,
                         pc0 * verts[0].normal.z + pc1 * verts[1].normal.z +
                             pc2 * verts[2].normal.z};
            n = mop_vec3_normalize(n);

            /* Interpolate color */
            float fr = pc0 * verts[0].color.r + pc1 * verts[1].color.r +
                       pc2 * verts[2].color.r;
            float fg = pc0 * verts[0].color.g + pc1 * verts[1].color.g +
                       pc2 * verts[2].color.g;
            float fb_ = pc0 * verts[0].color.b + pc1 * verts[1].color.b +
                        pc2 * verts[2].color.b;

            /* Interpolate world position for point/spot light attenuation */
            MopVec3 world_pos = {
                pc0 * verts[0].world_pos.x + pc1 * verts[1].world_pos.x +
                    pc2 * verts[2].world_pos.x,
                pc0 * verts[0].world_pos.y + pc1 * verts[1].world_pos.y +
                    pc2 * verts[2].world_pos.y,
                pc0 * verts[0].world_pos.z + pc1 * verts[1].world_pos.z +
                    pc2 * verts[2].world_pos.z};

            /* Per-pixel view direction for specular */
            MopVec3 view_dir =
                mop_vec3_normalize(mop_vec3_sub(cam_eye, world_pos));

            /* Per-channel diffuse (applies light color per RGB) */
            float lit_r, lit_g, lit_b;
            compute_multi_light_rgb(n, world_pos, lights, light_count, ambient,
                                    &lit_r, &lit_g, &lit_b);

            /* GGX specular (includes light color and π energy correction) */
            float spec_r, spec_g, spec_b;
            compute_multi_specular_ggx(n, world_pos, view_dir, lights,
                                       light_count, roughness, metallic, fr, fg,
                                       fb_, &spec_r, &spec_g, &spec_b);

            /* PBR energy balance */
            float diffuse_scale = 1.0f - metallic;

            /* IBL (Image-Based Lighting) — uses precomputed maps when
             * available, falls back to hemisphere gradient otherwise. */
            float env_r = 0.0f, env_g = 0.0f, env_b = 0.0f;
            float ibl_diff_r = 0.0f, ibl_diff_g = 0.0f, ibl_diff_b = 0.0f;

            if (s_ibl.irradiance) {
              /* IBL diffuse: irradiance map at surface normal */
              float irr[3];
              ibl_irradiance(n, irr);
              ibl_diff_r = irr[0];
              ibl_diff_g = irr[1];
              ibl_diff_b = irr[2];
            }

            {
              /* Reflect view direction around normal */
              float vdn = mop_vec3_dot(view_dir, n);
              MopVec3 refl = {2.0f * vdn * n.x - view_dir.x,
                              2.0f * vdn * n.y - view_dir.y,
                              2.0f * vdn * n.z - view_dir.z};

              float ndv = mop_vec3_dot(n, view_dir);
              if (ndv < 0.0f)
                ndv = 0.0f;

              /* F0: Fresnel at normal incidence */
              float f0_r = 0.04f * (1.0f - metallic) + fr * metallic;
              float f0_g = 0.04f * (1.0f - metallic) + fg * metallic;
              float f0_b = 0.04f * (1.0f - metallic) + fb_ * metallic;

              if (s_ibl.prefiltered && s_ibl.brdf_lut) {
                /* IBL specular: split-sum approximation */
                float pf[3];
                ibl_prefiltered(refl, roughness, pf);
                float brdf_s, brdf_b;
                ibl_brdf(ndv, roughness, &brdf_s, &brdf_b);

                env_r = pf[0] * (f0_r * brdf_s + brdf_b);
                env_g = pf[1] * (f0_g * brdf_s + brdf_b);
                env_b = pf[2] * (f0_b * brdf_s + brdf_b);
              } else if (metallic > 0.01f) {
                /* Fallback hemisphere gradient (no env map loaded) */
                float sky_t = refl.y * 0.5f + 0.5f;
                if (sky_t < 0.0f)
                  sky_t = 0.0f;
                if (sky_t > 1.0f)
                  sky_t = 1.0f;

                float sky_rv = 0.35f, sky_gv = 0.38f, sky_bv = 0.45f;
                float gnd_rv = 0.08f, gnd_gv = 0.07f, gnd_bv = 0.06f;
                float he_r = gnd_rv + sky_t * (sky_rv - gnd_rv);
                float he_g = gnd_gv + sky_t * (sky_gv - gnd_gv);
                float he_b = gnd_bv + sky_t * (sky_bv - gnd_bv);

                float rough_mix = roughness * roughness;
                float avg_env = (he_r + he_g + he_b) * (1.0f / 3.0f);
                he_r += rough_mix * (avg_env - he_r);
                he_g += rough_mix * (avg_env - he_g);
                he_b += rough_mix * (avg_env - he_b);

                float om = 1.0f - ndv;
                float om2 = om * om;
                float om5 = om2 * om2 * om;
                float fe_r = f0_r + (1.0f - f0_r) * om5;
                float fe_g = f0_g + (1.0f - f0_g) * om5;
                float fe_b = f0_b + (1.0f - f0_b) * om5;

                env_r = he_r * fe_r * ambient * 3.0f;
                env_g = he_g * fe_g * ambient * 3.0f;
                env_b = he_b * fe_b * ambient * 3.0f;
              }
            }

            /* Final composition: IBL diffuse replaces flat ambient when
             * irradiance map is available */
            float pr, pg, pb;
            if (s_ibl.irradiance) {
              pr = fr * (lit_r + ibl_diff_r) * diffuse_scale + env_r + spec_r;
              pg = fg * (lit_g + ibl_diff_g) * diffuse_scale + env_g + spec_g;
              pb = fb_ * (lit_b + ibl_diff_b) * diffuse_scale + env_b + spec_b;
            } else {
              pr = fr * lit_r * diffuse_scale + env_r + spec_r;
              pg = fg * lit_g * diffuse_scale + env_g + spec_g;
              pb = fb_ * lit_b * diffuse_scale + env_b + spec_b;
            }
            if (pr < 0.0f)
              pr = 0.0f;
            if (pg < 0.0f)
              pg = 0.0f;
            if (pb < 0.0f)
              pb = 0.0f;

            /* Edge AA coverage — disabled for opaque geometry to avoid
             * seam artifacts at shared mesh edges.  FXAA post-process
             * handles silhouette anti-aliasing instead. */
            float cov =
                (min_d < 1.0f && blend_mode != MOP_BLEND_OPAQUE) ? min_d : 1.0f;
            float final_alpha = op * cov;

            size_t ci = idx * 4;
            if (blend_mode == MOP_BLEND_OPAQUE && final_alpha >= 1.0f) {
              fb->color_hdr[ci + 0] = pr;
              fb->color_hdr[ci + 1] = pg;
              fb->color_hdr[ci + 2] = pb;
              fb->color_hdr[ci + 3] = 1.0f;
              fb->depth[idx] = z;
              fb->object_id[idx] = object_id;
            } else {
              float inv_fa = 1.0f - final_alpha;
              float dr = fb->color_hdr[ci + 0];
              float dg = fb->color_hdr[ci + 1];
              float db = fb->color_hdr[ci + 2];

              switch (blend_mode) {
              case MOP_BLEND_ADDITIVE:
                pr = dr + pr * final_alpha;
                pg = dg + pg * final_alpha;
                pb = db + pb * final_alpha;
                break;
              case MOP_BLEND_MULTIPLY:
                pr = dr * pr;
                pg = dg * pg;
                pb = db * pb;
                break;
              default:
                pr = pr * final_alpha + dr * inv_fa;
                pg = pg * final_alpha + dg * inv_fa;
                pb = pb * final_alpha + db * inv_fa;
                break;
              }
              fb->color_hdr[ci + 0] = pr;
              fb->color_hdr[ci + 1] = pg;
              fb->color_hdr[ci + 2] = pb;
              fb->color_hdr[ci + 3] = 1.0f;
              if (final_alpha > 0.5f) {
                fb->depth[idx] = z;
                fb->object_id[idx] = object_id;
              }
            }
          }
        }
      }
      w0 += e0_dx;
      w1 += e1_dx;
      w2 += e2_dx;
    }
    w0_row += e0_dy;
    w1_row += e1_dy;
    w2_row += e2_dy;
  }
}

/* -------------------------------------------------------------------------
 * Smooth-shaded triangle rasterization with normal mapping
 *
 * Per-pixel lighting with:
 *   - Perspective-correct interpolation
 *   - Edge anti-aliasing
 *   - TBN normal mapping
 *   - Blinn-Phong specular
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle_smooth_nm(const MopSwScreenVertex verts[3],
                                         uint32_t object_id, bool depth_test,
                                         MopVec3 light_dir, float ambient,
                                         float opacity, MopBlendMode blend_mode,
                                         const MopSwNormalMap *normal_map,
                                         MopSwFramebuffer *fb) {
  /* If no normal map, fall back to standard smooth shading */
  if (!normal_map || !normal_map->data) {
    mop_sw_rasterize_triangle_smooth(verts, object_id, depth_test, light_dir,
                                     ambient, opacity, blend_mode, fb);
    return;
  }

  float sx0 = verts[0].sx, sy0 = verts[0].sy, sz0 = verts[0].sz;
  float sx1 = verts[1].sx, sy1 = verts[1].sy, sz1 = verts[1].sz;
  float sx2 = verts[2].sx, sy2 = verts[2].sy, sz2 = verts[2].sz;

  /* Bounding box */
  float fmin_x = sx0;
  if (sx1 < fmin_x)
    fmin_x = sx1;
  if (sx2 < fmin_x)
    fmin_x = sx2;
  float fmin_y = sy0;
  if (sy1 < fmin_y)
    fmin_y = sy1;
  if (sy2 < fmin_y)
    fmin_y = sy2;
  float fmax_x = sx0;
  if (sx1 > fmax_x)
    fmax_x = sx1;
  if (sx2 > fmax_x)
    fmax_x = sx2;
  float fmax_y = sy0;
  if (sy1 > fmax_y)
    fmax_y = sy1;
  if (sy2 > fmax_y)
    fmax_y = sy2;

  int min_x = (int)floorf(fmin_x);
  int min_y = (int)floorf(fmin_y);
  int max_x = (int)ceilf(fmax_x);
  int max_y = (int)ceilf(fmax_y);

  if (min_x < 0)
    min_x = 0;
  if (min_y < 0)
    min_y = 0;
  if (max_x >= fb->width)
    max_x = fb->width - 1;
  if (max_y >= fb->height)
    max_y = fb->height - 1;
  if (min_x > max_x || min_y > max_y)
    return;

  float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
  if (fabsf(area) < 1e-6f)
    return;

  bool flip = (area < 0.0f);
  float inv_area = 1.0f / fabsf(area);

  float e0_dx = sy1 - sy2, e0_dy = sx2 - sx1;
  float e1_dx = sy2 - sy0, e1_dy = sx0 - sx2;
  float e2_dx = sy0 - sy1, e2_dy = sx1 - sx0;

  if (flip) {
    e0_dx = -e0_dx;
    e0_dy = -e0_dy;
    e1_dx = -e1_dx;
    e1_dy = -e1_dy;
    e2_dx = -e2_dx;
    e2_dy = -e2_dy;
  }

  /* Edge-distance scaling for AA */
  float e0_len = sqrtf(e0_dx * e0_dx + e0_dy * e0_dy);
  float e1_len = sqrtf(e1_dx * e1_dx + e1_dy * e1_dy);
  float e2_len = sqrtf(e2_dx * e2_dx + e2_dy * e2_dy);
  float inv_e0 = (e0_len > 1e-6f) ? 1.0f / e0_len : 0.0f;
  float inv_e1 = (e1_len > 1e-6f) ? 1.0f / e1_len : 0.0f;
  float inv_e2 = (e2_len > 1e-6f) ? 1.0f / e2_len : 0.0f;

  float px0 = (float)min_x + 0.5f;
  float py0 = (float)min_y + 0.5f;

  float w0_row = (sx2 - sx1) * (py0 - sy1) - (sy2 - sy1) * (px0 - sx1);
  float w1_row = (sx0 - sx2) * (py0 - sy2) - (sy0 - sy2) * (px0 - sx2);
  float w2_row = (sx1 - sx0) * (py0 - sy0) - (sy1 - sy0) * (px0 - sx0);

  if (flip) {
    w0_row = -w0_row;
    w1_row = -w1_row;
    w2_row = -w2_row;
  }

  MopVec3 nl = mop_vec3_normalize(light_dir);
  float op = clamp01(opacity);
  int width = fb->width;
  int nm_w = normal_map->width;
  int nm_h = normal_map->height;

  /* Per-vertex 1/w for perspective-correct interpolation */
  float iw0 = verts[0].inv_w, iw1 = verts[1].inv_w, iw2 = verts[2].inv_w;

  for (int y = min_y; y <= max_y; y++) {
    float w0 = w0_row, w1 = w1_row, w2 = w2_row;

    for (int x = min_x; x <= max_x; x++) {
      if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
        float d0 = w0 * inv_e0;
        float d1 = w1 * inv_e1;
        float d2 = w2 * inv_e2;
        float min_d = d0;
        if (d1 < min_d)
          min_d = d1;
        if (d2 < min_d)
          min_d = d2;

        if (min_d >= 0.0f) {
          float b0 = w0 * inv_area;
          float b1 = w1 * inv_area;
          float b2 = w2 * inv_area;

          float z = b0 * sz0 + b1 * sz1 + b2 * sz2;
          size_t idx = (size_t)y * (size_t)width + (size_t)x;

          if (!depth_test || z < fb->depth[idx]) {
            /* Perspective-correct weights */
            float pc0 = b0 * iw0;
            float pc1 = b1 * iw1;
            float pc2 = b2 * iw2;
            float inv_pc = 1.0f / (pc0 + pc1 + pc2);
            pc0 *= inv_pc;
            pc1 *= inv_pc;
            pc2 *= inv_pc;

            /* Interpolate normal (perspective-correct) */
            MopVec3 n = {pc0 * verts[0].normal.x + pc1 * verts[1].normal.x +
                             pc2 * verts[2].normal.x,
                         pc0 * verts[0].normal.y + pc1 * verts[1].normal.y +
                             pc2 * verts[2].normal.y,
                         pc0 * verts[0].normal.z + pc1 * verts[1].normal.z +
                             pc2 * verts[2].normal.z};
            n = mop_vec3_normalize(n);

            /* Interpolate tangent (perspective-correct) */
            MopVec3 t_vec = {
                pc0 * verts[0].tangent.x + pc1 * verts[1].tangent.x +
                    pc2 * verts[2].tangent.x,
                pc0 * verts[0].tangent.y + pc1 * verts[1].tangent.y +
                    pc2 * verts[2].tangent.y,
                pc0 * verts[0].tangent.z + pc1 * verts[1].tangent.z +
                    pc2 * verts[2].tangent.z};
            t_vec = mop_vec3_normalize(t_vec);

            /* Bitangent = cross(normal, tangent) */
            MopVec3 bitan = mop_vec3_cross(n, t_vec);
            bitan = mop_vec3_normalize(bitan);

            /* Sample normal map at perspective-correct UV */
            float uv_u = pc0 * verts[0].u + pc1 * verts[1].u + pc2 * verts[2].u;
            float uv_v = pc0 * verts[0].v + pc1 * verts[1].v + pc2 * verts[2].v;
            uv_u = uv_u - floorf(uv_u);
            uv_v = uv_v - floorf(uv_v);

            int nm_x = (int)(uv_u * (float)(nm_w - 1) + 0.5f);
            int nm_y = (int)(uv_v * (float)(nm_h - 1) + 0.5f);
            if (nm_x < 0)
              nm_x = 0;
            if (nm_x >= nm_w)
              nm_x = nm_w - 1;
            if (nm_y < 0)
              nm_y = 0;
            if (nm_y >= nm_h)
              nm_y = nm_h - 1;

            size_t nm_idx = ((size_t)nm_y * (size_t)nm_w + (size_t)nm_x) * 4;
            float nm_nx = (float)normal_map->data[nm_idx + 0] / 127.5f - 1.0f;
            float nm_ny = (float)normal_map->data[nm_idx + 1] / 127.5f - 1.0f;
            float nm_nz = (float)normal_map->data[nm_idx + 2] / 127.5f - 1.0f;

            /* TBN transform */
            MopVec3 perturbed = {
                t_vec.x * nm_nx + bitan.x * nm_ny + n.x * nm_nz,
                t_vec.y * nm_nx + bitan.y * nm_ny + n.y * nm_nz,
                t_vec.z * nm_nx + bitan.z * nm_ny + n.z * nm_nz};
            perturbed = mop_vec3_normalize(perturbed);

            /* Interpolate color (perspective-correct) */
            float fr = pc0 * verts[0].color.r + pc1 * verts[1].color.r +
                       pc2 * verts[2].color.r;
            float fg = pc0 * verts[0].color.g + pc1 * verts[1].color.g +
                       pc2 * verts[2].color.g;
            float fb_ = pc0 * verts[0].color.b + pc1 * verts[1].color.b +
                        pc2 * verts[2].color.b;

            /* Diffuse lighting with perturbed normal */
            float ndotl = mop_vec3_dot(perturbed, nl);
            if (ndotl < 0.0f)
              ndotl = 0.0f;
            float lit = ambient + (1.0f - ambient) * ndotl;

            /* Specular */
            MopVec3 half_v =
                mop_vec3_normalize((MopVec3){nl.x, nl.y + 1.0f, nl.z});
            float ndoth = mop_vec3_dot(perturbed, half_v);
            if (ndoth < 0.0f)
              ndoth = 0.0f;
            float spec = ndoth * ndoth;
            spec *= spec;
            spec *= spec;
            spec *= spec;
            spec *= spec;
            spec *= 0.25f;

            float pr = fr * lit + spec;
            float pg = fg * lit + spec;
            float pb = fb_ * lit + spec;
            if (pr < 0.0f)
              pr = 0.0f;
            if (pg < 0.0f)
              pg = 0.0f;
            if (pb < 0.0f)
              pb = 0.0f;

            /* Edge AA coverage — disabled for opaque geometry to avoid
             * seam artifacts at shared mesh edges.  FXAA post-process
             * handles silhouette anti-aliasing instead. */
            float cov =
                (min_d < 1.0f && blend_mode != MOP_BLEND_OPAQUE) ? min_d : 1.0f;
            float final_alpha = op * cov;

            size_t ci = idx * 4;
            if (blend_mode == MOP_BLEND_OPAQUE && final_alpha >= 1.0f) {
              fb->color_hdr[ci + 0] = pr;
              fb->color_hdr[ci + 1] = pg;
              fb->color_hdr[ci + 2] = pb;
              fb->color_hdr[ci + 3] = 1.0f;
              fb->depth[idx] = z;
              fb->object_id[idx] = object_id;
            } else {
              float inv_fa = 1.0f - final_alpha;
              float dr = fb->color_hdr[ci + 0];
              float dg = fb->color_hdr[ci + 1];
              float db = fb->color_hdr[ci + 2];

              switch (blend_mode) {
              case MOP_BLEND_ADDITIVE:
                pr = dr + pr * final_alpha;
                pg = dg + pg * final_alpha;
                pb = db + pb * final_alpha;
                break;
              case MOP_BLEND_MULTIPLY:
                pr = dr * pr;
                pg = dg * pg;
                pb = db * pb;
                break;
              default:
                pr = pr * final_alpha + dr * inv_fa;
                pg = pg * final_alpha + dg * inv_fa;
                pb = pb * final_alpha + db * inv_fa;
                break;
              }
              fb->color_hdr[ci + 0] = pr;
              fb->color_hdr[ci + 1] = pg;
              fb->color_hdr[ci + 2] = pb;
              fb->color_hdr[ci + 3] = 1.0f;
              if (final_alpha > 0.5f) {
                fb->depth[idx] = z;
                fb->object_id[idx] = object_id;
              }
            }
          }
        }
      }
      w0 += e0_dx;
      w1 += e1_dx;
      w2 += e2_dx;
    }
    w0_row += e0_dy;
    w1_row += e1_dy;
    w2_row += e2_dy;
  }
}

/* -------------------------------------------------------------------------
 * Full triangle rasterization with multi-light support
 *
 * Same structure as mop_sw_rasterize_triangle but dispatches to the
 * multi-light smooth shading path when lights are available, and uses
 * compute_multi_light() for flat shading with multiple lights.
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_triangle_full(
    const MopSwClipVertex vertices[3], uint32_t object_id, bool wireframe,
    bool depth_test, bool cull_back, MopVec3 light_dir, float ambient,
    float opacity, bool smooth_shading, MopBlendMode blend_mode,
    const MopLight *lights, uint32_t light_count, MopVec3 cam_eye,
    float metallic, float roughness, MopSwFramebuffer *fb) {
  MopVec4 a = vertices[0].position;
  MopVec4 b = vertices[1].position;
  MopVec4 c = vertices[2].position;

  /* Trivial frustum reject */
  if ((a.x < -a.w && b.x < -b.w && c.x < -c.w) ||
      (a.x > a.w && b.x > b.w && c.x > c.w) ||
      (a.y < -a.w && b.y < -b.w && c.y < -c.w) ||
      (a.y > a.w && b.y > b.w && c.y > c.w) ||
      (a.z < -a.w && b.z < -b.w && c.z < -c.w) ||
      (a.z > a.w && b.z > b.w && c.z > c.w))
    return;

  /* Early backface cull in clip space */
  if (cull_back && a.w > 0.0f && b.w > 0.0f && c.w > 0.0f) {
    float ex = b.x * a.w - a.x * b.w;
    float ey = b.y * a.w - a.y * b.w;
    float fx = c.x * a.w - a.x * c.w;
    float fy = c.y * a.w - a.y * c.w;
    if (ex * fy - ey * fx <= 0.0f)
      return;
  }

  /* Clip */
  const MopSwClipVertex *poly;
  MopSwClipVertex clipped[MAX_CLIP_VERTICES];
  int poly_count;

  if (a.w > 0.0f && b.w > 0.0f && c.w > 0.0f && a.x >= -a.w && a.x <= a.w &&
      a.y >= -a.w && a.y <= a.w && a.z >= -a.w && a.z <= a.w && b.x >= -b.w &&
      b.x <= b.w && b.y >= -b.w && b.y <= b.w && b.z >= -b.w && b.z <= b.w &&
      c.x >= -c.w && c.x <= c.w && c.y >= -c.w && c.y <= c.w && c.z >= -c.w &&
      c.z <= c.w) {
    poly = vertices;
    poly_count = 3;
  } else {
    poly_count = mop_sw_clip_polygon(vertices, 3, clipped, MAX_CLIP_VERTICES);
    if (poly_count < 3)
      return;
    poly = clipped;
  }

  MopVec3 norm_light = mop_vec3_normalize(light_dir);
  float half_w = (float)fb->width * 0.5f;
  float half_h = (float)fb->height * 0.5f;

  for (int i = 1; i < poly_count - 1; i++) {
    const MopSwClipVertex *v0 = &poly[0];
    const MopSwClipVertex *v1 = &poly[i];
    const MopSwClipVertex *v2 = &poly[i + 1];

    if (fabsf(v0->position.w) < 1e-7f || fabsf(v1->position.w) < 1e-7f ||
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

    float signed_area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
    if (cull_back && signed_area >= 0.0f) {
      continue;
    }

    /* Smooth shading with multi-light */
    if (smooth_shading && !wireframe) {
      MopSwScreenVertex sv[3] = {
          {sx0, sy0, sz0, inv_w0, v0->normal, v0->world_pos, v0->color, v0->u,
           v0->v, v0->tangent},
          {sx1, sy1, sz1, inv_w1, v1->normal, v1->world_pos, v1->color, v1->u,
           v1->v, v1->tangent},
          {sx2, sy2, sz2, inv_w2, v2->normal, v2->world_pos, v2->color, v2->u,
           v2->v, v2->tangent},
      };
      mop_sw_rasterize_triangle_smooth_ml(
          sv, object_id, depth_test, light_dir, ambient, opacity, blend_mode,
          lights, light_count, cam_eye, metallic, roughness, fb);
      continue;
    }

    /* Flat shading with multi-light */
    MopVec3 face_normal = mop_vec3_normalize(
        (MopVec3){(v0->normal.x + v1->normal.x + v2->normal.x),
                  (v0->normal.y + v1->normal.y + v2->normal.y),
                  (v0->normal.z + v1->normal.z + v2->normal.z)});

    float lighting;
    if (lights && light_count > 0) {
      /* Use triangle centroid for flat-shaded world position */
      MopVec3 world_pos = {
          (v0->world_pos.x + v1->world_pos.x + v2->world_pos.x) / 3.0f,
          (v0->world_pos.y + v1->world_pos.y + v2->world_pos.y) / 3.0f,
          (v0->world_pos.z + v1->world_pos.z + v2->world_pos.z) / 3.0f};
      lighting = compute_multi_light(face_normal, world_pos, lights,
                                     light_count, ambient);
    } else {
      float ndotl = mop_vec3_dot(face_normal, norm_light);
      if (ndotl < 0.0f)
        ndotl = 0.0f;
      lighting = clamp01(ambient + (1.0f - ambient) * ndotl);
    }

    float avg_r = (v0->color.r + v1->color.r + v2->color.r) * (1.0f / 3.0f);
    float avg_g = (v0->color.g + v1->color.g + v2->color.g) * (1.0f / 3.0f);
    float avg_b = (v0->color.b + v1->color.b + v2->color.b) * (1.0f / 3.0f);

    /* PBR metallic: reduce diffuse, add environment reflection */
    float diffuse_scale = 1.0f - metallic;
    float env_flat =
        ambient * metallic * 0.3f; /* simplified env for flat path */
    float crf = avg_r * (lighting * diffuse_scale + env_flat);
    float cgf = avg_g * (lighting * diffuse_scale + env_flat);
    float cbf = avg_b * (lighting * diffuse_scale + env_flat);
    if (crf < 0.0f)
      crf = 0.0f;
    if (cgf < 0.0f)
      cgf = 0.0f;
    if (cbf < 0.0f)
      cbf = 0.0f;

    if (wireframe) {
      uint8_t cr8 = (uint8_t)((crf > 1.0f ? 1.0f : crf) * 255.0f);
      uint8_t cg8 = (uint8_t)((cgf > 1.0f ? 1.0f : cgf) * 255.0f);
      uint8_t cb8 = (uint8_t)((cbf > 1.0f ? 1.0f : cbf) * 255.0f);
      mop_sw_draw_line_aa(fb, sx0, sy0, sz0, sx1, sy1, sz1, cr8, cg8, cb8,
                          object_id, depth_test, 1.0f);
      mop_sw_draw_line_aa(fb, sx1, sy1, sz1, sx2, sy2, sz2, cr8, cg8, cb8,
                          object_id, depth_test, 1.0f);
      mop_sw_draw_line_aa(fb, sx2, sy2, sz2, sx0, sy0, sz0, cr8, cg8, cb8,
                          object_id, depth_test, 1.0f);
    } else {
      float caf = clamp01(opacity);
      rasterize_filled_triangle(fb, sx0, sy0, sz0, sx1, sy1, sz1, sx2, sy2, sz2,
                                crf, cgf, cbf, caf, object_id, depth_test,
                                blend_mode);
    }
  }
}

/* -------------------------------------------------------------------------
 * FXAA post-process pass (Fast Approximate Anti-Aliasing)
 *
 * Simplified FXAA 3.11 quality preset.  Operates in-place on the color
 * buffer.  Detects high-contrast edges via luma comparison and blends
 * along the dominant gradient direction.
 *
 * Applied once after all geometry is rendered, smoothing all remaining
 * aliased edges in the final image.
 * ------------------------------------------------------------------------- */

void mop_sw_fxaa(MopSwFramebuffer *fb) {
  int w = fb->width;
  int h = fb->height;
  if (w < 3 || h < 3)
    return;

  /* Use persistent scratch buffer for FXAA source copy */
  size_t buf_size = (size_t)w * (size_t)h * 4;
  uint8_t *src = fb->fxaa_scratch;
  if (!src)
    return;
  memcpy(src, fb->color, buf_size);

  /* FXAA constants */
  const float EDGE_THRESHOLD = 0.0625f;    /* 1/16 — minimum contrast */
  const float EDGE_THRESHOLD_MIN = 0.004f; /* skip very dark areas */
  const float SUBPIX_QUALITY = 0.75f;      /* sub-pixel AA strength */

/* Luma from RGBA8 at (bx,by) in the source buffer */
#define FXAA_LUMA(bx, by)                                                      \
  (0.299f * (float)src[((size_t)(by) * (size_t)w + (size_t)(bx)) * 4 + 0] +    \
   0.587f * (float)src[((size_t)(by) * (size_t)w + (size_t)(bx)) * 4 + 1] +    \
   0.114f * (float)src[((size_t)(by) * (size_t)w + (size_t)(bx)) * 4 + 2])

  for (int y = 1; y < h - 1; y++) {
    for (int x = 1; x < w - 1; x++) {
      /* Sample luma in 3x3 neighborhood */
      float lM = FXAA_LUMA(x, y);
      float lN = FXAA_LUMA(x, y - 1);
      float lS = FXAA_LUMA(x, y + 1);
      float lW = FXAA_LUMA(x - 1, y);
      float lE = FXAA_LUMA(x + 1, y);

      /* Range (max - min) of local contrast */
      float range_min = lM;
      if (lN < range_min)
        range_min = lN;
      if (lS < range_min)
        range_min = lS;
      if (lW < range_min)
        range_min = lW;
      if (lE < range_min)
        range_min = lE;

      float range_max = lM;
      if (lN > range_max)
        range_max = lN;
      if (lS > range_max)
        range_max = lS;
      if (lW > range_max)
        range_max = lW;
      if (lE > range_max)
        range_max = lE;

      float range = range_max - range_min;

      /* Skip if contrast is below threshold */
      if (range < EDGE_THRESHOLD * range_max + EDGE_THRESHOLD_MIN) {
        continue;
      }

      /* Diagonal lumas for sub-pixel filter */
      float lNW = FXAA_LUMA(x - 1, y - 1);
      float lNE = FXAA_LUMA(x + 1, y - 1);
      float lSW = FXAA_LUMA(x - 1, y + 1);
      float lSE = FXAA_LUMA(x + 1, y + 1);

      /* Sub-pixel amount: lowpass filter vs center */
      float luma_avg = (lN + lS + lW + lE) * (2.0f / 12.0f) +
                       (lNW + lNE + lSW + lSE) * (1.0f / 12.0f);
      float subpix = fabsf(luma_avg - lM);
      subpix = subpix / range;
      if (subpix > 1.0f)
        subpix = 1.0f;
      /* Smooth step */
      subpix = subpix * subpix * (3.0f - 2.0f * subpix);
      subpix *= SUBPIX_QUALITY;

      /* Determine edge orientation: horizontal or vertical */
      float edge_h = fabsf(-2.0f * lW + lNW + lSW) +
                     fabsf(-2.0f * lM + lN + lS) * 2.0f +
                     fabsf(-2.0f * lE + lNE + lSE);
      float edge_v = fabsf(-2.0f * lN + lNW + lNE) +
                     fabsf(-2.0f * lM + lW + lE) * 2.0f +
                     fabsf(-2.0f * lS + lSW + lSE);
      bool is_horiz = (edge_h >= edge_v);

      /* Step direction perpendicular to edge */
      int step_x = is_horiz ? 0 : 1;
      int step_y = is_horiz ? 1 : 0;

      /* Choose positive or negative direction */
      float luma_pos = is_horiz ? lS : lE;
      float luma_neg = is_horiz ? lN : lW;
      float grad_pos = fabsf(luma_pos - lM);
      float grad_neg = fabsf(luma_neg - lM);

      int off_x, off_y;
      if (grad_pos >= grad_neg) {
        off_x = step_x;
        off_y = step_y;
      } else {
        off_x = -step_x;
        off_y = -step_y;
      }

      /* Blend factor based on sub-pixel AA */
      float blend = subpix;

      /* Fetch neighbor pixel */
      int nx = x + off_x;
      int ny = y + off_y;
      if (nx < 0)
        nx = 0;
      if (nx >= w)
        nx = w - 1;
      if (ny < 0)
        ny = 0;
      if (ny >= h)
        ny = h - 1;

      /* Blend current pixel with neighbor */
      size_t ci = ((size_t)y * (size_t)w + (size_t)x) * 4;
      size_t ni = ((size_t)ny * (size_t)w + (size_t)nx) * 4;
      float inv_blend = 1.0f - blend;

      fb->color[ci + 0] =
          (uint8_t)(src[ci + 0] * inv_blend + src[ni + 0] * blend);
      fb->color[ci + 1] =
          (uint8_t)(src[ci + 1] * inv_blend + src[ni + 1] * blend);
      fb->color[ci + 2] =
          (uint8_t)(src[ci + 2] * inv_blend + src[ni + 2] * blend);
    }
  }

#undef FXAA_LUMA
}

/* -------------------------------------------------------------------------
 * HDR → LDR resolve — ACES Filmic tonemapping
 *
 * Converts the HDR float accumulation buffer to uint8 with ACES Filmic
 * tonemapping and exposure control.
 * ------------------------------------------------------------------------- */

static float aces_tonemap(float x) {
  float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
  float m = (x * (a * x + b)) / (x * (c * x + d) + e);
  return m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
}

void mop_sw_hdr_resolve(MopSwFramebuffer *fb, float exposure) {
  if (!fb || !fb->color_hdr)
    return;
  size_t n = (size_t)fb->width * fb->height;
  for (size_t i = 0; i < n; i++) {
    size_t ci = i * 4;
    /* object_id==0 marks background pixels (gradient / clear color).
     * Background brightness stays constant regardless of exposure —
     * only scene geometry (object_id > 0) gets tonemapped. */
    if (fb->object_id[i] == 0) {
      float r = fb->color_hdr[ci + 0];
      float g = fb->color_hdr[ci + 1];
      float b = fb->color_hdr[ci + 2];
      fb->color[ci + 0] = (uint8_t)(fminf(r, 1.0f) * 255.0f + 0.5f);
      fb->color[ci + 1] = (uint8_t)(fminf(g, 1.0f) * 255.0f + 0.5f);
      fb->color[ci + 2] = (uint8_t)(fminf(b, 1.0f) * 255.0f + 0.5f);
      fb->color[ci + 3] = 255;
    } else {
      float r = aces_tonemap(fb->color_hdr[ci + 0] * exposure);
      float g = aces_tonemap(fb->color_hdr[ci + 1] * exposure);
      float b = aces_tonemap(fb->color_hdr[ci + 2] * exposure);
      fb->color[ci + 0] = (uint8_t)(r * 255.0f + 0.5f);
      fb->color[ci + 1] = (uint8_t)(g * 255.0f + 0.5f);
      fb->color[ci + 2] = (uint8_t)(b * 255.0f + 0.5f);
      fb->color[ci + 3] = (uint8_t)(fb->color_hdr[ci + 3] * 255.0f + 0.5f);
    }
  }
}
