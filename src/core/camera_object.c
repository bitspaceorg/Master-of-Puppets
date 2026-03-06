/*
 * Master of Puppets — Camera Object Implementation
 * camera_object.c — Camera as a manipulable scene object (Phase 5)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include <math.h>
#include <mop/mop.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define CAM_ID_BASE 0xFFFE0000u
#define CAM_ICON_SIZE 0.02f /* tiny — picking handled via screen-space */
#define CAM_PI 3.14159265358979323846f

/* -------------------------------------------------------------------------
 * Forward declarations for internal helpers
 * ------------------------------------------------------------------------- */

static void regenerate_icon(MopViewport *vp, MopCameraObject *cam,
                            uint32_t cam_index);

/* -------------------------------------------------------------------------
 * Add / remove cameras
 * ------------------------------------------------------------------------- */

MopCameraObject *mop_viewport_add_camera(MopViewport *vp,
                                         const MopCameraObjectDesc *desc) {
  if (!vp || !desc)
    return NULL;
  if (desc->object_id == 0)
    return NULL;
  if (vp->camera_count >= MOP_MAX_CAMERAS)
    return NULL;

  /* Find first inactive slot */
  MopCameraObject *cam = NULL;
  uint32_t slot = 0;
  for (uint32_t i = 0; i < MOP_MAX_CAMERAS; i++) {
    if (!vp->cameras[i].active) {
      cam = &vp->cameras[i];
      slot = i;
      break;
    }
  }
  if (!cam)
    return NULL;

  /* Populate fields */
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

  if (desc->name) {
    strncpy(cam->name, desc->name, sizeof(cam->name) - 1);
    cam->name[sizeof(cam->name) - 1] = '\0';
  } else {
    cam->name[0] = '\0';
  }

  cam->frustum_mesh = NULL;
  cam->icon_mesh = NULL;

  vp->camera_count++;

  /* Generate picking mesh (icon only — 2D overlay handles visuals) */
  regenerate_icon(vp, cam, slot);

  return cam;
}

void mop_viewport_remove_camera(MopViewport *vp, MopCameraObject *cam) {
  if (!vp || !cam || !cam->active)
    return;

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
}

/* -------------------------------------------------------------------------
 * Setters — update field, then regenerate frustum mesh
 * ------------------------------------------------------------------------- */

void mop_camera_object_set_position(MopCameraObject *cam, MopVec3 pos) {
  if (!cam || !cam->active)
    return;
  cam->position = pos;
}

void mop_camera_object_set_target(MopCameraObject *cam, MopVec3 target) {
  if (!cam || !cam->active)
    return;
  cam->target = target;
}

void mop_camera_object_set_up(MopCameraObject *cam, MopVec3 up) {
  if (!cam || !cam->active)
    return;
  cam->up = up;
}

void mop_camera_object_set_fov(MopCameraObject *cam, float fov_degrees) {
  if (!cam || !cam->active)
    return;
  cam->fov_degrees = fov_degrees;
}

void mop_camera_object_set_near(MopCameraObject *cam, float near_plane) {
  if (!cam || !cam->active)
    return;
  cam->near_plane = near_plane;
}

void mop_camera_object_set_far(MopCameraObject *cam, float far_plane) {
  if (!cam || !cam->active)
    return;
  cam->far_plane = far_plane;
}

void mop_camera_object_set_aspect(MopCameraObject *cam, float aspect_ratio) {
  if (!cam || !cam->active)
    return;
  cam->aspect_ratio = aspect_ratio;
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
  cam->frustum_visible = visible;
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
  vp->active_camera = cam;
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
  for (uint32_t i = 0; i < MOP_MAX_CAMERAS; i++) {
    if (vp->cameras[i].active) {
      if (found == index)
        return (MopCameraObject *)&vp->cameras[i];
      found++;
    }
  }
  return NULL;
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Camera icon mesh — tiny pick target at camera position
 * Frustum visuals are handled entirely by the 2D overlay.
 * ------------------------------------------------------------------------- */

static void regenerate_icon(MopViewport *vp, MopCameraObject *cam,
                            uint32_t cam_index) {
  if (!vp || !cam)
    return;

  /* Use the camera's object_id so clicking the icon selects it */
  uint32_t icon_id = cam->object_id;

  /* Tiny box in local space (centered at origin).
   * mesh->position places it at the camera world position.
   * Picking is handled via screen-space — this just provides the
   * gizmo target mesh and a few-pixel pick buffer entry. */
  float s = CAM_ICON_SIZE;
  MopColor color = vp->theme.camera_frustum_color;

  /* 8 corners of a cube centered at origin */
  MopVec3 box[8] = {{-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s},
                    {-s, -s, s},  {s, -s, s},  {s, s, s},  {-s, s, s}};

  /* 6 faces, 2 triangles each = 12 triangles = 36 indices */
  /* Each face has its own 4 vertices with proper normals = 24 total verts */
  static const int faces[6][4] = {
      {0, 1, 2, 3}, /* back  (-Z) */
      {4, 5, 6, 7}, /* front (+Z) */
      {0, 1, 5, 4}, /* bottom (-Y) */
      {2, 3, 7, 6}, /* top   (+Y) */
      {0, 3, 7, 4}, /* left  (-X) */
      {1, 2, 6, 5}  /* right (+X) */
  };
  static const MopVec3 normals[6] = {{0, 0, -1}, {0, 0, 1},  {0, -1, 0},
                                     {0, 1, 0},  {-1, 0, 0}, {1, 0, 0}};

  MopVertex verts[24];
  uint32_t indices[36];

  for (int f = 0; f < 6; f++) {
    int base = f * 4;
    for (int v = 0; v < 4; v++) {
      verts[base + v].position = box[faces[f][v]];
      verts[base + v].normal = normals[f];
      verts[base + v].color = color;
      verts[base + v].u = 0.0f;
      verts[base + v].v = 0.0f;
    }
    int ib = f * 6;
    indices[ib + 0] = (uint32_t)(base + 0);
    indices[ib + 1] = (uint32_t)(base + 1);
    indices[ib + 2] = (uint32_t)(base + 2);
    indices[ib + 3] = (uint32_t)(base + 0);
    indices[ib + 4] = (uint32_t)(base + 2);
    indices[ib + 5] = (uint32_t)(base + 3);
  }

  MopMeshDesc desc;
  memset(&desc, 0, sizeof(desc));
  desc.vertices = verts;
  desc.vertex_count = 24;
  desc.indices = indices;
  desc.index_count = 36;
  desc.object_id = icon_id;

  if (cam->icon_mesh) {
    mop_mesh_update_geometry(cam->icon_mesh, vp, verts, 24, indices, 36);
  } else {
    cam->icon_mesh = mop_viewport_add_mesh(vp, &desc);
  }

  /* Place the mesh at the camera's world position via TRS transform.
   * This ensures gizmo->target->position is correct for gizmo placement. */
  if (cam->icon_mesh)
    mop_mesh_set_position(cam->icon_mesh, cam->position);
}
