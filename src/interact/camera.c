/*
 * Master of Puppets — Orbit Camera
 * camera.c — Spherical orbit camera implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <mop/camera.h>
#include <mop/viewport.h>

MopOrbitCamera mop_orbit_camera_default(void) {
  return (MopOrbitCamera){
      .target = {0.0f, 0.4f, 0.0f},
      .distance = 4.5f,
      .yaw = 0.6f,
      .pitch = 0.4f,
      .fov_degrees = 60.0f,
      .near_plane = 0.1f,
      .far_plane = 100.0f,
      .max_pitch = 1.5f,
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
  mop_viewport_set_camera(vp, mop_orbit_camera_eye(cam), cam->target,
                          (MopVec3){0, 1, 0}, cam->fov_degrees, cam->near_plane,
                          cam->far_plane);
}

void mop_orbit_camera_orbit(MopOrbitCamera *cam, float dx, float dy,
                            float sensitivity) {
  cam->yaw -= dx * sensitivity;
  cam->pitch += dy * sensitivity;
  if (cam->pitch > cam->max_pitch)
    cam->pitch = cam->max_pitch;
  if (cam->pitch < -cam->max_pitch)
    cam->pitch = -cam->max_pitch;
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
}

void mop_orbit_camera_move(MopOrbitCamera *cam, float forward, float right) {
  float fwd_x = -sinf(cam->yaw), fwd_z = -cosf(cam->yaw);
  float rgt_x = cosf(cam->yaw), rgt_z = -sinf(cam->yaw);
  cam->target.x += fwd_x * forward + rgt_x * right;
  cam->target.z += fwd_z * forward + rgt_z * right;
}
