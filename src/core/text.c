/*
 * Master of Puppets — Text Drawing
 * core/text.c — Submission queue + CPU SDF rasterizer
 *
 * The viewport queues text commands during the frame; this file
 * walks the queue at readback time and blits each glyph's SDF onto
 * the RGBA8 framebuffer using the standard MSDF/SDF screen-space
 * derivative trick for a single-pixel transition band.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/font_internal.h"
#include "core/viewport_internal.h"
#include <mop/core/font.h>
#include <mop/core/text.h>
#include <mop/query/spatial.h>
#include <mop/util/log.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MOP_TEXT_INITIAL_CAP 16u

/* -------------------------------------------------------------------------
 * UTF-8 walker — consumes one codepoint, advances the pointer.
 * Returns 0xFFFD on malformed input so the layout pass keeps moving.
 * ------------------------------------------------------------------------- */

static uint32_t text_utf8_next(const char **pp) {
  const unsigned char *p = (const unsigned char *)*pp;
  uint32_t cp;
  if (p[0] < 0x80) {
    cp = p[0];
    *pp += 1;
  } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
    cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    *pp += 2;
  } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
             (p[2] & 0xC0) == 0x80) {
    cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
         (p[2] & 0x3F);
    *pp += 3;
  } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
             (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
    cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
         ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    *pp += 4;
  } else {
    cp = 0xFFFDu;
    *pp += 1;
  }
  return cp;
}

/* -------------------------------------------------------------------------
 * Queue management
 * ------------------------------------------------------------------------- */

void mop_text_queue_reset(MopViewport *vp) {
  if (!vp || !vp->text_prims)
    return;
  for (uint32_t i = 0; i < vp->text_prim_count; i++) {
    free(vp->text_prims[i].utf8);
    vp->text_prims[i].utf8 = NULL;
  }
  vp->text_prim_count = 0;
}

void mop_text_queue_destroy(MopViewport *vp) {
  if (!vp)
    return;
  mop_text_queue_reset(vp);
  free(vp->text_prims);
  vp->text_prims = NULL;
  vp->text_prim_capacity = 0;
}

static struct MopTextPrim *queue_acquire(MopViewport *vp) {
  if (vp->text_prim_count >= vp->text_prim_capacity) {
    if (!mop_dyn_grow((void **)&vp->text_prims, &vp->text_prim_capacity,
                      sizeof(struct MopTextPrim), MOP_TEXT_INITIAL_CAP))
      return NULL;
  }
  return &vp->text_prims[vp->text_prim_count++];
}

/* -------------------------------------------------------------------------
 * Public: 2D screen-pinned submission
 * ------------------------------------------------------------------------- */

void mop_text_draw_2d(MopViewport *vp, const MopFont *font, const char *utf8,
                      float pixel_x, float pixel_y, MopTextStyle style) {
  if (!vp || !utf8 || style.px_size <= 0.0f)
    return;
  if (!font)
    font = mop_font_hud();
  if (!font)
    return; /* no font available — silent no-op */

  size_t len = strlen(utf8);
  if (len == 0)
    return;

  MOP_VP_LOCK(vp);
  struct MopTextPrim *p = queue_acquire(vp);
  if (!p) {
    MOP_VP_UNLOCK(vp);
    return;
  }
  char *copy = (char *)malloc(len + 1);
  if (!copy) {
    /* Roll the count back so we don't leave a torn entry behind. */
    vp->text_prim_count--;
    MOP_VP_UNLOCK(vp);
    return;
  }
  memcpy(copy, utf8, len + 1);
  p->font = font;
  p->utf8 = copy;
  p->x = pixel_x;
  p->y = pixel_y;
  p->px_size = style.px_size;
  p->color = style.color;
  p->flags = style.flags;
  p->weight = style.weight;
  p->bg_color = style.bg_color;
  p->bg_padding = style.bg_padding;
  p->target = NULL;
  p->anchor = MOP_LABEL_TOP_CENTER;
  p->depth_mode = MOP_LABEL_ALWAYS_ON_TOP;
  MOP_VP_UNLOCK(vp);
}

/* -------------------------------------------------------------------------
 * Public: world-anchored label submission
 *
 * The rasterizer projects the mesh's world AABB top-center (or pivot)
 * each frame.  Submission only stashes the mesh pointer + offset; the
 * projection is deferred so a moving camera or animated mesh always
 * gets the up-to-date screen position.
 *
 * The (x, y) on the prim is repurposed as a *pixel offset* from the
 * projected anchor — by convention y is negative to draw above the
 * anchor.  We pre-set a sensible default (12 px above) so callers
 * who don't care about offset get the right thing.
 * ------------------------------------------------------------------------- */

void mop_text_draw_label(MopViewport *vp, const MopFont *font, MopMesh *target,
                         const char *utf8, MopLabelAnchor anchor,
                         MopLabelDepth depth_mode, MopTextStyle style) {
  if (!vp || !target || !utf8 || style.px_size <= 0.0f)
    return;
  if (!font)
    font = mop_font_hud();
  if (!font)
    return;

  size_t len = strlen(utf8);
  if (len == 0)
    return;

  MOP_VP_LOCK(vp);
  struct MopTextPrim *p = queue_acquire(vp);
  if (!p) {
    MOP_VP_UNLOCK(vp);
    return;
  }
  char *copy = (char *)malloc(len + 1);
  if (!copy) {
    vp->text_prim_count--;
    MOP_VP_UNLOCK(vp);
    return;
  }
  memcpy(copy, utf8, len + 1);
  p->font = font;
  p->utf8 = copy;
  /* Pixel offset from the projected anchor — 12 px above is the
   * design-language default for selection callouts. */
  p->x = 0.0f;
  p->y = -12.0f - style.px_size; /* lift label by px_size so its
                                  * baseline floats *above* the anchor,
                                  * not bleeding into the mesh. */
  p->px_size = style.px_size;
  p->color = style.color;
  p->flags = style.flags;
  p->weight = style.weight;
  p->bg_color = style.bg_color;
  p->bg_padding = style.bg_padding;
  p->target = target;
  p->anchor = (int)anchor;
  p->depth_mode = (int)depth_mode;
  MOP_VP_UNLOCK(vp);
}

/* -------------------------------------------------------------------------
 * CPU rasterizer — the SDF blit.
 *
 * Per glyph:
 *   1. Compute the screen-space quad from the glyph's plane bounds
 *      and the requested px_size.
 *   2. Clip the quad to the framebuffer.
 *   3. For each pixel in the clipped quad, bilinearly sample the
 *      atlas, convert SDF byte → screen-space signed distance, and
 *      derive coverage as clamp(sd_screen + 0.5, 0, 1).
 *   4. Straight-alpha-blend (text_color * coverage) into RGBA8.
 *
 * The SDF atlas was baked with onedge_value = 128 and
 * pixel_dist_scale = 128 / px_range, so:
 *   sd_atlas_px = (byte - 128) * px_range / 128
 *   sd_screen_px = sd_atlas_px * (px_size / em_size)
 * ------------------------------------------------------------------------- */

static inline float clamp01(float x) {
  return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static inline float bilinear_sample(const uint8_t *atlas, int atlas_w,
                                    int atlas_h, float u, float v) {
  /* Clamp to atlas bounds so glyphs near the edge don't pick up
   * neighbor data. */
  if (u < 0.0f)
    u = 0.0f;
  if (v < 0.0f)
    v = 0.0f;
  if (u > (float)(atlas_w - 1))
    u = (float)(atlas_w - 1);
  if (v > (float)(atlas_h - 1))
    v = (float)(atlas_h - 1);

  int u0 = (int)u;
  int v0 = (int)v;
  int u1 = u0 + 1 < atlas_w ? u0 + 1 : u0;
  int v1 = v0 + 1 < atlas_h ? v0 + 1 : v0;
  float fu = u - (float)u0;
  float fv = v - (float)v0;

  float a = (float)atlas[v0 * atlas_w + u0];
  float b = (float)atlas[v0 * atlas_w + u1];
  float c = (float)atlas[v1 * atlas_w + u0];
  float d = (float)atlas[v1 * atlas_w + u1];
  float ab = a + (b - a) * fu;
  float cd = c + (d - c) * fu;
  return ab + (cd - ab) * fv;
}

/* Paint a solid rectangle in sRGB-encoded space onto the framebuffer.
 * Used for the optional background pill behind label text.  The
 * coordinates are framebuffer pixels (post-ssaa scaling). */
static void fill_rect(uint8_t *rgba, int fb_w, int fb_h, float x0, float y0,
                      float x1, float y1, MopColor color) {
  if (color.a <= 0.0f)
    return;
  int ix0 = (int)floorf(x0);
  int iy0 = (int)floorf(y0);
  int ix1 = (int)ceilf(x1);
  int iy1 = (int)ceilf(y1);
  if (ix0 < 0)
    ix0 = 0;
  if (iy0 < 0)
    iy0 = 0;
  if (ix1 > fb_w)
    ix1 = fb_w;
  if (iy1 > fb_h)
    iy1 = fb_h;

  /* Same gamma-2.0 sRGB approximation we use for glyphs. */
  float r = sqrtf(color.r < 0.0f ? 0.0f : (color.r > 1.0f ? 1.0f : color.r));
  float g = sqrtf(color.g < 0.0f ? 0.0f : (color.g > 1.0f ? 1.0f : color.g));
  float b = sqrtf(color.b < 0.0f ? 0.0f : (color.b > 1.0f ? 1.0f : color.b));
  float a = color.a < 0.0f ? 0.0f : (color.a > 1.0f ? 1.0f : color.a);
  float inv_a = 1.0f - a;

  for (int y = iy0; y < iy1; y++) {
    uint8_t *row = rgba + (size_t)y * (size_t)fb_w * 4u;
    for (int x = ix0; x < ix1; x++) {
      uint8_t *p = row + (size_t)x * 4u;
      float fr = (float)p[0] / 255.0f;
      float fg = (float)p[1] / 255.0f;
      float fb_ = (float)p[2] / 255.0f;
      fr = r * a + fr * inv_a;
      fg = g * a + fg * inv_a;
      fb_ = b * a + fb_ * inv_a;
      if (fr < 0.0f)
        fr = 0.0f;
      if (fr > 1.0f)
        fr = 1.0f;
      if (fg < 0.0f)
        fg = 0.0f;
      if (fg > 1.0f)
        fg = 1.0f;
      if (fb_ < 0.0f)
        fb_ = 0.0f;
      if (fb_ > 1.0f)
        fb_ = 1.0f;
      p[0] = (uint8_t)(fr * 255.0f + 0.5f);
      p[1] = (uint8_t)(fg * 255.0f + 0.5f);
      p[2] = (uint8_t)(fb_ * 255.0f + 0.5f);
    }
  }
}

/* Composite linear-RGBA premultiplied glyph coverage onto an sRGB-ish
 * RGBA8 framebuffer pixel.  The framebuffer has already gone through
 * tonemap + gamma at this point in the pipeline (overlays composite
 * after post-process), so we treat fb as sRGB and convert color to
 * sRGB on the fly via a fast gamma-2.0 approximation. */
static inline void blend_pixel(uint8_t *p, MopColor color, float coverage) {
  if (coverage <= 0.0f)
    return;
  /* gamma-2.0 approx — sufficient at text sizes; full sRGB curve
   * would be three powf calls per pixel. */
  float r = sqrtf(clamp01(color.r));
  float g = sqrtf(clamp01(color.g));
  float b = sqrtf(clamp01(color.b));
  float a = clamp01(color.a) * coverage;
  float inv_a = 1.0f - a;

  float fr = (float)p[0] / 255.0f;
  float fg = (float)p[1] / 255.0f;
  float fb = (float)p[2] / 255.0f;

  fr = r * a + fr * inv_a;
  fg = g * a + fg * inv_a;
  fb = b * a + fb * inv_a;

  p[0] = (uint8_t)(clamp01(fr) * 255.0f + 0.5f);
  p[1] = (uint8_t)(clamp01(fg) * 255.0f + 0.5f);
  p[2] = (uint8_t)(clamp01(fb) * 255.0f + 0.5f);
  /* Leave alpha channel untouched — viewports are typically opaque. */
}

/* -------------------------------------------------------------------------
 * Project a world-space point onto the screen in presentation pixels.
 * Returns 0 if the point is behind the camera, in which case the
 * label is skipped for this frame.
 * ------------------------------------------------------------------------- */

static int project_world_to_screen(const MopViewport *vp, MopVec3 world,
                                   int presentation_w, int presentation_h,
                                   float *out_x, float *out_y) {
  MopMat4 vp_mat = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);
  MopVec4 wp = {world.x, world.y, world.z, 1.0f};
  MopVec4 clip = mop_mat4_mul_vec4(vp_mat, wp);
  if (clip.w <= 0.001f)
    return 0;
  float ndc_x = clip.x / clip.w;
  float ndc_y = clip.y / clip.w;
  *out_x = (ndc_x * 0.5f + 0.5f) * (float)presentation_w;
  *out_y = (1.0f - (ndc_y * 0.5f + 0.5f)) * (float)presentation_h;
  return 1;
}

/* -------------------------------------------------------------------------
 * Resolve an anchor to a world-space point.  Falls back to the mesh
 * pivot when the AABB query yields an obviously-degenerate box (e.g.,
 * a freshly-created mesh whose AABB hasn't been computed).
 * ------------------------------------------------------------------------- */

static MopVec3 resolve_anchor(const MopViewport *vp, const MopMesh *mesh,
                              int anchor) {
  if (anchor == MOP_LABEL_FOLLOW_PIVOT) {
    /* Pivot = world translation row of the world transform.  Column-
     * major: translation lives at d[12], d[13], d[14]. */
    return (MopVec3){mesh->world_transform.d[12], mesh->world_transform.d[13],
                     mesh->world_transform.d[14]};
  }
  MopAABB box = mop_mesh_get_aabb_world(mesh, vp);
  float center_x = (box.min.x + box.max.x) * 0.5f;
  float center_z = (box.min.z + box.max.z) * 0.5f;
  float y = (anchor == MOP_LABEL_BOTTOM_CENTER) ? box.min.y : box.max.y;
  return (MopVec3){center_x, y, center_z};
}

static void rasterize_one(uint8_t *rgba, int fb_w, int fb_h,
                          const struct MopTextPrim *p, float pixel_scale,
                          float pres_origin_x, float pres_origin_y) {
  const MopFont *font = p->font;
  if (!font)
    return;
  if (pixel_scale <= 0.0f)
    pixel_scale = 1.0f;

  int atlas_w, atlas_h, atlas_ch;
  const uint8_t *atlas =
      mop_font_atlas_pixels(font, &atlas_w, &atlas_h, &atlas_ch);
  if (!atlas || atlas_ch != 1)
    return; /* MSDF path lands in a later slice */

  float px_range = mop_font_px_range(font);
  float em_size = mop_font_em_size(font);
  if (em_size <= 0.0f || px_range <= 0.0f)
    return;

  MopFontMetrics m = mop_font_metrics(font);

  /* Scale presentation-size submission to internal-FB coordinates.
   * The caller passed the cell-top origin in presentation px; we
   * scale it here so the rasterizer paints into the SSAA buffer at
   * the right resolution. */
  float px_size_fb = p->px_size * pixel_scale;
  float origin_x = pres_origin_x * pixel_scale;
  float origin_y = pres_origin_y * pixel_scale;

  /* Optional background pill — drawn first so glyphs composite on
   * top.  The pill bounds the full text bbox + padding; we measure
   * once via mop_text_measure to get the width.  Multi-line strings
   * use line_height * num_lines for the vertical extent. */
  if (p->bg_color.a > 0.0f) {
    float text_w_pres = mop_text_measure(font, p->utf8, p->px_size);
    /* Count newlines for multi-line height. */
    int line_count = 1;
    for (const char *s = p->utf8; *s; s++) {
      if (*s == '\n')
        line_count++;
    }
    float text_h_pres = m.line_height * p->px_size * (float)line_count;
    float pad = p->bg_padding * pixel_scale;
    float bx0 = pres_origin_x * pixel_scale - pad;
    float by0 = pres_origin_y * pixel_scale - pad;
    float bx1 = (pres_origin_x + text_w_pres) * pixel_scale + pad;
    float by1 = (pres_origin_y + text_h_pres) * pixel_scale + pad;
    fill_rect(rgba, fb_w, fb_h, bx0, by0, bx1, by1, p->bg_color);
  }

  /* The caller passes (pixel_x, pixel_y) as the top-left of the
   * glyph cell.  Convert to baseline-relative pen coords. */
  float pen_x = origin_x;
  float baseline_y = origin_y + m.ascent * px_size_fb;

  /* Per the canonical MSDF/SDF shader, the byte→signed-distance map
   * is sd_norm = (sample - 128) / 128 ∈ [-1, 1], and the screen-px
   * distance is sd_norm * screenPxRange, where:
   *
   *   screenPxRange = pxRange * (px_size / em_size)
   *
   * is the SDF transition band measured in screen pixels.  Clamp
   * the band to ≥ 1.0 px so tiny render sizes still saturate to
   * full coverage in glyph interiors — without this clamp, sub-px
   * transitions starve the inner pixels of solid alpha and text
   * comes out gray instead of bone-bright. */
  float screen_px_range = px_range * (px_size_fb / em_size);
  if (screen_px_range < 1.0f)
    screen_px_range = 1.0f;
  const float sd_to_screen = screen_px_range / 128.0f;

  /* Stroke weight — additive bias on the iso-contour.  A value of
   * 0.0 leaves the regular-weight glyph alone; positive values
   * thicken the stroke uniformly.  Clamp into a sane range so a
   * runaway caller can't blow glyphs into white blobs. */
  float weight_bias = p->weight;
  if (weight_bias < 0.0f)
    weight_bias = 0.0f;
  if (weight_bias > 0.45f)
    weight_bias = 0.45f;

  const MopFontGlyph *prev = NULL;

  for (const char *s = p->utf8; *s;) {
    uint32_t cp = text_utf8_next(&s);
    if (cp == '\n') {
      pen_x = origin_x;
      baseline_y += m.line_height * px_size_fb;
      prev = NULL;
      continue;
    }

    const MopFontGlyph *g = mop_font_lookup_glyph(font, cp);
    if (!g) {
      pen_x += 0.5f * px_size_fb; /* fallback advance */
      prev = NULL;
      continue;
    }
    if (prev)
      pen_x += mop_font_kerning(font, prev, g) * px_size_fb;

    /* Skip non-printing glyphs (e.g., space) — they have no atlas
     * footprint but still advance the pen. */
    int gw = g->atlas_uv_max_x - g->atlas_uv_min_x;
    int gh = g->atlas_uv_max_y - g->atlas_uv_min_y;
    if (gw <= 0 || gh <= 0) {
      pen_x += g->advance * px_size_fb;
      prev = g;
      continue;
    }

    /* Quad in screen pixels.  plane_min/max are em-relative,
     * Y-up baseline-relative — convert to Y-down screen by
     * negating against the baseline. */
    float qx0 = pen_x + g->plane_min_x * px_size_fb;
    float qx1 = pen_x + g->plane_max_x * px_size_fb;
    float qy0 = baseline_y - g->plane_max_y * px_size_fb;
    float qy1 = baseline_y - g->plane_min_y * px_size_fb;

    int ix0 = (int)floorf(qx0);
    int iy0 = (int)floorf(qy0);
    int ix1 = (int)ceilf(qx1);
    int iy1 = (int)ceilf(qy1);

    if (ix0 < 0)
      ix0 = 0;
    if (iy0 < 0)
      iy0 = 0;
    if (ix1 > fb_w)
      ix1 = fb_w;
    if (iy1 > fb_h)
      iy1 = fb_h;

    /* Inverse quad span — used for screen-px → atlas-px mapping. */
    float qsx = qx1 - qx0;
    float qsy = qy1 - qy0;
    if (qsx <= 0.0f || qsy <= 0.0f) {
      pen_x += g->advance * px_size_fb;
      prev = g;
      continue;
    }
    float inv_qsx = 1.0f / qsx;
    float inv_qsy = 1.0f / qsy;
    float au_min = (float)g->atlas_uv_min_x;
    float av_min = (float)g->atlas_uv_min_y;
    float au_span = (float)gw;
    float av_span = (float)gh;

    for (int sy = iy0; sy < iy1; sy++) {
      float fy = ((float)sy + 0.5f - qy0) * inv_qsy;
      float av = av_min + fy * av_span;
      uint8_t *row = rgba + (size_t)sy * (size_t)fb_w * 4u;
      for (int sx = ix0; sx < ix1; sx++) {
        float fx = ((float)sx + 0.5f - qx0) * inv_qsx;
        float au = au_min + fx * au_span;

        float sample = bilinear_sample(atlas, atlas_w, atlas_h, au, av);
        float sd_screen = (sample - 128.0f) * sd_to_screen;
        float coverage = clamp01(sd_screen + 0.5f + weight_bias);
        if (coverage > 0.0f)
          blend_pixel(row + (size_t)sx * 4u, p->color, coverage);
      }
    }

    pen_x += g->advance * px_size_fb;
    prev = g;
  }
}

/* -------------------------------------------------------------------------
 * Stacking layout — for label prims only.
 *
 * Walks the queue once to collect labels, projects each anchor to
 * presentation-pixel screen space, computes a tight bbox using the
 * pre-baked metrics, and runs a greedy push-up pass so overlapping
 * labels don't sit on top of each other.  The final origin lands in
 * the parallel `final_origin_y[]` array indexed by prim index.
 *
 * Beyond MOP_LABEL_MAX_VISIBLE labels the overflow is dropped — the
 * stacking pass would degenerate into a wall of text otherwise.
 * (Slice 4 will replace the overflow drop with an aggregate badge.)
 * ------------------------------------------------------------------------- */

typedef struct LabelLayout {
  uint32_t prim_index;
  float origin_x; /* presentation-pixel cell-top X (final after stack)        */
  float origin_y; /* presentation-pixel cell-top Y (final after stack)        */
  float width;    /* presentation-pixel cell width                            */
  float height;   /* presentation-pixel cell height                           */
  uint8_t valid;  /* 0 = drop this prim (behind camera, mesh inactive, …)    */
} LabelLayout;

static int compare_label_anchor_y(const void *a, const void *b) {
  float ya = ((const LabelLayout *)a)->origin_y;
  float yb = ((const LabelLayout *)b)->origin_y;
  return (ya > yb) - (ya < yb);
}

static void stack_labels(MopViewport *vp, LabelLayout *labels, int n,
                         int presentation_w, int presentation_h) {
  (void)presentation_w;
  /* Sort top-to-bottom by initial origin_y (smaller = higher). */
  qsort(labels, (size_t)n, sizeof(LabelLayout), compare_label_anchor_y);

  /* Greedy place: for each label in sorted order, push up while it
   * vertically overlaps any already-placed label whose horizontal
   * span overlaps.  Cap N is small (<= MOP_LABEL_MAX_VISIBLE) so
   * the O(N²) check is fine. */
  for (int i = 0; i < n; i++) {
    if (!labels[i].valid)
      continue;
    int adjusted = 1;
    int guard = 0; /* hard cap to avoid pathological loops          */
    while (adjusted && guard++ < n * 2) {
      adjusted = 0;
      for (int j = 0; j < i; j++) {
        if (!labels[j].valid)
          continue;
        const LabelLayout *a = &labels[i];
        const LabelLayout *b = &labels[j];
        /* Horizontal overlap test (bboxes share any x range). */
        int h_overlap = !(a->origin_x + a->width < b->origin_x ||
                          b->origin_x + b->width < a->origin_x);
        if (!h_overlap)
          continue;
        /* Vertical overlap test. */
        int v_overlap = !(a->origin_y + a->height < b->origin_y ||
                          b->origin_y + b->height < a->origin_y);
        if (!v_overlap)
          continue;
        /* Push `i` up so its bottom sits 4 px above `j`'s top. */
        labels[i].origin_y = b->origin_y - a->height - 4.0f;
        adjusted = 1;
        break;
      }
    }
    /* If the label has been pushed off the top of the viewport, drop
     * it — anything else would be cosmetically broken. */
    if (labels[i].origin_y < 0.0f)
      labels[i].valid = 0;
    if (labels[i].origin_y > (float)presentation_h)
      labels[i].valid = 0;
  }
}

/* -------------------------------------------------------------------------
 * Inline text rasterization — drives a single string at framebuffer
 * coordinates with no queue or anchor projection.  Used by the
 * overlay rasterizer to service MOP_PRIM_TEXT primitives so navigator
 * / gizmo letters are drawn z-ordered with the surrounding overlay
 * primitives.
 *
 * We synthesize a transient MopTextPrim on the stack and reuse the
 * same rasterize_one path the regular queue uses.  pixel_scale = 1.0
 * because the caller has already scaled into framebuffer pixels.
 * ------------------------------------------------------------------------- */

void mop_text_rasterize_inline(uint8_t *rgba, int fb_w, int fb_h,
                               const MopFont *font, const char *utf8,
                               float fb_x, float fb_y, float fb_px_size,
                               MopColor color, float weight) {
  if (!rgba || !utf8 || fb_w <= 0 || fb_h <= 0 || fb_px_size <= 0.0f)
    return;
  if (!font)
    font = mop_font_hud();
  if (!font)
    return;

  struct MopTextPrim tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp.font = font;
  tmp.utf8 = (char *)utf8; /* read-only; rasterize_one doesn't mutate */
  tmp.x = fb_x;
  tmp.y = fb_y;
  tmp.px_size = fb_px_size;
  tmp.color = color;
  tmp.weight = weight;
  /* bg_color.a defaults to 0 → no pill, target = NULL → 2D mode. */
  rasterize_one(rgba, fb_w, fb_h, &tmp, 1.0f, fb_x, fb_y);
}

void mop_text_rasterize_cpu(MopViewport *vp, uint8_t *rgba, int w, int h,
                            const struct MopTextPrim *prims, uint32_t count,
                            float pixel_scale) {
  if (!rgba || !prims || count == 0)
    return;

  /* Presentation size = framebuffer / ssaa.  Used by the projection
   * step (which always speaks presentation pixels) and as the canvas
   * for the stacking pass. */
  if (pixel_scale <= 0.0f)
    pixel_scale = 1.0f;
  int pres_w = (int)((float)w / pixel_scale + 0.5f);
  int pres_h = (int)((float)h / pixel_scale + 0.5f);

  /* Phase 1: project + measure every label prim.  2D screen-pinned
   * prims skip this and rasterize directly with their submitted
   * (x, y).  Build a parallel layout table indexed by prim index. */
  LabelLayout layout[MOP_LABEL_MAX_VISIBLE];
  int label_count = 0;

  for (uint32_t i = 0; i < count && label_count < MOP_LABEL_MAX_VISIBLE; i++) {
    const struct MopTextPrim *p = &prims[i];
    if (!p->target)
      continue;
    /* Drop labels for inactive meshes — the host may have removed
     * the mesh between submission and render. */
    if (!p->target->active)
      continue;
    MopVec3 anchor_world = resolve_anchor(vp, p->target, p->anchor);
    float screen_x = 0.0f, screen_y = 0.0f;
    if (!project_world_to_screen(vp, anchor_world, pres_w, pres_h, &screen_x,
                                 &screen_y)) {
      continue; /* behind camera — silently skip */
    }
    float text_w = mop_text_measure(p->font, p->utf8, p->px_size);
    float text_h = mop_font_metrics(p->font).line_height * p->px_size;
    /* Center horizontally over the anchor — common DCC convention. */
    float origin_x = screen_x - text_w * 0.5f + p->x;
    float origin_y = screen_y + p->y;
    layout[label_count].prim_index = i;
    layout[label_count].origin_x = origin_x;
    layout[label_count].origin_y = origin_y;
    layout[label_count].width = text_w;
    layout[label_count].height = text_h;
    layout[label_count].valid = 1;
    label_count++;
  }

  /* Phase 2: stack overlapping labels. */
  if (label_count > 1)
    stack_labels(vp, layout, label_count, pres_w, pres_h);

  /* Phase 3: rasterize every prim.
   *   - 2D prims (target == NULL) use the submitted (p->x, p->y)
   *     directly.
   *   - Label prims look up their resolved origin in `layout[]`. */
  for (uint32_t i = 0; i < count; i++) {
    const struct MopTextPrim *p = &prims[i];
    if (!p->target) {
      rasterize_one(rgba, w, h, p, pixel_scale, p->x, p->y);
      continue;
    }
    /* Find the label in the layout table.  Linear scan over <= 32
     * entries is cheaper than a parallel map. */
    for (int k = 0; k < label_count; k++) {
      if (layout[k].prim_index != i)
        continue;
      if (!layout[k].valid)
        break;
      rasterize_one(rgba, w, h, p, pixel_scale, layout[k].origin_x,
                    layout[k].origin_y);
      break;
    }
  }
}
