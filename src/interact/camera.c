/*
 * Master of Puppets — Orbit Camera
 * camera.c — Spherical orbit camera implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <mop/core/viewport.h>
#include <mop/interact/camera.h>
#include <stdbool.h>

MopOrbitCamera mop_orbit_camera_default(void) {
  return (MopOrbitCamera){
      .target = {0.0f, 0.4f, 0.0f},
      .distance = 4.5f,
      .yaw = 0.6f,
      .pitch = 0.4f,
      .fov_degrees = 60.0f,
      .near_plane = 0.1f,
      .far_plane = 100.0f,
      .max_pitch = 3.14159265f,
      .target_distance = 4.5f,
  };
}

MopVec3 mop_orbit_camera_eye(const MopOrbitCamera *cam) {
  float cp = cosf(cam->pitch);
  return (MopVec3){
      cam->target.x + cam->distance * cp * sinf(cam->yaw),
      cam->target.y + cam->distance * sinf(cam->pitch),
      cam->target.z + cam->distance * cp * cosf(cam->yaw),
  };
}

void mop_orbit_camera_apply(const MopOrbitCamera *cam, MopViewport *vp) {
  /* Derive up vector from orbit angles (pitch tangent on the sphere).
   * This is always perpendicular to the view direction, avoiding gimbal
   * lock at ±90° pitch where a fixed (0,1,0) up would fail. */
  float sp = sinf(cam->pitch);
  float cp = cosf(cam->pitch);
  float sy = sinf(cam->yaw);
  float cy = cosf(cam->yaw);
  MopVec3 up = {-sp * sy, cp, -sp * cy};
  mop_viewport_set_camera_orbit(vp, mop_orbit_camera_eye(cam), cam->target, up,
                                cam->fov_degrees, cam->near_plane,
                                cam->far_plane);
}

void mop_orbit_camera_orbit(MopOrbitCamera *cam, float dx, float dy,
                            float sensitivity) {
  cam->yaw -= dx * sensitivity;
  cam->pitch += dy * sensitivity;
  /* Wrap pitch to [-π, π] — free 360° orbit like Blender */
  while (cam->pitch > (float)M_PI)
    cam->pitch -= 2.0f * (float)M_PI;
  while (cam->pitch < -(float)M_PI)
    cam->pitch += 2.0f * (float)M_PI;
}

void mop_orbit_camera_pan(MopOrbitCamera *cam, float dx, float dy) {
  float s = cam->distance * 0.003f;
  float cos_yaw = cosf(cam->yaw);
  float sin_yaw = sinf(cam->yaw);
  cam->target.x -= cos_yaw * dx * s;
  cam->target.z += sin_yaw * dx * s;
  cam->target.y += dy * s;
}

void mop_orbit_camera_zoom(MopOrbitCamera *cam, float delta) {
  cam->distance -= delta * 0.3f;
  if (cam->distance < 0.5f)
    cam->distance = 0.5f;
  if (cam->distance > 500.0f)
    cam->distance = 500.0f;
  cam->target_distance = cam->distance;
}

void mop_orbit_camera_move(MopOrbitCamera *cam, float forward, float right) {
  float fwd_x = -sinf(cam->yaw), fwd_z = -cosf(cam->yaw);
  float rgt_x = cosf(cam->yaw), rgt_z = -sinf(cam->yaw);
  cam->target.x += fwd_x * forward + rgt_x * right;
  cam->target.z += fwd_z * forward + rgt_z * right;
}

bool mop_orbit_camera_tick(MopOrbitCamera *cam, float dt) {
  if (dt <= 0.0f)
    return false;

  /* Smooth zoom — lerp distance toward target */
  float zoom_diff = cam->target_distance - cam->distance;
  if (fabsf(zoom_diff) > 0.001f) {
    float zoom_speed = 1.0f - expf(-16.0f * dt);
    cam->distance += zoom_diff * zoom_speed;
    return true;
  }
  cam->distance = cam->target_distance;
  return false;
}

void mop_orbit_camera_snap_to_view(MopOrbitCamera *cam, MopViewAxis view) {
  if (!cam)
    return;
  /* Spherical coords: eye = target + d*(cos(p)*sin(y), sin(p), cos(p)*cos(y))
   * Front(-Z→+Z): eye at -Z → yaw=π
   * Back(+Z→-Z):  eye at +Z → yaw=0
   * Right(+X→-X): eye at +X → yaw=π/2
   * Left(-X→+X):  eye at -X → yaw=-π/2
   * Top:   pitch=π/2   Bottom: pitch=-π/2 */
  static const float views[][2] = {
      /* {yaw, pitch} */
      [MOP_VIEW_FRONT] = {3.14159265f, 0.0f},
      [MOP_VIEW_BACK] = {0.0f, 0.0f},
      [MOP_VIEW_RIGHT] = {1.57079633f, 0.0f},
      [MOP_VIEW_LEFT] = {-1.57079633f, 0.0f},
      [MOP_VIEW_TOP] = {0.0f, 1.57079633f},
      [MOP_VIEW_BOTTOM] = {0.0f, -1.57079633f},
  };
  cam->yaw = views[view][0];
  cam->pitch = views[view][1];
}
