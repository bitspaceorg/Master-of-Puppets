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

#ifndef MOP_OVERLAY_H
#define MOP_OVERLAY_H

#include "types.h"

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
  MOP_OVERLAY_BUILTIN_COUNT = 4,
} MopOverlayId;

/* -------------------------------------------------------------------------
 * Custom overlay callback
 * ------------------------------------------------------------------------- */

typedef void (*MopOverlayFn)(MopViewport *vp, void *user_data);

/* -------------------------------------------------------------------------
 * Overlay entry — internal storage for both built-in and custom overlays
 * ------------------------------------------------------------------------- */

#define MOP_MAX_OVERLAYS 16

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

#endif /* MOP_OVERLAY_H */
