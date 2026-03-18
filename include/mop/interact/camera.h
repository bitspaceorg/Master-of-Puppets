/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * camera.h — Orbit camera for interactive viewports
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_INTERACT_CAMERA_H
#define MOP_INTERACT_CAMERA_H

#include <mop/types.h>

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Camera projection mode
 * ------------------------------------------------------------------------- */

typedef enum MopCameraMode {
  MOP_CAMERA_PERSPECTIVE = 0,
  MOP_CAMERA_ORTHOGRAPHIC = 1,
} MopCameraMode;

/* -------------------------------------------------------------------------
 * Orbit camera
 *
 * A spherical camera that orbits around a target point.  Standard controls
 * for 3D viewport interaction: orbit, pan, zoom, and WASD-style movement.
 * ------------------------------------------------------------------------- */

typedef struct MopOrbitCamera {
  MopVec3 target;    /* look-at point */
  float distance;    /* distance from target */
  float yaw;         /* horizontal angle (radians) */
  float pitch;       /* vertical angle (radians), clamped to ±max_pitch */
  float fov_degrees; /* vertical field of view */
  float near_plane;
  float far_plane;
  float max_pitch; /* pitch clamp (default π — full vertical orbit) */

  float target_distance; /* smooth zoom target (interpolated by tick) */

  MopCameraMode mode; /* perspective or orthographic */
  float ortho_size;   /* ortho half-height in world units (default 5.0) */
} MopOrbitCamera;

/* Return sensible defaults: distance 4.5, yaw 0.6, pitch 0.4, fov 60,
 * near 0.1, far 100, target at origin. */
MopOrbitCamera mop_orbit_camera_default(void);

/* Compute the eye position from the current orbit state. */
MopVec3 mop_orbit_camera_eye(const MopOrbitCamera *cam);

/* Push the camera state into the viewport (calls mop_viewport_set_camera). */
void mop_orbit_camera_apply(const MopOrbitCamera *cam, MopViewport *vp);

/* Orbit: rotate around the target.  dx/dy are mouse deltas in pixels.
 * Typical sensitivity: 0.005 per pixel. */
void mop_orbit_camera_orbit(MopOrbitCamera *cam, float dx, float dy,
                            float sensitivity);

/* Pan: translate the target in the camera's local right/up plane.
 * dx/dy are mouse deltas in pixels.  Scales with distance automatically. */
void mop_orbit_camera_pan(MopOrbitCamera *cam, float dx, float dy);

/* Zoom: adjust distance.  delta is scroll amount (positive = zoom in). */
void mop_orbit_camera_zoom(MopOrbitCamera *cam, float delta);

/* Move the target on the world XZ plane along the camera's facing direction.
 * forward/right are signed amounts (e.g. dt * speed). */
void mop_orbit_camera_move(MopOrbitCamera *cam, float forward, float right);

/* Tick inertia: call once per frame with dt in seconds.  Applies velocity
 * to yaw/pitch/distance/target and decays it exponentially.  Returns true
 * if the camera is still moving (needs another frame). */
bool mop_orbit_camera_tick(MopOrbitCamera *cam, float dt);

/* Axis view identifiers for snap-to-view */
typedef enum MopViewAxis {
  MOP_VIEW_FRONT,  /* -Z → +Z (looking along -Z) */
  MOP_VIEW_BACK,   /* +Z → -Z */
  MOP_VIEW_RIGHT,  /* +X → -X */
  MOP_VIEW_LEFT,   /* -X → +X */
  MOP_VIEW_TOP,    /* +Y → -Y */
  MOP_VIEW_BOTTOM, /* -Y → +Y */
} MopViewAxis;

/* Snap the orbit camera to an axis-aligned view. Preserves target and
 * distance. */
void mop_orbit_camera_snap_to_view(MopOrbitCamera *cam, MopViewAxis view);

#endif /* MOP_INTERACT_CAMERA_H */
