/*
 * Master of Puppets — Orthographic Camera Tests
 * test_ortho_camera.c — Phase 7A: Ortho/perspective mode switching
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

static MopViewport *make_viewport(void) {
  MopViewportDesc desc = {
      .width = 128, .height = 128, .backend = MOP_BACKEND_CPU};
  return mop_viewport_create(&desc);
}

/* ---- Tests ---- */

static void test_default_camera_mode(void) {
  TEST_BEGIN("default_camera_mode");
  MopViewport *vp = make_viewport();
  TEST_ASSERT(vp != NULL);

  /* Default should be perspective */
  MopCameraMode mode = mop_viewport_get_camera_mode(vp);
  TEST_ASSERT(mode == MOP_CAMERA_PERSPECTIVE);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_set_orthographic(void) {
  TEST_BEGIN("set_orthographic");
  MopViewport *vp = make_viewport();

  mop_viewport_set_camera_mode(vp, MOP_CAMERA_ORTHOGRAPHIC);
  TEST_ASSERT(mop_viewport_get_camera_mode(vp) == MOP_CAMERA_ORTHOGRAPHIC);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_switch_back_to_perspective(void) {
  TEST_BEGIN("switch_back_to_perspective");
  MopViewport *vp = make_viewport();

  mop_viewport_set_camera_mode(vp, MOP_CAMERA_ORTHOGRAPHIC);
  TEST_ASSERT(mop_viewport_get_camera_mode(vp) == MOP_CAMERA_ORTHOGRAPHIC);

  mop_viewport_set_camera_mode(vp, MOP_CAMERA_PERSPECTIVE);
  TEST_ASSERT(mop_viewport_get_camera_mode(vp) == MOP_CAMERA_PERSPECTIVE);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_ortho_render(void) {
  TEST_BEGIN("ortho_render");
  MopViewport *vp = make_viewport();

  /* Add a mesh */
  MopVertex verts[3] = {
      {{0, 0.5f, 0}, {0, 0, 1}, {1, 0, 0, 1}, 0, 0},
      {{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1, 0, 1}, 0, 0},
      {{0.5f, -0.5f, 0}, {0, 0, 1}, {0, 0, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  mop_viewport_add_mesh(vp, &desc);

  /* Switch to ortho and render — should not crash */
  mop_viewport_set_camera_mode(vp, MOP_CAMERA_ORTHOGRAPHIC);
  MopRenderResult r = mop_viewport_render(vp);
  TEST_ASSERT(r == MOP_RENDER_OK);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_ortho_perspective_render_both(void) {
  TEST_BEGIN("ortho_perspective_render_both");
  MopViewport *vp = make_viewport();

  MopVertex verts[3] = {
      {{0, 0.5f, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{-0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  mop_viewport_add_mesh(vp, &desc);

  /* Render in perspective */
  mop_viewport_set_camera_mode(vp, MOP_CAMERA_PERSPECTIVE);
  MopRenderResult r1 = mop_viewport_render(vp);
  TEST_ASSERT(r1 == MOP_RENDER_OK);

  /* Switch to ortho and render */
  mop_viewport_set_camera_mode(vp, MOP_CAMERA_ORTHOGRAPHIC);
  MopRenderResult r2 = mop_viewport_render(vp);
  TEST_ASSERT(r2 == MOP_RENDER_OK);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_ortho_null_safety(void) {
  TEST_BEGIN("ortho_null_safety");
  /* Should not crash on NULL viewport */
  mop_viewport_set_camera_mode(NULL, MOP_CAMERA_ORTHOGRAPHIC);
  MopCameraMode mode = mop_viewport_get_camera_mode(NULL);
  /* Implementation-defined return for NULL; just don't crash */
  (void)mode;
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("ortho_camera");

  TEST_RUN(test_default_camera_mode);
  TEST_RUN(test_set_orthographic);
  TEST_RUN(test_switch_back_to_perspective);
  TEST_RUN(test_ortho_render);
  TEST_RUN(test_ortho_perspective_render_both);
  TEST_RUN(test_ortho_null_safety);

  TEST_REPORT();
  TEST_EXIT();
}
