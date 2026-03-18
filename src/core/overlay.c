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
  for (uint32_t i = MOP_OVERLAY_BUILTIN_COUNT; i < vp->overlay_count; i++) {
    if (!vp->overlays[i].active) {
      vp->overlays[i].name = name;
      vp->overlays[i].draw_fn = draw_fn;
      vp->overlays[i].user_data = user_data;
      vp->overlays[i].active = true;
      vp->overlay_enabled[i] = true;
      return i;
    }
  }

  /* No free slot — grow if at capacity */
  if (vp->overlay_count >= vp->overlay_capacity) {
    uint32_t old_cap = vp->overlay_capacity;
    if (!mop_dyn_grow((void **)&vp->overlays, &vp->overlay_capacity,
                      sizeof(MopOverlayEntry), MOP_INITIAL_OVERLAY_CAPACITY))
      return UINT32_MAX;
    /* Grow the parallel overlay_enabled array to match */
    bool *new_enabled = realloc(vp->overlay_enabled,
                                (size_t)vp->overlay_capacity * sizeof(bool));
    if (!new_enabled)
      return UINT32_MAX;
    memset(new_enabled + old_cap, 0,
           (size_t)(vp->overlay_capacity - old_cap) * sizeof(bool));
    vp->overlay_enabled = new_enabled;
  }

  uint32_t slot = vp->overlay_count++;
  vp->overlays[slot].name = name;
  vp->overlays[slot].draw_fn = draw_fn;
  vp->overlays[slot].user_data = user_data;
  vp->overlays[slot].active = true;
  vp->overlay_enabled[slot] = true;
  return slot;
}

void mop_viewport_remove_overlay(MopViewport *vp, uint32_t handle) {
  if (!vp || handle < MOP_OVERLAY_BUILTIN_COUNT || handle >= vp->overlay_count)
    return;
  vp->overlays[handle].active = false;
  vp->overlays[handle].draw_fn = NULL;
  vp->overlay_enabled[handle] = false;
}

void mop_viewport_set_overlay_enabled(MopViewport *vp, uint32_t id,
                                      bool enabled) {
  if (!vp || id >= vp->overlay_count)
    return;
  vp->overlay_enabled[id] = enabled;
}

bool mop_viewport_get_overlay_enabled(const MopViewport *vp, uint32_t id) {
  if (!vp || id >= vp->overlay_count)
    return false;
  return vp->overlay_enabled[id];
}
