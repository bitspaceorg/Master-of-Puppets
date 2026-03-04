/*
 * Master of Puppets — Theme Tests
 * test_theme.c — Tests for MOP theme system
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

static void test_theme_default_valid(void) {
  TEST_BEGIN("theme_default_valid");
  MopTheme t = mop_theme_default();
  /* Background colors should be reasonable */
  TEST_ASSERT(t.bg_top.r > 0.0f);
  TEST_ASSERT(t.bg_bottom.r >= 0.0f);
  /* Grid colors */
  TEST_ASSERT(t.grid_minor.a > 0.0f);
  TEST_ASSERT(t.grid_major.a > 0.0f);
  /* Gizmo colors */
  TEST_ASSERT(t.gizmo_x.r > 0.5f); /* red dominant */
  TEST_ASSERT(t.gizmo_y.g > 0.5f); /* green dominant */
  TEST_ASSERT(t.gizmo_z.b > 0.5f); /* blue dominant */
  TEST_END();
}

static void test_theme_set_get(void) {
  TEST_BEGIN("theme_set_get");
  MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU});
  TEST_ASSERT(vp != NULL);

  const MopTheme *t = mop_viewport_get_theme(vp);
  TEST_ASSERT(t != NULL);

  MopTheme custom = *t;
  custom.bg_top = (MopColor){1.0f, 0.0f, 0.0f, 1.0f};
  mop_viewport_set_theme(vp, &custom);

  const MopTheme *t2 = mop_viewport_get_theme(vp);
  TEST_ASSERT(t2->bg_top.r == 1.0f);
  TEST_ASSERT(t2->bg_top.g == 0.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("theme");

  TEST_RUN(test_theme_default_valid);
  TEST_RUN(test_theme_set_get);

  TEST_REPORT();
  TEST_EXIT();
}
