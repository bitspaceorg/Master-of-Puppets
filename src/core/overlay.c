/*
 * Master of Puppets — Overlay System
 * overlay.c — Registration, enable/disable, dispatch
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include <stdint.h>
#include <string.h>

uint32_t mop_viewport_add_overlay(MopViewport *vp, const char *name,
                                  MopOverlayFn draw_fn, void *user_data) {
  if (!vp || !draw_fn)
    return UINT32_MAX;

  /* Find a free slot after the built-in range */
  for (uint32_t i = MOP_OVERLAY_BUILTIN_COUNT; i < MOP_MAX_OVERLAYS; i++) {
    if (!vp->overlays[i].active) {
      vp->overlays[i].name = name;
      vp->overlays[i].draw_fn = draw_fn;
      vp->overlays[i].user_data = user_data;
      vp->overlays[i].active = true;
      vp->overlay_enabled[i] = true;
      if (vp->overlay_count <= i) {
        vp->overlay_count = i + 1;
      }
      return i;
    }
  }
  return UINT32_MAX; /* full */
}

void mop_viewport_remove_overlay(MopViewport *vp, uint32_t handle) {
  if (!vp || handle < MOP_OVERLAY_BUILTIN_COUNT || handle >= MOP_MAX_OVERLAYS)
    return;
  vp->overlays[handle].active = false;
  vp->overlays[handle].draw_fn = NULL;
  vp->overlay_enabled[handle] = false;
}

void mop_viewport_set_overlay_enabled(MopViewport *vp, uint32_t id,
                                      bool enabled) {
  if (!vp || id >= MOP_MAX_OVERLAYS)
    return;
  vp->overlay_enabled[id] = enabled;
}

bool mop_viewport_get_overlay_enabled(const MopViewport *vp, uint32_t id) {
  if (!vp || id >= MOP_MAX_OVERLAYS)
    return false;
  return vp->overlay_enabled[id];
}
