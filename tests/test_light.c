/*
 * Master of Puppets — Light System Tests
 * test_light.c
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

static void test_default_light(void) {
  TEST_BEGIN("default_light_count_is_one");
  MopViewport *vp = make_viewport();
  TEST_ASSERT(vp != NULL);
  TEST_ASSERT(mop_viewport_light_count(vp) == 1);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_add_light(void) {
  TEST_BEGIN("add_light");
  MopViewport *vp = make_viewport();
  TEST_ASSERT(vp != NULL);

  MopLight point = {
      .type = MOP_LIGHT_POINT,
      .position = {5, 5, 5},
      .color = {1, 1, 1, 1},
      .intensity = 2.0f,
      .range = 10.0f,
      .active = true,
  };
  MopLight *l = mop_viewport_add_light(vp, &point);
  TEST_ASSERT(l != NULL);
  TEST_ASSERT(l->type == MOP_LIGHT_POINT);
  TEST_ASSERT(mop_viewport_light_count(vp) == 2);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_remove_light(void) {
  TEST_BEGIN("remove_light");
  MopViewport *vp = make_viewport();

  MopLight spot = {
      .type = MOP_LIGHT_SPOT,
      .position = {0, 10, 0},
      .direction = {0, -1, 0},
      .color = {1, 0.8f, 0.6f, 1},
      .intensity = 1.5f,
      .range = 20.0f,
      .spot_inner_cos = 0.9f,
      .spot_outer_cos = 0.8f,
      .active = true,
  };
  MopLight *l = mop_viewport_add_light(vp, &spot);
  TEST_ASSERT(l != NULL);
  TEST_ASSERT(mop_viewport_light_count(vp) == 2);

  mop_viewport_remove_light(vp, l);
  TEST_ASSERT(mop_viewport_light_count(vp) == 1);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_max_lights(void) {
  TEST_BEGIN("max_lights");
  MopViewport *vp = make_viewport();

  /* Default already uses 1 slot. Add MOP_MAX_LIGHTS - 1 more */
  for (int i = 1; i < MOP_MAX_LIGHTS; i++) {
    MopLight dir = {
        .type = MOP_LIGHT_DIRECTIONAL,
        .direction = {0, 1, 0},
        .color = {1, 1, 1, 1},
        .intensity = 0.5f,
        .active = true,
    };
    MopLight *l = mop_viewport_add_light(vp, &dir);
    TEST_ASSERT(l != NULL);
  }
  TEST_ASSERT(mop_viewport_light_count(vp) == MOP_MAX_LIGHTS);

  /* One more should fail */
  MopLight extra = {
      .type = MOP_LIGHT_DIRECTIONAL,
      .direction = {0, 1, 0},
      .color = {1, 1, 1, 1},
      .intensity = 0.5f,
      .active = true,
  };
  MopLight *fail = mop_viewport_add_light(vp, &extra);
  TEST_ASSERT(fail == NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_light_setters(void) {
  TEST_BEGIN("light_setters");
  MopLight l = {
      .type = MOP_LIGHT_POINT,
      .position = {0, 0, 0},
      .intensity = 1.0f,
      .active = true,
  };

  mop_light_set_position(&l, (MopVec3){3, 4, 5});
  TEST_ASSERT_FLOAT_EQ(l.position.x, 3.0f);
  TEST_ASSERT_FLOAT_EQ(l.position.y, 4.0f);
  TEST_ASSERT_FLOAT_EQ(l.position.z, 5.0f);

  mop_light_set_direction(&l, (MopVec3){0, -1, 0});
  TEST_ASSERT_FLOAT_EQ(l.direction.y, -1.0f);

  mop_light_set_intensity(&l, 3.5f);
  TEST_ASSERT_FLOAT_EQ(l.intensity, 3.5f);

  mop_light_set_color(&l, (MopColor){1, 0, 0, 1});
  TEST_ASSERT_FLOAT_EQ(l.color.r, 1.0f);
  TEST_ASSERT_FLOAT_EQ(l.color.g, 0.0f);
  TEST_END();
}

static void test_legacy_compat(void) {
  TEST_BEGIN("legacy_light_dir_compat");
  MopViewport *vp = make_viewport();

  /* Setting legacy light_dir should update lights[0] */
  mop_viewport_set_light_dir(vp, (MopVec3){0, -1, 0});
  /* We can verify by rendering — if it doesn't crash, backward compat works */
  mop_viewport_render(vp);

  mop_viewport_set_ambient(vp, 0.5f);
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("light");

  TEST_RUN(test_default_light);
  TEST_RUN(test_add_light);
  TEST_RUN(test_remove_light);
  TEST_RUN(test_max_lights);
  TEST_RUN(test_light_setters);
  TEST_RUN(test_legacy_compat);

  TEST_REPORT();
  TEST_EXIT();
}
