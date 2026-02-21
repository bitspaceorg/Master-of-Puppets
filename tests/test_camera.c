/*
 * Master of Puppets — Camera Tests
 * test_camera.c — Orbit camera defaults, eye position, orbit/pan/zoom bounds
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

static void test_defaults(void) {
  TEST_BEGIN("camera_defaults");
  MopOrbitCamera cam = mop_orbit_camera_default();
  TEST_ASSERT_FLOAT_EQ(cam.distance, 4.5f);
  TEST_ASSERT_FLOAT_EQ(cam.fov_degrees, 60.0f);
  TEST_ASSERT_FLOAT_EQ(cam.near_plane, 0.1f);
  TEST_ASSERT_FLOAT_EQ(cam.far_plane, 100.0f);
  TEST_ASSERT_FLOAT_EQ(cam.max_pitch, 1.5f);
  TEST_END();
}

static void test_eye_position(void) {
  TEST_BEGIN("camera_eye_position");
  MopOrbitCamera cam = mop_orbit_camera_default();
  cam.yaw = 0.0f;
  cam.pitch = 0.0f;
  cam.target = (MopVec3){0, 0, 0};
  cam.distance = 5.0f;
  MopVec3 eye = mop_orbit_camera_eye(&cam);
  /* yaw=0, pitch=0: eye at (0, 0, distance) */
  TEST_ASSERT_FLOAT_EQ(eye.x, 0.0f);
  TEST_ASSERT_FLOAT_EQ(eye.y, 0.0f);
  TEST_ASSERT_FLOAT_EQ(eye.z, 5.0f);
  TEST_END();
}

static void test_orbit(void) {
  TEST_BEGIN("camera_orbit");
  MopOrbitCamera cam = mop_orbit_camera_default();
  float old_yaw = cam.yaw;
  float old_pitch = cam.pitch;
  mop_orbit_camera_orbit(&cam, 100.0f, 0.0f, 0.005f);
  TEST_ASSERT(cam.yaw != old_yaw);
  TEST_ASSERT_FLOAT_EQ(cam.pitch, old_pitch);
  TEST_END();
}

static void test_pitch_clamp(void) {
  TEST_BEGIN("camera_pitch_clamp");
  MopOrbitCamera cam = mop_orbit_camera_default();
  /* Try to orbit past max_pitch */
  mop_orbit_camera_orbit(&cam, 0.0f, -100000.0f, 0.005f);
  TEST_ASSERT(cam.pitch <= cam.max_pitch);
  TEST_ASSERT(cam.pitch >= -cam.max_pitch);
  mop_orbit_camera_orbit(&cam, 0.0f, 100000.0f, 0.005f);
  TEST_ASSERT(cam.pitch <= cam.max_pitch);
  TEST_END();
}

static void test_zoom(void) {
  TEST_BEGIN("camera_zoom");
  MopOrbitCamera cam = mop_orbit_camera_default();
  float old_dist = cam.distance;
  mop_orbit_camera_zoom(&cam, 1.0f);
  TEST_ASSERT(cam.distance < old_dist);
  TEST_END();
}

static void test_zoom_clamp(void) {
  TEST_BEGIN("camera_zoom_clamp");
  MopOrbitCamera cam = mop_orbit_camera_default();
  /* Zoom in as much as possible */
  for (int i = 0; i < 1000; i++)
    mop_orbit_camera_zoom(&cam, 100.0f);
  TEST_ASSERT(cam.distance >= 0.5f);
  /* Zoom out as much as possible */
  for (int i = 0; i < 1000; i++)
    mop_orbit_camera_zoom(&cam, -100.0f);
  TEST_ASSERT(cam.distance <= 500.0f);
  TEST_END();
}

static void test_pan(void) {
  TEST_BEGIN("camera_pan");
  MopOrbitCamera cam = mop_orbit_camera_default();
  MopVec3 old_target = cam.target;
  mop_orbit_camera_pan(&cam, 10.0f, 10.0f);
  /* Target should have moved */
  TEST_ASSERT(cam.target.x != old_target.x || cam.target.y != old_target.y ||
              cam.target.z != old_target.z);
  TEST_END();
}

static void test_move(void) {
  TEST_BEGIN("camera_move_xz");
  MopOrbitCamera cam = mop_orbit_camera_default();
  cam.yaw = 0.0f;
  MopVec3 old_target = cam.target;
  mop_orbit_camera_move(&cam, 1.0f, 0.0f);
  /* With yaw=0, forward should translate Z negatively */
  TEST_ASSERT(cam.target.z != old_target.z);
  /* Y should not change (XZ plane movement) */
  TEST_ASSERT_FLOAT_EQ(cam.target.y, old_target.y);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("camera");

  TEST_RUN(test_defaults);
  TEST_RUN(test_eye_position);
  TEST_RUN(test_orbit);
  TEST_RUN(test_pitch_clamp);
  TEST_RUN(test_zoom);
  TEST_RUN(test_zoom_clamp);
  TEST_RUN(test_pan);
  TEST_RUN(test_move);

  TEST_REPORT();
  TEST_EXIT();
}
