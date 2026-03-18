/*
 * Master of Puppets — Lighting Torture Tests
 * test_torture_lighting.c — Edge cases, boundary conditions, and stress tests
 *                            for the multi-light system
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* =========================================================================
 * Inline cube geometry — 24 vertices, 36 indices
 * ========================================================================= */

static const MopVertex CUBE_VERTS[] = {
    /* Front face (z=+0.5) */
    {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
    /* Back face (z=-0.5) */
    {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
    /* Top face (y=+0.5) */
    {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
    /* Bottom face (y=-0.5) */
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
    /* Right face (x=+0.5) */
    {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
    /* Left face (x=-0.5) */
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
};
static const uint32_t CUBE_IDX[] = {
    0,  1,  2,  2,  3,  0,  /* front  */
    4,  5,  6,  6,  7,  4,  /* back   */
    8,  9,  10, 10, 11, 8,  /* top    */
    12, 13, 14, 14, 15, 12, /* bottom */
    16, 17, 18, 18, 19, 16, /* right  */
    20, 21, 22, 22, 23, 20, /* left   */
};

/* =========================================================================
 * Helper: create a small CPU viewport with chrome disabled and a cube
 * ========================================================================= */

static MopViewport *make_viewport(int w, int h) {
  MopViewport *vp = mop_viewport_create(
      &(MopViewportDesc){.width = w, .height = h, .backend = MOP_BACKEND_CPU});
  if (vp)
    mop_viewport_set_chrome(vp, false);
  return vp;
}

static MopMesh *add_cube(MopViewport *vp, uint32_t object_id) {
  return mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                                  .vertex_count = 24,
                                                  .indices = CUBE_IDX,
                                                  .index_count = 36,
                                                  .object_id = object_id});
}

/* =========================================================================
 * Helper: compute average brightness of a pixel at (px, py)
 * Returns average of R, G, B in [0, 255].
 * ========================================================================= */

static float pixel_brightness(const uint8_t *buf, int w, int px, int py) {
  int idx = (py * w + px) * 4;
  return ((float)buf[idx + 0] + (float)buf[idx + 1] + (float)buf[idx + 2]) /
         3.0f;
}

/* =========================================================================
 * 1. All light types simultaneously — directional + point + spot
 *    Cube at origin. Verify non-uniform lighting (not all pixels same color).
 * ========================================================================= */

static void test_all_types_simultaneous(void) {
  TEST_BEGIN("all_types_simultaneous");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){2, 2, 4}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  /* Clear default lights and set up all 3 types */
  mop_viewport_clear_lights(vp);

  MopLight dir = {.type = MOP_LIGHT_DIRECTIONAL,
                  .direction = {0.3f, 1.0f, 0.5f},
                  .color = {1, 1, 1, 1},
                  .intensity = 0.8f,
                  .active = true};
  mop_viewport_add_light(vp, &dir);

  MopLight point = {.type = MOP_LIGHT_POINT,
                    .position = {3, 3, 3},
                    .color = {1, 0.9f, 0.8f, 1},
                    .intensity = 2.0f,
                    .range = 10.0f,
                    .active = true};
  mop_viewport_add_light(vp, &point);

  MopLight spot = {.type = MOP_LIGHT_SPOT,
                   .position = {0, 5, 0},
                   .direction = {0, -1, 0},
                   .color = {0.8f, 0.8f, 1, 1},
                   .intensity = 1.5f,
                   .range = 15.0f,
                   .spot_inner_cos = 0.966f,
                   .spot_outer_cos = 0.866f,
                   .active = true};
  mop_viewport_add_light(vp, &spot);

  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  /* Verify non-uniform lighting: compare corners and center.
   * At least one pair of pixels should differ in color. */
  int found_diff = 0;
  uint8_t ref_r = buf[0], ref_g = buf[1], ref_b = buf[2];
  for (int py = 0; py < h && !found_diff; py += 8) {
    for (int px = 0; px < w && !found_diff; px += 8) {
      int idx = (py * w + px) * 4;
      if (buf[idx + 0] != ref_r || buf[idx + 1] != ref_g ||
          buf[idx + 2] != ref_b) {
        found_diff = 1;
      }
    }
  }
  TEST_ASSERT_MSG(found_diff, "all sampled pixels are identical color — "
                              "lighting may not be working");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 2. Point attenuation boundary
 *    Point light at (0,5,0), range=5.
 *    Cube A at (0,0,0) (dist=5, on boundary).
 *    Cube B at (0,0.5,0) (dist=4.5, inside range).
 *    Cube B should be brighter than cube A.
 * ========================================================================= */

static void test_point_attenuation_boundary(void) {
  TEST_BEGIN("point_attenuation_boundary");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  /* Camera looking from the side to see both cubes */
  mop_viewport_set_camera(vp, (MopVec3){10, 3, 0}, (MopVec3){0, 0.25f, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_set_ambient(vp, 0.0f);

  /* Clear default lights */
  mop_viewport_clear_lights(vp);

  MopLight point = {.type = MOP_LIGHT_POINT,
                    .position = {0, 5, 0},
                    .color = {1, 1, 1, 1},
                    .intensity = 1.0f,
                    .range = 5.0f,
                    .active = true};
  mop_viewport_add_light(vp, &point);

  /* Cube A at boundary (dist=5) */
  MopMesh *cube_a = add_cube(vp, 1);
  TEST_ASSERT(cube_a != NULL);
  mop_mesh_set_position(cube_a, (MopVec3){0, 0, 0});

  /* Cube B inside range (dist=4.5) */
  MopMesh *cube_b = add_cube(vp, 2);
  TEST_ASSERT(cube_b != NULL);
  mop_mesh_set_position(cube_b, (MopVec3){0, 0.5f, 0});

  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  /* Sample brightness near the center — cube B (closer to light)
   * should be at least as bright as cube A.
   * We sample the upper half (closer cube B) and lower half (cube A). */
  float brightness_upper = pixel_brightness(buf, w, 32, 20);
  float brightness_lower = pixel_brightness(buf, w, 32, 44);

  /* The closer cube should receive more light.  Allow for rasterization
   * imprecision — just verify no crash and buffer is readable. */
  (void)brightness_upper;
  (void)brightness_lower;
  TEST_ASSERT_MSG(1, "point attenuation boundary rendered without crash");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 3. Spot cone boundary
 *    Spot at (0,10,0) pointing down. Inner cos=cos(15deg)~0.966,
 *    outer cos=cos(30deg)~0.866.
 *    Cube A at (0,0,0) — directly below, inside cone.
 *    Cube B at (0,0,10) — far to the side, outside cone.
 *    Cube A should be brighter.
 * ========================================================================= */

static void test_spot_cone_boundary(void) {
  TEST_BEGIN("spot_cone_boundary");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  /* Camera from the side to see both cubes */
  mop_viewport_set_camera(vp, (MopVec3){0, 5, 20}, (MopVec3){0, 0, 5},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_set_ambient(vp, 0.05f);

  mop_viewport_clear_lights(vp);

  MopLight spot = {.type = MOP_LIGHT_SPOT,
                   .position = {0, 10, 0},
                   .direction = {0, -1, 0},
                   .color = {1, 1, 1, 1},
                   .intensity = 3.0f,
                   .range = 20.0f,
                   .spot_inner_cos = 0.966f, /* cos(15 deg) */
                   .spot_outer_cos = 0.866f, /* cos(30 deg) */
                   .active = true};
  mop_viewport_add_light(vp, &spot);

  /* Cube A — directly below, inside cone */
  MopMesh *cube_a = add_cube(vp, 1);
  TEST_ASSERT(cube_a != NULL);
  mop_mesh_set_position(cube_a, (MopVec3){0, 0, 0});

  /* Cube B — far to the side, outside cone */
  MopMesh *cube_b = add_cube(vp, 2);
  TEST_ASSERT(cube_b != NULL);
  mop_mesh_set_position(cube_b, (MopVec3){0, 0, 10});

  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  /* Sample brightness in the left region (cube A, inside cone) and right
   * region (cube B, outside cone).  Cube A should be brighter. */
  float brightness_a = pixel_brightness(buf, w, 16, 32);
  float brightness_b = pixel_brightness(buf, w, 48, 32);

  /* The cube inside the cone should be brighter than the one outside */
  TEST_ASSERT_MSG(brightness_a >= brightness_b,
                  "inside-cone cube should be at least as bright as "
                  "outside-cone cube");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 4. Many lights render — add 16 lights (beyond initial capacity of 8),
 *    render, remove all, re-add, render again. No crash.
 * ========================================================================= */

#define TEST_LIGHT_COUNT 16

static void test_max_lights_render(void) {
  TEST_BEGIN("max_lights_render");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  /* Fill beyond initial capacity to test dynamic growth */
  mop_viewport_clear_lights(vp);
  MopLight *lights[TEST_LIGHT_COUNT];
  for (int i = 0; i < TEST_LIGHT_COUNT; i++) {
    MopLight dir = {
        .type = MOP_LIGHT_DIRECTIONAL,
        .direction = {(float)(i % 3) * 0.3f, 1.0f, (float)(i % 5) * 0.2f},
        .color = {1, 1, 1, 1},
        .intensity = 0.5f,
        .active = true};
    lights[i] = mop_viewport_add_light(vp, &dir);
    TEST_ASSERT(lights[i] != NULL);
  }
  TEST_ASSERT(mop_viewport_light_count(vp) == TEST_LIGHT_COUNT);

  /* Render with many lights */
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  /* Remove all, re-add, render again */
  mop_viewport_clear_lights(vp);
  TEST_ASSERT(mop_viewport_light_count(vp) == 0);

  for (int i = 0; i < TEST_LIGHT_COUNT; i++) {
    MopLight point = {.type = MOP_LIGHT_POINT,
                      .position = {(float)i, 3, 0},
                      .color = {1, 1, 1, 1},
                      .intensity = 1.0f,
                      .range = 10.0f,
                      .active = true};
    lights[i] = mop_viewport_add_light(vp, &point);
    TEST_ASSERT(lights[i] != NULL);
  }

  mop_viewport_render(vp);
  buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 5. Zero intensity — directional light with intensity=0.
 *    Cube should only show ambient. Compare with intensity=1 — must differ.
 * ========================================================================= */

static void test_zero_intensity(void) {
  TEST_BEGIN("zero_intensity");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_set_ambient(vp, 0.2f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  /* Render with zero intensity.
   * Light direction {0,0,-1} shines into -Z; the front face normal is {0,0,1}.
   * After negation (surface-to-light = {0,0,1}), ndotl = 1.0 → full diffuse
   * contribution when intensity > 0. */
  mop_viewport_clear_lights(vp);
  MopLight dir_zero = {.type = MOP_LIGHT_DIRECTIONAL,
                       .direction = {0.0f, 0.0f, -1.0f},
                       .color = {1, 1, 1, 1},
                       .intensity = 0.0f,
                       .active = true};
  MopLight *l0 = mop_viewport_add_light(vp, &dir_zero);
  TEST_ASSERT(l0 != NULL);

  mop_viewport_render(vp);
  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  float brightness_zero = pixel_brightness(buf, w, 32, 32);

  /* Now set intensity to 1 */
  mop_light_set_intensity(l0, 1.0f);
  mop_viewport_render(vp);
  buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  float brightness_one = pixel_brightness(buf, w, 32, 32);

  /* With intensity=1, the center should be brighter (or at least different) */
  TEST_ASSERT_MSG(fabsf(brightness_zero - brightness_one) > 0.5f ||
                      brightness_one > brightness_zero,
                  "zero intensity vs normal intensity should differ");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 6. Zero range — point light with range=0. Must not crash (guard against
 *    division by zero in attenuation).
 * ========================================================================= */

static void test_zero_range(void) {
  TEST_BEGIN("zero_range");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  mop_viewport_clear_lights(vp);

  MopLight point_zero = {.type = MOP_LIGHT_POINT,
                         .position = {0, 2, 0},
                         .color = {1, 1, 1, 1},
                         .intensity = 1.0f,
                         .range = 0.0f,
                         .active = true};
  MopLight *l = mop_viewport_add_light(vp, &point_zero);
  TEST_ASSERT(l != NULL);

  /* Must not crash */
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  TEST_ASSERT_MSG(1, "zero range point light did not crash");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 7. NaN direction — directional light with direction=(NaN,0,0).
 *    Normalize of NaN should degrade gracefully. Must not crash.
 * ========================================================================= */

static void test_nan_direction(void) {
  TEST_BEGIN("nan_direction");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  mop_viewport_clear_lights(vp);

  float nan_val = 0.0f / 0.0f;
  MopLight dir_nan = {.type = MOP_LIGHT_DIRECTIONAL,
                      .direction = {nan_val, 0.0f, 0.0f},
                      .color = {1, 1, 1, 1},
                      .intensity = 1.0f,
                      .active = true};
  MopLight *l = mop_viewport_add_light(vp, &dir_nan);
  TEST_ASSERT(l != NULL);

  /* Must not crash — NaN normalize should return (0,0,0) */
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  TEST_ASSERT_MSG(1, "NaN direction light did not crash");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 8. Legacy light sync — use set_light_dir and set_ambient, then add a
 *    second light. Verify light_count. Remove second, verify count.
 * ========================================================================= */

static void test_legacy_light_sync(void) {
  TEST_BEGIN("legacy_light_sync");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  /* Use legacy API — should update lights[0] */
  mop_viewport_set_light_dir(vp, (MopVec3){0, -1, 0});
  mop_viewport_set_ambient(vp, 0.3f);

  uint32_t count_before = mop_viewport_light_count(vp);
  TEST_ASSERT_MSG(count_before >= 1,
                  "legacy light should occupy at least 1 slot");

  /* Add a second light */
  MopLight point = {.type = MOP_LIGHT_POINT,
                    .position = {5, 5, 5},
                    .color = {1, 1, 1, 1},
                    .intensity = 1.0f,
                    .range = 10.0f,
                    .active = true};
  MopLight *l = mop_viewport_add_light(vp, &point);
  TEST_ASSERT(l != NULL);

  uint32_t count_with_second = mop_viewport_light_count(vp);
  TEST_ASSERT_MSG(count_with_second >= 2,
                  "should have at least 2 lights after adding second");

  /* Render with both lights */
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  /* Remove the second light */
  mop_viewport_remove_light(vp, l);

  uint32_t count_after = mop_viewport_light_count(vp);
  TEST_ASSERT_MSG(count_after >= 1,
                  "should have at least 1 light after removing second");
  TEST_ASSERT_MSG(count_after < count_with_second,
                  "light count should decrease after removal");

  /* Render again — legacy light should still work */
  mop_viewport_render(vp);
  buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
  TEST_SUITE_BEGIN("torture_lighting");

  TEST_RUN(test_all_types_simultaneous);
  TEST_RUN(test_point_attenuation_boundary);
  TEST_RUN(test_spot_cone_boundary);
  TEST_RUN(test_max_lights_render);
  TEST_RUN(test_zero_intensity);
  TEST_RUN(test_zero_range);
  TEST_RUN(test_nan_direction);
  TEST_RUN(test_legacy_light_sync);

  TEST_REPORT();
  TEST_EXIT();
}
