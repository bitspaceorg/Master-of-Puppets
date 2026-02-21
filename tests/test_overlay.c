/*
 * Master of Puppets — Overlay System Tests
 * test_overlay.c
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

static MopViewport *make_viewport(void) {
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  return mop_viewport_create(&desc);
}

static int s_custom_overlay_called = 0;
static void custom_overlay_fn(MopViewport *vp, void *user_data) {
  (void)vp;
  (void)user_data;
  s_custom_overlay_called++;
}

static void test_overlay_defaults_disabled(void) {
  TEST_BEGIN("overlay_defaults_disabled");
  MopViewport *vp = make_viewport();
  TEST_ASSERT(vp != NULL);
  TEST_ASSERT(!mop_viewport_get_overlay_enabled(vp, MOP_OVERLAY_WIREFRAME));
  TEST_ASSERT(!mop_viewport_get_overlay_enabled(vp, MOP_OVERLAY_NORMALS));
  TEST_ASSERT(!mop_viewport_get_overlay_enabled(vp, MOP_OVERLAY_BOUNDS));
  TEST_ASSERT(!mop_viewport_get_overlay_enabled(vp, MOP_OVERLAY_SELECTION));
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_overlay_enable_disable(void) {
  TEST_BEGIN("overlay_enable_disable");
  MopViewport *vp = make_viewport();

  mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_WIREFRAME, true);
  TEST_ASSERT(mop_viewport_get_overlay_enabled(vp, MOP_OVERLAY_WIREFRAME));

  mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_WIREFRAME, false);
  TEST_ASSERT(!mop_viewport_get_overlay_enabled(vp, MOP_OVERLAY_WIREFRAME));

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_custom_overlay_registration(void) {
  TEST_BEGIN("custom_overlay_registration");
  MopViewport *vp = make_viewport();

  uint32_t handle =
      mop_viewport_add_overlay(vp, "test_overlay", custom_overlay_fn, NULL);
  TEST_ASSERT(handle != UINT32_MAX);
  TEST_ASSERT(handle >= MOP_OVERLAY_BUILTIN_COUNT);
  TEST_ASSERT(mop_viewport_get_overlay_enabled(vp, handle));

  mop_viewport_remove_overlay(vp, handle);
  TEST_ASSERT(!mop_viewport_get_overlay_enabled(vp, handle));

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_custom_overlay_invoked(void) {
  TEST_BEGIN("custom_overlay_invoked");
  MopViewport *vp = make_viewport();

  s_custom_overlay_called = 0;
  uint32_t handle =
      mop_viewport_add_overlay(vp, "counter", custom_overlay_fn, NULL);
  TEST_ASSERT(handle != UINT32_MAX);

  /* Render a frame — the custom overlay should be called */
  mop_viewport_render(vp);
  TEST_ASSERT(s_custom_overlay_called == 1);

  /* Render again */
  mop_viewport_render(vp);
  TEST_ASSERT(s_custom_overlay_called == 2);

  /* Disable it */
  mop_viewport_set_overlay_enabled(vp, handle, false);
  mop_viewport_render(vp);
  TEST_ASSERT(s_custom_overlay_called == 2); /* not called */

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_wireframe_overlay_render(void) {
  TEST_BEGIN("wireframe_overlay_render");
  MopViewport *vp = make_viewport();

  /* Add a mesh */
  MopVertex verts[3] = {
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{-1, -1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, -1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  MopMesh *m = mop_viewport_add_mesh(vp, &desc);
  TEST_ASSERT(m != NULL);

  /* Enable wireframe overlay */
  MopDisplaySettings ds = mop_display_settings_default();
  ds.wireframe_overlay = true;
  mop_viewport_set_display(vp, &ds);
  mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_WIREFRAME, true);

  /* Render should succeed without crash */
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_overlay_out_of_bounds(void) {
  TEST_BEGIN("overlay_out_of_bounds");
  MopViewport *vp = make_viewport();

  /* Out-of-bounds access should not crash */
  TEST_ASSERT(!mop_viewport_get_overlay_enabled(vp, 999));
  mop_viewport_set_overlay_enabled(vp, 999, true);
  TEST_ASSERT(!mop_viewport_get_overlay_enabled(vp, 999));

  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("overlay");

  TEST_RUN(test_overlay_defaults_disabled);
  TEST_RUN(test_overlay_enable_disable);
  TEST_RUN(test_custom_overlay_registration);
  TEST_RUN(test_custom_overlay_invoked);
  TEST_RUN(test_wireframe_overlay_render);
  TEST_RUN(test_overlay_out_of_bounds);

  TEST_REPORT();
  TEST_EXIT();
}
