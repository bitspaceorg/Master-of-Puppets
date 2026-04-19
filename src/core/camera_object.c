/*
 * Master of Puppets — Camera Object Implementation
 * camera_object.c — Camera as a manipulable scene object (Phase 5)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include <math.h>
#include <mop/mop.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Add / remove cameras
 * ------------------------------------------------------------------------- */

MopCameraObject *mop_viewport_add_camera(MopViewport *vp,
                                         const MopCameraObjectDesc *desc) {
  if (!vp || !desc)
    return NULL;
  if (desc->object_id == 0)
    return NULL;

  MOP_VP_LOCK(vp);
  /* Find first inactive slot */
  for (uint32_t i = 0; i < vp->camera_count; i++) {
    if (!vp->cameras[i].active) {
      MopCameraObject *cam = &vp->cameras[i];
      memset(cam, 0, sizeof(*cam));
      cam->position = desc->position;
      cam->target = desc->target;
      cam->up = desc->up;
      cam->fov_degrees = desc->fov_degrees;
      cam->near_plane = desc->near_plane;
      cam->far_plane = desc->far_plane;
      cam->aspect_ratio = desc->aspect_ratio;
      cam->object_id = desc->object_id;
      cam->active = true;
      cam->frustum_visible = true;
      cam->viewport = vp;
      if (desc->name) {
        strncpy(cam->name, desc->name, sizeof(cam->name) - 1);
        cam->name[sizeof(cam->name) - 1] = '\0';
      }
      MOP_VP_UNLOCK(vp);
      return cam;
    }
  }

  /* No inactive slot — grow if at capacity */
  if (vp->camera_count >= vp->camera_capacity) {
    if (!mop_dyn_grow((void **)&vp->cameras, &vp->camera_capacity,
                      sizeof(struct MopCameraObject),
                      MOP_INITIAL_CAMERA_CAPACITY)) {
      MOP_VP_UNLOCK(vp);
      return NULL;
    }
    /* Reset active_camera pointer — realloc may have moved the array */
    vp->active_camera = NULL;
  }

  MopCameraObject *cam = &vp->cameras[vp->camera_count++];
  memset(cam, 0, sizeof(*cam));
  cam->position = desc->position;
  cam->target = desc->target;
  cam->up = desc->up;
  cam->fov_degrees = desc->fov_degrees;
  cam->near_plane = desc->near_plane;
  cam->far_plane = desc->far_plane;
  cam->aspect_ratio = desc->aspect_ratio;
  cam->object_id = desc->object_id;
  cam->active = true;
  cam->frustum_visible = true;
  cam->viewport = vp;

  if (desc->name) {
    strncpy(cam->name, desc->name, sizeof(cam->name) - 1);
    cam->name[sizeof(cam->name) - 1] = '\0';
  } else {
    cam->name[0] = '\0';
  }

  cam->frustum_mesh = NULL;
  cam->icon_mesh = NULL;

  MOP_VP_UNLOCK(vp);
  return cam;
}

void mop_viewport_remove_camera(MopViewport *vp, MopCameraObject *cam) {
  if (!vp || !cam || !cam->active)
    return;

  MOP_VP_LOCK(vp);
  /* If this was the active camera, reset to orbit */
  if (vp->active_camera == cam)
    vp->active_camera = NULL;

  /* Remove visual meshes from viewport */
  if (cam->frustum_mesh) {
    mop_viewport_remove_mesh(vp, cam->frustum_mesh);
    cam->frustum_mesh = NULL;
  }
  if (cam->icon_mesh) {
    mop_viewport_remove_mesh(vp, cam->icon_mesh);
    cam->icon_mesh = NULL;
  }

  cam->active = false;
  if (vp->camera_count > 0)
    vp->camera_count--;
  MOP_VP_UNLOCK(vp);
}

/* -------------------------------------------------------------------------
 * Setters — update field, then regenerate frustum mesh
 * ------------------------------------------------------------------------- */

void mop_camera_object_set_position(MopCameraObject *cam, MopVec3 pos) {
  if (!cam || !cam->active)
    return;
  MOP_VP_LOCK(cam->viewport);
  cam->position = pos;
  MOP_VP_UNLOCK(cam->viewport);
}

void mop_camera_object_set_target(MopCameraObject *cam, MopVec3 target) {
  if (!cam || !cam->active)
    return;
  MOP_VP_LOCK(cam->viewport);
  cam->target = target;
  MOP_VP_UNLOCK(cam->viewport);
}

void mop_camera_object_set_up(MopCameraObject *cam, MopVec3 up) {
  if (!cam || !cam->active)
    return;
  MOP_VP_LOCK(cam->viewport);
  cam->up = up;
  MOP_VP_UNLOCK(cam->viewport);
}

void mop_camera_object_set_fov(MopCameraObject *cam, float fov_degrees) {
  if (!cam || !cam->active)
    return;
  MOP_VP_LOCK(cam->viewport);
  cam->fov_degrees = fov_degrees;
  MOP_VP_UNLOCK(cam->viewport);
}

void mop_camera_object_set_near(MopCameraObject *cam, float near_plane) {
  if (!cam || !cam->active)
    return;
  MOP_VP_LOCK(cam->viewport);
  cam->near_plane = near_plane;
  MOP_VP_UNLOCK(cam->viewport);
}

void mop_camera_object_set_far(MopCameraObject *cam, float far_plane) {
  if (!cam || !cam->active)
    return;
  MOP_VP_LOCK(cam->viewport);
  cam->far_plane = far_plane;
  MOP_VP_UNLOCK(cam->viewport);
}

void mop_camera_object_set_aspect(MopCameraObject *cam, float aspect_ratio) {
  if (!cam || !cam->active)
    return;
  MOP_VP_LOCK(cam->viewport);
  cam->aspect_ratio = aspect_ratio;
  MOP_VP_UNLOCK(cam->viewport);
}

/* -------------------------------------------------------------------------
 * Getters
 * ------------------------------------------------------------------------- */

MopVec3 mop_camera_object_get_position(const MopCameraObject *cam) {
  if (!cam)
    return (MopVec3){0, 0, 0};
  return cam->position;
}

MopVec3 mop_camera_object_get_target(const MopCameraObject *cam) {
  if (!cam)
    return (MopVec3){0, 0, 0};
  return cam->target;
}

float mop_camera_object_get_fov(const MopCameraObject *cam) {
  if (!cam)
    return 0.0f;
  return cam->fov_degrees;
}

uint32_t mop_camera_object_get_id(const MopCameraObject *cam) {
  if (!cam)
    return 0;
  return cam->object_id;
}

const char *mop_camera_object_get_name(const MopCameraObject *cam) {
  if (!cam)
    return "";
  return cam->name;
}

/* -------------------------------------------------------------------------
 * Frustum visibility
 * ------------------------------------------------------------------------- */

void mop_camera_object_set_frustum_visible(MopCameraObject *cam, bool visible) {
  if (!cam || !cam->active)
    return;
  MOP_VP_LOCK(cam->viewport);
  cam->frustum_visible = visible;
  MOP_VP_UNLOCK(cam->viewport);
}

bool mop_camera_object_get_frustum_visible(const MopCameraObject *cam) {
  if (!cam)
    return false;
  return cam->frustum_visible;
}

/* -------------------------------------------------------------------------
 * Active camera
 * ------------------------------------------------------------------------- */

void mop_viewport_set_active_camera(MopViewport *vp, MopCameraObject *cam) {
  if (!vp)
    return;
  MOP_VP_LOCK(vp);
  vp->active_camera = cam;
  MOP_VP_UNLOCK(vp);
}

MopCameraObject *mop_viewport_get_active_camera(const MopViewport *vp) {
  if (!vp)
    return NULL;
  return vp->active_camera;
}

/* -------------------------------------------------------------------------
 * Enumeration
 * ------------------------------------------------------------------------- */

uint32_t mop_viewport_get_camera_count(const MopViewport *vp) {
  if (!vp)
    return 0;
  return vp->camera_count;
}

MopCameraObject *mop_viewport_get_camera(const MopViewport *vp,
                                         uint32_t index) {
  if (!vp)
    return NULL;

  /* Walk active slots */
  uint32_t found = 0;
  for (uint32_t i = 0; i < vp->camera_count; i++) {
    if (vp->cameras[i].active) {
      if (found == index)
        return (MopCameraObject *)&vp->cameras[i];
      found++;
    }
  }
  return NULL;
}
