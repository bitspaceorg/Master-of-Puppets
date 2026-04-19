/*
 * Master of Puppets — Light Management
 * light.c — Multi-light add/remove/update + visual indicators
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants for light indicators
 * ------------------------------------------------------------------------- */

#define MOP_LIGHT_ID_BASE 0xFFFE0000u
#define LI_PI 3.14159265358979323846f

/* Wireframe icon parameters — thin line icons like Maya/Blender/Houdini.
 * Each "line" is a cross-shaped pair of quads (8 verts, 4 tris) for
 * visibility from all angles — same technique as gizmo tube handles. */
#define LI_HW 0.012f /* half-width of line quads — clear 2D UI feel */
#define LI_SEGS 16   /* segments for circles/arcs */

MopLight *mop_viewport_add_light(MopViewport *vp, const MopLight *desc) {
  if (!vp || !desc)
    return NULL;

  MOP_VP_LOCK(vp);
  /* Find an inactive slot below the high-water mark */
  for (uint32_t i = 0; i < vp->light_count; i++) {
    if (!vp->lights[i].active) {
      vp->lights[i] = *desc;
      vp->lights[i].active = true;
      vp->lights[i].viewport = vp;
      MopLight *ret = &vp->lights[i];
      MOP_VP_UNLOCK(vp);
      return ret;
    }
  }

  /* No inactive slot — grow if at capacity */
  if (vp->light_count >= vp->light_capacity) {
    if (!mop_dyn_grow((void **)&vp->lights, &vp->light_capacity,
                      sizeof(MopLight), MOP_INITIAL_LIGHT_CAPACITY)) {
      MOP_VP_UNLOCK(vp);
      return NULL;
    }
    /* Grow light_indicators to match */
    MopMesh **new_ind = realloc(vp->light_indicators,
                                (size_t)vp->light_capacity * sizeof(MopMesh *));
    if (!new_ind) {
      MOP_VP_UNLOCK(vp);
      return NULL;
    }
    /* Zero the newly allocated portion */
    uint32_t old_ind_count = vp->light_count; /* was == old capacity */
    memset(new_ind + old_ind_count, 0,
           (size_t)(vp->light_capacity - old_ind_count) * sizeof(MopMesh *));
    vp->light_indicators = new_ind;
  }

  uint32_t idx = vp->light_count++;
  vp->lights[idx] = *desc;
  vp->lights[idx].active = true;
  vp->lights[idx].viewport = vp;
  MopLight *ret = &vp->lights[idx];
  MOP_VP_UNLOCK(vp);
  return ret;
}

void mop_viewport_remove_light(MopViewport *vp, MopLight *light) {
  if (!vp || !light)
    return;
  MOP_VP_LOCK(vp);
  light->active = false;
  MOP_VP_UNLOCK(vp);
}

void mop_viewport_clear_lights(MopViewport *vp) {
  if (!vp)
    return;
  MOP_VP_LOCK(vp);
  for (uint32_t i = 0; i < vp->light_count; i++) {
    vp->lights[i].active = false;
  }
  vp->light_count = 0;
  MOP_VP_UNLOCK(vp);
}

void mop_light_set_position(MopLight *l, MopVec3 pos) {
  if (!l)
    return;
  MOP_VP_LOCK(l->viewport);
  l->position = pos;
  MOP_VP_UNLOCK(l->viewport);
}

void mop_light_set_direction(MopLight *l, MopVec3 dir) {
  if (!l)
    return;
  MOP_VP_LOCK(l->viewport);
  l->direction = dir;
  MOP_VP_UNLOCK(l->viewport);
}

void mop_light_set_color(MopLight *l, MopColor color) {
  if (!l)
    return;
  MOP_VP_LOCK(l->viewport);
  l->color = color;
  MOP_VP_UNLOCK(l->viewport);
}

void mop_light_set_intensity(MopLight *l, float intensity) {
  if (!l)
    return;
  MOP_VP_LOCK(l->viewport);
  l->intensity = intensity;
  MOP_VP_UNLOCK(l->viewport);
}

uint32_t mop_viewport_light_count(const MopViewport *vp) {
  if (!vp)
    return 0;
  uint32_t count = 0;
  for (uint32_t i = 0; i < vp->light_count; i++) {
    if (vp->lights[i].active)
      count++;
  }
  return count;
}

/* =========================================================================
 * Light indicators — visual representations of lights in the viewport
 *
 * 3D light indicator meshes have been removed.  Visuals are now handled
 * entirely by the 2D SDF overlay (mop_overlay_builtin_light_indicators)
 * and picking uses screen-space distance (pick_light_screen).
 * ========================================================================= */

static void li_destroy(MopViewport *vp, uint32_t idx) {
  if (idx < vp->light_capacity && vp->light_indicators[idx]) {
    mop_viewport_remove_mesh(vp, vp->light_indicators[idx]);
    vp->light_indicators[idx] = NULL;
  }
}

/* -------------------------------------------------------------------------
 * Public (internal) API — called from viewport.c each frame
 * ------------------------------------------------------------------------- */

void mop_light_update_indicators(MopViewport *vp) {
  if (!vp)
    return;

  /* 3D light indicator meshes are no longer created — visuals come from
   * the 2D SDF overlay (mop_overlay_builtin_light_indicators) and picking
   * is handled by screen-space distance (pick_light_screen).  Clean up
   * any leftover meshes from previous code paths. */
  for (uint32_t i = 0; i < vp->light_count; i++) {
    if (vp->light_indicators[i]) {
      li_destroy(vp, i);
    }
  }
}

void mop_light_destroy_indicators(MopViewport *vp) {
  if (!vp)
    return;
  for (uint32_t i = 0; i < vp->light_count; i++) {
    li_destroy(vp, i);
  }
}
