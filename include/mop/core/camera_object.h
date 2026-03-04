/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * camera_object.h — Camera as a manipulable scene object
 *
 * Camera objects are visual entities in the scene with position, target,
 * and frustum visualization.  They can be moved with gizmos like any
 * other object.  One camera can be set as "active" to drive the viewport.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_CAMERA_OBJECT_H
#define MOP_CORE_CAMERA_OBJECT_H

#include <mop/types.h>

typedef struct MopViewport MopViewport;
typedef struct MopCameraObject MopCameraObject;

typedef struct MopCameraObjectDesc {
  MopVec3 position;
  MopVec3 target;
  MopVec3 up;
  float fov_degrees;
  float near_plane;
  float far_plane;
  float aspect_ratio;
  uint32_t object_id; /* unique ID for picking, must be nonzero */
  const char *name;   /* display name, copied internally */
} MopCameraObjectDesc;

/* Add a camera to the scene.  Returns NULL on failure. */
MopCameraObject *mop_viewport_add_camera(MopViewport *vp,
                                         const MopCameraObjectDesc *desc);

/* Remove a camera from the scene. */
void mop_viewport_remove_camera(MopViewport *vp, MopCameraObject *cam);

/* Setters */
void mop_camera_object_set_position(MopCameraObject *cam, MopVec3 pos);
void mop_camera_object_set_target(MopCameraObject *cam, MopVec3 target);
void mop_camera_object_set_up(MopCameraObject *cam, MopVec3 up);
void mop_camera_object_set_fov(MopCameraObject *cam, float fov_degrees);
void mop_camera_object_set_near(MopCameraObject *cam, float near_plane);
void mop_camera_object_set_far(MopCameraObject *cam, float far_plane);
void mop_camera_object_set_aspect(MopCameraObject *cam, float aspect_ratio);

/* Getters */
MopVec3 mop_camera_object_get_position(const MopCameraObject *cam);
MopVec3 mop_camera_object_get_target(const MopCameraObject *cam);
float mop_camera_object_get_fov(const MopCameraObject *cam);
uint32_t mop_camera_object_get_id(const MopCameraObject *cam);
const char *mop_camera_object_get_name(const MopCameraObject *cam);

/* Set/get frustum wireframe visibility */
void mop_camera_object_set_frustum_visible(MopCameraObject *cam, bool visible);
bool mop_camera_object_get_frustum_visible(const MopCameraObject *cam);

/* Set the active camera — the viewport will render from this camera's POV.
 * Pass NULL to return to the default orbit camera. */
void mop_viewport_set_active_camera(MopViewport *vp, MopCameraObject *cam);

/* Get the active camera.  Returns NULL if using default orbit camera. */
MopCameraObject *mop_viewport_get_active_camera(const MopViewport *vp);

/* Get camera count */
uint32_t mop_viewport_get_camera_count(const MopViewport *vp);

/* Get camera by index (0-based) */
MopCameraObject *mop_viewport_get_camera(const MopViewport *vp, uint32_t index);

#endif /* MOP_CORE_CAMERA_OBJECT_H */
