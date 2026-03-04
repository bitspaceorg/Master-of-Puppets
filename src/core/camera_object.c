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
#define CAM_ICON_SIZE 0.08f
#define CAM_PI 3.14159265358979323846f

/* -------------------------------------------------------------------------
 * Forward declarations for internal helpers
 * ------------------------------------------------------------------------- */

static void regenerate_frustum(MopViewport *vp, MopCameraObject *cam);
static void regenerate_icon(MopViewport *vp, MopCameraObject *cam,
                            uint32_t cam_index);
static uint32_t cam_slot_index(const MopViewport *vp,
                               const MopCameraObject *cam);

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

  /* Generate visual meshes */
  regenerate_frustum(vp, cam);
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
  if (cam->frustum_mesh)
    mop_mesh_set_opacity(cam->frustum_mesh, visible ? 1.0f : 0.0f);
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

  /* Restore frustum visibility on previously active camera */
  if (vp->active_camera && vp->active_camera->frustum_mesh) {
    if (vp->active_camera->frustum_visible)
      mop_mesh_set_opacity(vp->active_camera->frustum_mesh, 1.0f);
  }

  vp->active_camera = cam;

  /* Hide frustum of the active camera (don't see your own frustum) */
  if (cam && cam->frustum_mesh)
    mop_mesh_set_opacity(cam->frustum_mesh, 0.0f);
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

static uint32_t cam_slot_index(const MopViewport *vp,
                               const MopCameraObject *cam) {
  return (uint32_t)(cam - vp->cameras);
}

/* -------------------------------------------------------------------------
 * Frustum mesh generation
 *
 * Computes the 8 frustum corners in world space by inverting the
 * view-projection matrix, then creates 12 degenerate triangles
 * (one per frustum edge) for wireframe rendering.
 * ------------------------------------------------------------------------- */

static void regenerate_frustum(MopViewport *vp, MopCameraObject *cam) {
  if (!vp || !cam)
    return;

  uint32_t slot = cam_slot_index(vp, cam);
  uint32_t frustum_id = CAM_ID_BASE + slot * 2u;

  /* Build view and projection matrices from camera params */
  float fov_rad = cam->fov_degrees * (CAM_PI / 180.0f);
  MopMat4 view = mop_mat4_look_at(cam->position, cam->target, cam->up);
  MopMat4 proj = mop_mat4_perspective(fov_rad, cam->aspect_ratio,
                                      cam->near_plane, cam->far_plane);
  MopMat4 vp_mat = mop_mat4_multiply(proj, view);
  MopMat4 inv_vp = mop_mat4_inverse(vp_mat);

  /* NDC corners: 8 points of the [-1,1]^3 cube */
  static const float ndc[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1},
                                  {-1, 1, -1},  {-1, -1, 1}, {1, -1, 1},
                                  {1, 1, 1},    {-1, 1, 1}};

  MopVec3 corners[8];
  for (int i = 0; i < 8; i++) {
    MopVec4 clip = {ndc[i][0], ndc[i][1], ndc[i][2], 1.0f};
    MopVec4 world = mop_mat4_mul_vec4(inv_vp, clip);
    if (fabsf(world.w) > 1e-8f) {
      float inv_w = 1.0f / world.w;
      corners[i] = (MopVec3){world.x * inv_w, world.y * inv_w, world.z * inv_w};
    } else {
      corners[i] = (MopVec3){0, 0, 0};
    }
  }

  /* 12 frustum edges: 4 near, 4 far, 4 connecting near-to-far */
  static const int edges[12][2] = {/* Near face */
                                   {0, 1},
                                   {1, 2},
                                   {2, 3},
                                   {3, 0},
                                   /* Far face */
                                   {4, 5},
                                   {5, 6},
                                   {6, 7},
                                   {7, 4},
                                   /* Connecting edges */
                                   {0, 4},
                                   {1, 5},
                                   {2, 6},
                                   {3, 7}};

  /* Build mesh: 8 vertices, 12 degenerate triangles (v0, v1, v0) = 36 idx */
  MopColor color = vp->theme.camera_frustum_color;
  MopVec3 up_n = {0, 1, 0};

  MopVertex verts[8];
  for (int i = 0; i < 8; i++) {
    verts[i].position = corners[i];
    verts[i].normal = up_n;
    verts[i].color = color;
    verts[i].u = 0.0f;
    verts[i].v = 0.0f;
  }

  uint32_t indices[36];
  for (int i = 0; i < 12; i++) {
    int a = edges[i][0];
    int b = edges[i][1];
    indices[i * 3 + 0] = (uint32_t)a;
    indices[i * 3 + 1] = (uint32_t)b;
    indices[i * 3 + 2] = (uint32_t)a; /* degenerate */
  }

  MopMeshDesc desc;
  memset(&desc, 0, sizeof(desc));
  desc.vertices = verts;
  desc.vertex_count = 8;
  desc.indices = indices;
  desc.index_count = 36;
  desc.object_id = frustum_id;

  if (cam->frustum_mesh) {
    /* Update existing mesh geometry in-place */
    mop_mesh_update_geometry(cam->frustum_mesh, vp, verts, 8, indices, 36);
  } else {
    cam->frustum_mesh = mop_viewport_add_mesh(vp, &desc);
    if (cam->frustum_mesh) {
      mop_mesh_set_opacity(cam->frustum_mesh,
                           cam->frustum_visible ? 1.0f : 0.0f);
    }
  }

  /* If this camera is the active one, hide its frustum */
  if (vp->active_camera == cam && cam->frustum_mesh)
    mop_mesh_set_opacity(cam->frustum_mesh, 0.0f);
}

/* -------------------------------------------------------------------------
 * Camera icon mesh — small box at camera position
 * ------------------------------------------------------------------------- */

static void regenerate_icon(MopViewport *vp, MopCameraObject *cam,
                            uint32_t cam_index) {
  if (!vp || !cam)
    return;

  uint32_t icon_id = CAM_ID_BASE + cam_index * 2u + 1u;

  /* Small box centered at camera position */
  float s = CAM_ICON_SIZE;
  MopVec3 p = cam->position;
  MopColor color = vp->theme.camera_frustum_color;

  /* 8 corners of a cube */
  MopVec3 box[8] = {{p.x - s, p.y - s, p.z - s}, {p.x + s, p.y - s, p.z - s},
                    {p.x + s, p.y + s, p.z - s}, {p.x - s, p.y + s, p.z - s},
                    {p.x - s, p.y - s, p.z + s}, {p.x + s, p.y - s, p.z + s},
                    {p.x + s, p.y + s, p.z + s}, {p.x - s, p.y + s, p.z + s}};

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
}
