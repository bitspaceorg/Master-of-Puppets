/*
 * Master of Puppets — Exposure Tests
 * test_exposure.c — Verify exposure does not affect default background
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static MopViewport *make_viewport(int w, int h) {
  MopViewportDesc desc = {.width = w, .height = h, .backend = MOP_BACKEND_CPU};
  return mop_viewport_create(&desc);
}

/* Sample a pixel's RGB from the readback buffer.  Returns false if readback
 * is unavailable. */
static bool sample_pixel(MopViewport *vp, int x, int y, uint8_t out[3]) {
  int w, h;
  const uint8_t *px = mop_viewport_read_color(vp, &w, &h);
  if (!px)
    return false;
  if (x < 0 || x >= w || y < 0 || y >= h)
    return false;
  int idx = (y * w + x) * 4;
  out[0] = px[idx + 0];
  out[1] = px[idx + 1];
  out[2] = px[idx + 2];
  return true;
}

/* -------------------------------------------------------------------------
 * Test: background color is unchanged across exposure values (no skybox)
 * ------------------------------------------------------------------------- */

static void test_bg_exposure_invariant(void) {
  TEST_BEGIN("bg_exposure_invariant");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  /* Render at default exposure (1.0) — no scene objects, just background */
  mop_viewport_set_exposure(vp, 1.0f);
  MopRenderResult r1 = mop_viewport_render(vp);
  TEST_ASSERT(r1 == MOP_RENDER_OK);

  /* Sample center pixel (pure background) */
  uint8_t bg1[3];
  TEST_ASSERT(sample_pixel(vp, 32, 32, bg1));

  /* Render at high exposure (5.0) */
  mop_viewport_set_exposure(vp, 5.0f);
  MopRenderResult r2 = mop_viewport_render(vp);
  TEST_ASSERT(r2 == MOP_RENDER_OK);

  uint8_t bg2[3];
  TEST_ASSERT(sample_pixel(vp, 32, 32, bg2));

  /* Background should be identical regardless of exposure */
  TEST_ASSERT_MSG(bg1[0] == bg2[0] && bg1[1] == bg2[1] && bg1[2] == bg2[2],
                  "background changed with exposure (no skybox)");

  /* Render at low exposure (0.1) */
  mop_viewport_set_exposure(vp, 0.1f);
  MopRenderResult r3 = mop_viewport_render(vp);
  TEST_ASSERT(r3 == MOP_RENDER_OK);

  uint8_t bg3[3];
  TEST_ASSERT(sample_pixel(vp, 32, 32, bg3));

  TEST_ASSERT_MSG(bg1[0] == bg3[0] && bg1[1] == bg3[1] && bg1[2] == bg3[2],
                  "background changed with low exposure (no skybox)");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Test: scene object IS affected by exposure
 * ------------------------------------------------------------------------- */

static const MopVertex TRI_VERTS[] = {
    {{0.0f, 0.5f, -1.0f}, {0, 0, 1}, {1, 0, 0, 1}},
    {{-0.5f, -0.5f, -1.0f}, {0, 0, 1}, {1, 0, 0, 1}},
    {{0.5f, -0.5f, -1.0f}, {0, 0, 1}, {1, 0, 0, 1}},
};
static const uint32_t TRI_IDX[] = {0, 1, 2};

static void test_scene_exposure_changes(void) {
  TEST_BEGIN("scene_exposure_changes");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  /* Position camera looking at origin along -Z */
  mop_viewport_set_camera(vp, (MopVec3){0, 0, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  /* Add a bright red triangle centered in view */
  MopMesh *m = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = TRI_VERTS,
                                                        .vertex_count = 3,
                                                        .indices = TRI_IDX,
                                                        .index_count = 3,
                                                        .object_id = 1});
  TEST_ASSERT(m != NULL);

  /* Add a light so the triangle is visible */
  MopLight light_desc = {.type = MOP_LIGHT_DIRECTIONAL,
                         .direction = {0, 0, -1},
                         .color = {1, 1, 1, 1},
                         .intensity = 3.0f,
                         .active = true};
  MopLight *light = mop_viewport_add_light(vp, &light_desc);
  TEST_ASSERT(light != NULL);

  /* Render at exposure 1.0 */
  mop_viewport_set_exposure(vp, 1.0f);
  mop_viewport_render(vp);

  /* Find a scene pixel by scanning for non-background object_id */
  int w, h;
  const uint8_t *pixels = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(pixels != NULL);

  /* Sample the center — if the triangle covers it */
  uint8_t px1[3];
  TEST_ASSERT(sample_pixel(vp, 32, 32, px1));

  /* Render at exposure 5.0 */
  mop_viewport_set_exposure(vp, 5.0f);
  mop_viewport_render(vp);

  uint8_t px2[3];
  TEST_ASSERT(sample_pixel(vp, 32, 32, px2));

  /* Scene pixel should differ — higher exposure = brighter.
   * Even if center is background, at least one pixel must differ
   * if scene objects respond to exposure. Check a wide scan. */
  bool found_diff = false;
  for (int y = 16; y < 48 && !found_diff; y += 4) {
    for (int x = 16; x < 48 && !found_diff; x += 4) {
      mop_viewport_set_exposure(vp, 1.0f);
      mop_viewport_render(vp);
      uint8_t a[3];
      sample_pixel(vp, x, y, a);

      mop_viewport_set_exposure(vp, 5.0f);
      mop_viewport_render(vp);
      uint8_t b[3];
      sample_pixel(vp, x, y, b);

      int d = abs((int)a[0] - (int)b[0]) + abs((int)a[1] - (int)b[1]) +
              abs((int)a[2] - (int)b[2]);
      if (d > 5)
        found_diff = true;
    }
  }
  TEST_ASSERT_MSG(found_diff, "no scene pixel changed with exposure");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Test: corner pixels (background) also stable
 * ------------------------------------------------------------------------- */

static void test_bg_corners_stable(void) {
  TEST_BEGIN("bg_corners_exposure_stable");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_exposure(vp, 1.0f);
  mop_viewport_render(vp);

  uint8_t tl1[3], br1[3];
  TEST_ASSERT(sample_pixel(vp, 1, 1, tl1));
  TEST_ASSERT(sample_pixel(vp, 62, 62, br1));

  mop_viewport_set_exposure(vp, 10.0f);
  mop_viewport_render(vp);

  uint8_t tl2[3], br2[3];
  TEST_ASSERT(sample_pixel(vp, 1, 1, tl2));
  TEST_ASSERT(sample_pixel(vp, 62, 62, br2));

  TEST_ASSERT_MSG(tl1[0] == tl2[0] && tl1[1] == tl2[1] && tl1[2] == tl2[2],
                  "top-left corner changed with exposure");
  TEST_ASSERT_MSG(br1[0] == br2[0] && br1[1] == br2[1] && br1[2] == br2[2],
                  "bottom-right corner changed with exposure");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("exposure");

  TEST_RUN(test_bg_exposure_invariant);
  TEST_RUN(test_scene_exposure_changes);
  TEST_RUN(test_bg_corners_stable);

  TEST_REPORT();
  TEST_EXIT();
}
