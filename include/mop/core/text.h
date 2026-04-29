/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * core/text.h — Text drawing primitives
 *
 * Three text-rendering modes — same SDF pipeline, different anchor
 * resolution.  Submitted per-frame; the viewport accumulates a queue
 * of text commands and rasterizes them after the scene + overlay
 * passes during readback compositing.
 *
 *   1. mop_text_draw_2d     — screen-pinned (HUD, navigator panels).
 *   2. mop_text_draw_label  — world-anchored, screen-aligned (selection
 *                              callouts, gizmo labels).               [SOON]
 *   3. mop_text_draw_3d     — world-embedded (user-placed scene text). [SOON]
 *
 * For v1 only mop_text_draw_2d is wired through to the rasterizer;
 * the other two are stubbed and reserved.  All three share the same
 * MopTextStyle.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_TEXT_H
#define MOP_CORE_TEXT_H

#include <mop/core/font.h>
#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Style
 *
 * px_size : pixel height of one em.  Standard tooling sizes:
 *             10 → axis labels / dense lists
 *             12 → navigator rows, breadcrumb
 *             13 → selection-callout primary line
 *             16 → emphasized body
 * color   : linear RGBA, gets straight-alpha-blended into the
 *           framebuffer.  Use the brand tokens defined below for
 *           consistency.
 * flags   : reserved (outline / drop-shadow land here in slice 3).
 * ------------------------------------------------------------------------- */

typedef struct MopTextStyle {
  MopColor color;
  float px_size;
  uint32_t flags;

  /* Stroke weight — shifts the SDF iso-contour outward so glyphs
   * appear thicker without baking a separate Bold atlas.
   *
   *   0.00  Regular  (default)
   *   0.10  Medium-ish
   *   0.18  Semibold
   *   0.30  Bold
   *
   * Values much beyond 0.35 start clipping into adjacent glyphs.
   * Shipping a real Bold variant via `make fonts` will eventually
   * replace this trick — for now it gives a single-atlas weight
   * range tunable per-call. */
  float weight;

  /* Optional filled background pill behind the text — used by
   * selection callouts to maximize legibility against arbitrary
   * scenes.  The pill is a sharp-cornered rectangle drawn first;
   * text composites on top.  Set bg_color.a = 0 to disable.
   *
   * bg_padding extends the pill by `bg_padding` presentation-px
   * on every side beyond the text bbox. */
  MopColor bg_color;
  float bg_padding;
} MopTextStyle;

/* -------------------------------------------------------------------------
 * Brand colors — single source of truth for design-language hits.
 *
 * These are linear-space RGBA; the post-process gamma stage turns
 * them into the same near-black / bone / gel-red the design language
 * specifies in screen space.
 * ------------------------------------------------------------------------- */

#define MOP_COLOR_BONE                                                         \
  ((MopColor){0.93f, 0.92f, 0.88f, 1.0f}) /* warm off-white     */
#define MOP_COLOR_BONE_DIM                                                     \
  ((MopColor){0.55f, 0.54f, 0.51f, 1.0f}) /* secondary text     */
#define MOP_COLOR_GEL_RED                                                      \
  ((MopColor){0.88f, 0.18f, 0.20f, 1.0f}) /* accent / selection */
#define MOP_COLOR_NEAR_BLACK                                                   \
  ((MopColor){0.055f, 0.055f, 0.063f, 1.0f}) /* viewport BG       */

/* -------------------------------------------------------------------------
 * 2D screen-pinned text.
 *
 *   pixel_x, pixel_y : top-left baseline anchor in framebuffer pixels.
 *                      The pen is positioned at (pixel_x, pixel_y +
 *                      ascent_px) so that pixel_y is the top of the
 *                      glyph cell — matches typical UI layout where
 *                      callers think in cell tops, not baselines.
 *   utf8             : caller-owned UTF-8 string; the viewport copies
 *                      it into the per-frame queue.  Safe to free or
 *                      mutate immediately after the call returns.
 *
 * If `font` is NULL, the embedded HUD font (mop_font_hud()) is used.
 * If both are NULL, the call is a no-op.
 * ------------------------------------------------------------------------- */

void mop_text_draw_2d(MopViewport *vp, const MopFont *font, const char *utf8,
                      float pixel_x, float pixel_y, MopTextStyle style);

/* -------------------------------------------------------------------------
 * World-anchored, screen-aligned label.
 *
 * Anchors the text to a 3D mesh — its world AABB top-center (or
 * pivot, depending on `anchor`) is projected to screen each frame,
 * and the text is drawn at a small pixel offset above the projected
 * anchor.  Size and orientation are screen-space; the label never
 * scales with distance.
 *
 * Multi-label submissions on overlapping anchors are placed via a
 * greedy push-up stacking pass so labels never sit on top of each
 * other.  When a label would collide with a previously placed one,
 * the newcomer is pushed further above its anchor until clear.
 *
 * Depth-mode v1: only MOP_LABEL_ALWAYS_ON_TOP is honored.  The
 * other values are accepted (validation forward-compatible) but
 * currently render the same as ALWAYS_ON_TOP.
 *
 * `target` MUST be a valid mesh handle owned by `vp`.  The label
 * does not extend the mesh's lifetime — if the mesh is removed
 * before render, the label is silently dropped.
 * ------------------------------------------------------------------------- */

typedef struct MopMesh MopMesh;

typedef enum MopLabelAnchor {
  MOP_LABEL_TOP_CENTER = 0, /* world AABB top-center, +Y up                  */
  MOP_LABEL_BOTTOM_CENTER = 1, /* world AABB bottom-center */
  MOP_LABEL_FOLLOW_PIVOT = 2, /* mesh's translation (pivot) — cheaper, robust */
} MopLabelAnchor;

typedef enum MopLabelDepth {
  MOP_LABEL_ALWAYS_ON_TOP = 0, /* draw on top of scene (default) */
  MOP_LABEL_FADE_OCCLUDED = 1, /* dim when behind geometry          [SOON] */
  MOP_LABEL_DEPTH_TEST = 2, /* hard occlude when behind geometry [SOON]     */
} MopLabelDepth;

/* Cap on labels rasterized in one frame.  Beyond this, additional
 * labels are dropped — slice-4 will introduce an aggregate badge. */
#define MOP_LABEL_MAX_VISIBLE 32

void mop_text_draw_label(MopViewport *vp, const MopFont *font, MopMesh *target,
                         const char *utf8, MopLabelAnchor anchor,
                         MopLabelDepth depth_mode, MopTextStyle style);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_TEXT_H */
