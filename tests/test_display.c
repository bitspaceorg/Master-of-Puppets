/*
 * Master of Puppets — Display Settings Tests
 * test_display.c
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

static void test_default_settings(void) {
  TEST_BEGIN("display_default_settings");
  MopDisplaySettings ds = mop_display_settings_default();
  TEST_ASSERT(!ds.wireframe_overlay);
  TEST_ASSERT_FLOAT_EQ(ds.wireframe_color.r, 1.0f);
  TEST_ASSERT_FLOAT_EQ(ds.wireframe_color.g, 0.6f);
  TEST_ASSERT_FLOAT_EQ(ds.wireframe_color.b, 0.2f);
  TEST_ASSERT_FLOAT_EQ(ds.wireframe_opacity, 0.15f);
  TEST_ASSERT(!ds.show_normals);
  TEST_ASSERT_FLOAT_EQ(ds.normal_display_length, 0.1f);
  TEST_ASSERT(!ds.show_bounds);
  TEST_ASSERT(!ds.show_vertices);
  TEST_ASSERT_FLOAT_EQ(ds.vertex_display_size, 3.0f);
  TEST_ASSERT(ds.vertex_map_mode == MOP_VTXMAP_NONE);
  TEST_ASSERT(ds.vertex_map_channel == 0);
  TEST_END();
}

static void test_viewport_display_roundtrip(void) {
  TEST_BEGIN("display_viewport_roundtrip");
  MopViewport *vp = make_viewport();
  TEST_ASSERT(vp != NULL);

  MopDisplaySettings ds = mop_display_settings_default();
  ds.wireframe_overlay = true;
  ds.wireframe_opacity = 0.5f;
  ds.show_normals = true;
  ds.normal_display_length = 0.3f;
  ds.show_bounds = true;
  ds.vertex_map_mode = MOP_VTXMAP_UV;

  mop_viewport_set_display(vp, &ds);
  MopDisplaySettings got = mop_viewport_get_display(vp);

  TEST_ASSERT(got.wireframe_overlay == true);
  TEST_ASSERT_FLOAT_EQ(got.wireframe_opacity, 0.5f);
  TEST_ASSERT(got.show_normals == true);
  TEST_ASSERT_FLOAT_EQ(got.normal_display_length, 0.3f);
  TEST_ASSERT(got.show_bounds == true);
  TEST_ASSERT(got.vertex_map_mode == MOP_VTXMAP_UV);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_viewport_default_display(void) {
  TEST_BEGIN("display_viewport_default");
  MopViewport *vp = make_viewport();
  TEST_ASSERT(vp != NULL);

  /* Freshly created viewport should have default settings */
  MopDisplaySettings ds = mop_viewport_get_display(vp);
  TEST_ASSERT(!ds.wireframe_overlay);
  TEST_ASSERT(!ds.show_normals);
  TEST_ASSERT(!ds.show_bounds);
  TEST_ASSERT(!ds.show_vertices);
  TEST_ASSERT(ds.vertex_map_mode == MOP_VTXMAP_NONE);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_null_safety(void) {
  TEST_BEGIN("display_null_safety");
  /* set_display with NULL viewport should not crash */
  MopDisplaySettings ds = mop_display_settings_default();
  mop_viewport_set_display(NULL, &ds);

  /* get_display with NULL viewport should return defaults */
  MopDisplaySettings got = mop_viewport_get_display(NULL);
  TEST_ASSERT(!got.wireframe_overlay);
  TEST_ASSERT_FLOAT_EQ(got.wireframe_opacity, 0.15f);

  /* set_display with NULL settings should not crash */
  MopViewport *vp = make_viewport();
  mop_viewport_set_display(vp, NULL);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_vertex_map_modes(void) {
  TEST_BEGIN("display_vertex_map_modes");
  MopViewport *vp = make_viewport();

  MopDisplaySettings ds = mop_display_settings_default();

  /* Cycle through all modes */
  ds.vertex_map_mode = MOP_VTXMAP_UV;
  mop_viewport_set_display(vp, &ds);
  TEST_ASSERT(mop_viewport_get_display(vp).vertex_map_mode == MOP_VTXMAP_UV);

  ds.vertex_map_mode = MOP_VTXMAP_WEIGHTS;
  mop_viewport_set_display(vp, &ds);
  TEST_ASSERT(mop_viewport_get_display(vp).vertex_map_mode ==
              MOP_VTXMAP_WEIGHTS);

  ds.vertex_map_mode = MOP_VTXMAP_NORMALS;
  mop_viewport_set_display(vp, &ds);
  TEST_ASSERT(mop_viewport_get_display(vp).vertex_map_mode ==
              MOP_VTXMAP_NORMALS);

  ds.vertex_map_mode = MOP_VTXMAP_CUSTOM;
  ds.vertex_map_channel = 2;
  mop_viewport_set_display(vp, &ds);
  MopDisplaySettings got = mop_viewport_get_display(vp);
  TEST_ASSERT(got.vertex_map_mode == MOP_VTXMAP_CUSTOM);
  TEST_ASSERT(got.vertex_map_channel == 2);

  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("display");

  TEST_RUN(test_default_settings);
  TEST_RUN(test_viewport_display_roundtrip);
  TEST_RUN(test_viewport_default_display);
  TEST_RUN(test_null_safety);
  TEST_RUN(test_vertex_map_modes);

  TEST_REPORT();
  TEST_EXIT();
}
