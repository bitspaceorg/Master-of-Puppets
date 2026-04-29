/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * overlay.h — Overlay registration and dispatch
 *
 * Overlays are draw callbacks invoked after the main scene pass.
 * Built-in overlays (wireframe, normals, bounds, selection) are
 * pre-registered.  Applications can register custom overlays.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_OVERLAY_H
#define MOP_CORE_OVERLAY_H

#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Built-in overlay IDs
 * ------------------------------------------------------------------------- */

typedef enum MopOverlayId {
  MOP_OVERLAY_WIREFRAME = 0, /* wireframe on shaded */
  MOP_OVERLAY_NORMALS = 1,   /* vertex normal lines */
  MOP_OVERLAY_BOUNDS = 2,    /* per-mesh bounding boxes */
  MOP_OVERLAY_SELECTION = 3, /* selection highlight */
  MOP_OVERLAY_OUTLINE = 4,   /* always-on object outline (accent color) */
  MOP_OVERLAY_SKELETON = 5,  /* bone hierarchy lines + joint indicators */
  MOP_OVERLAY_BUILTIN_COUNT = 6,
} MopOverlayId;

/* -------------------------------------------------------------------------
 * Custom overlay callback
 * ------------------------------------------------------------------------- */

typedef void (*MopOverlayFn)(MopViewport *vp, void *user_data);

/* -------------------------------------------------------------------------
 * Overlay entry — internal storage for both built-in and custom overlays
 * ------------------------------------------------------------------------- */

typedef struct MopOverlayEntry {
  const char *name;
  MopOverlayFn draw_fn;
  void *user_data;
  bool active;
} MopOverlayEntry;

/* -------------------------------------------------------------------------
 * Overlay management
 * ------------------------------------------------------------------------- */

/* Register a custom overlay.  Returns the overlay's slot index (handle),
 * or UINT32_MAX on failure. */
uint32_t mop_viewport_add_overlay(MopViewport *vp, const char *name,
                                  MopOverlayFn draw_fn, void *user_data);

/* Remove a custom overlay by handle. */
void mop_viewport_remove_overlay(MopViewport *vp, uint32_t handle);

/* Enable/disable an overlay by its index (works for built-in and custom). */
void mop_viewport_set_overlay_enabled(MopViewport *vp, uint32_t id,
                                      bool enabled);
bool mop_viewport_get_overlay_enabled(const MopViewport *vp, uint32_t id);

/* -------------------------------------------------------------------------
 * Per-frame line submission
 *
 * Push a line into the overlay queue. Lines are CPU-rasterized on top of
 * the readback color buffer once per frame, so they composite correctly on
 * every backend (CPU, OpenGL, Vulkan) and respect the depth value passed
 * (smaller = closer to camera in NDC; pass 0.0 for an always-on-top line).
 *
 * Cheap enough for "spark" particle systems and debug visualizations —
 * each line is a few floats in a flat queue, drained at frame end. Beats
 * spawning a sphere mesh per spark.
 *
 * Coordinate space:
 *   - mop_overlay_push_line_2d: framebuffer pixels (top-left origin),
 *     pre-SSAA. Pass-through to the rasterizer.
 *   - mop_overlay_push_line_3d: world-space endpoints. Projected to screen
 *     each frame via the current view/projection matrices and then queued
 *     as a 2D line. Returns false if both endpoints are clipped behind the
 *     near plane (the line is silently dropped); true on success.
 *
 * width is in framebuffer pixels (1.0 = 1px line). depth is in the same
 * normalized space the renderer uses; 0.0 means "always on top."
 * ------------------------------------------------------------------------- */

void mop_overlay_push_line_2d(MopViewport *vp, float x0, float y0, float x1,
                              float y1, MopColor color, float width,
                              float depth);

bool mop_overlay_push_line_3d(MopViewport *vp, MopVec3 world_a, MopVec3 world_b,
                              MopColor color, float width);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_OVERLAY_H */
