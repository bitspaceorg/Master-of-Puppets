/*
 * Master of Puppets — Camera Object Tests
 * test_camera_object.c — Tests for MOP camera object system
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

static void test_camera_add_remove(void) {
  TEST_BEGIN("camera_add_remove");
  MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU});
  TEST_ASSERT(vp != NULL);
  TEST_ASSERT(mop_viewport_get_camera_count(vp) == 0);

  MopCameraObject *cam =
      mop_viewport_add_camera(vp, &(MopCameraObjectDesc){.position = {5, 3, 5},
                                                         .target = {0, 0, 0},
                                                         .up = {0, 1, 0},
                                                         .fov_degrees = 60.0f,
                                                         .near_plane = 0.1f,
                                                         .far_plane = 100.0f,
                                                         .aspect_ratio = 1.0f,
                                                         .object_id = 100,
                                                         .name = "TestCam"});
  TEST_ASSERT(cam != NULL);
  TEST_ASSERT(mop_viewport_get_camera_count(vp) == 1);

  mop_viewport_remove_camera(vp, cam);
  TEST_ASSERT(mop_viewport_get_camera_count(vp) == 0);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_camera_getters(void) {
  TEST_BEGIN("camera_getters");
  MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU});
  TEST_ASSERT(vp != NULL);

  MopCameraObject *cam =
      mop_viewport_add_camera(vp, &(MopCameraObjectDesc){.position = {1, 2, 3},
                                                         .target = {0, 0, 0},
                                                         .up = {0, 1, 0},
                                                         .fov_degrees = 45.0f,
                                                         .near_plane = 0.5f,
                                                         .far_plane = 200.0f,
                                                         .aspect_ratio = 1.5f,
                                                         .object_id = 101,
                                                         .name = "MyCam"});
  TEST_ASSERT(cam != NULL);

  MopVec3 p = mop_camera_object_get_position(cam);
  TEST_ASSERT_FLOAT_EQ(p.x, 1.0f);
  TEST_ASSERT_FLOAT_EQ(p.y, 2.0f);

  TEST_ASSERT(mop_camera_object_get_id(cam) == 101);
  TEST_ASSERT_FLOAT_EQ(mop_camera_object_get_fov(cam), 45.0f);

  const char *name = mop_camera_object_get_name(cam);
  TEST_ASSERT(name != NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_camera_active_switch(void) {
  TEST_BEGIN("camera_active_switch");
  MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU});
  TEST_ASSERT(vp != NULL);

  TEST_ASSERT(mop_viewport_get_active_camera(vp) == NULL);

  MopCameraObject *cam =
      mop_viewport_add_camera(vp, &(MopCameraObjectDesc){.position = {3, 3, 3},
                                                         .target = {0, 0, 0},
                                                         .up = {0, 1, 0},
                                                         .fov_degrees = 60.0f,
                                                         .near_plane = 0.1f,
                                                         .far_plane = 100.0f,
                                                         .aspect_ratio = 1.0f,
                                                         .object_id = 200,
                                                         .name = "ActiveCam"});
  TEST_ASSERT(cam != NULL);

  mop_viewport_set_active_camera(vp, cam);
  TEST_ASSERT(mop_viewport_get_active_camera(vp) == cam);

  mop_viewport_set_active_camera(vp, NULL);
  TEST_ASSERT(mop_viewport_get_active_camera(vp) == NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_camera_null_safety(void) {
  TEST_BEGIN("camera_null_safety");
  mop_viewport_remove_camera(NULL, NULL);
  mop_viewport_set_active_camera(NULL, NULL);
  TEST_ASSERT(mop_viewport_get_active_camera(NULL) == NULL);
  TEST_ASSERT(mop_viewport_get_camera_count(NULL) == 0);
  TEST_ASSERT(mop_viewport_get_camera(NULL, 0) == NULL);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("camera_object");

  TEST_RUN(test_camera_add_remove);
  TEST_RUN(test_camera_getters);
  TEST_RUN(test_camera_active_switch);
  TEST_RUN(test_camera_null_safety);

  TEST_REPORT();
  TEST_EXIT();
}
