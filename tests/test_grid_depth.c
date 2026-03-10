/*
 * Master of Puppets — Grid Depth Occlusion Tests
 * test_grid_depth.c — Verify grid is properly occluded by scene geometry
 *
 * Strategy: render with chrome ON (grid visible) vs OFF (grid hidden) and
 * compare object-interior pixels. For surfaces above Y=0, the grid must
 * not bleed — pixels should be identical. For empty ground, the grid
 * axes should be detectable by their distinct color signature.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>
#include <stdlib.h>

/* =========================================================================
 * Shared cube geometry
 * ========================================================================= */

static const MopVertex CUBE_VERTS[] = {
    /* Front face (z=+0.5) */
    {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.9f, 0.9f, 0.9f, 1}, 0, 0},
    {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.9f, 0.9f, 0.9f, 1}, 1, 0},
    {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.9f, 0.9f, 0.9f, 1}, 1, 1},
    {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.9f, 0.9f, 0.9f, 1}, 0, 1},
    /* Back face (z=-0.5) */
    {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.9f, 0.9f, 0.9f, 1}, 0, 0},
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.9f, 0.9f, 0.9f, 1}, 1, 0},
    {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.9f, 0.9f, 0.9f, 1}, 1, 1},
    {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.9f, 0.9f, 0.9f, 1}, 0, 1},
    /* Top face (y=+0.5) */
    {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.9f, 0.9f, 0.9f, 1}, 0, 0},
    {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.9f, 0.9f, 0.9f, 1}, 1, 0},
    {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.9f, 0.9f, 0.9f, 1}, 1, 1},
    {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.9f, 0.9f, 0.9f, 1}, 0, 1},
    /* Bottom face (y=-0.5) */
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.9f, 0.9f, 0.9f, 1}, 0, 0},
    {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.9f, 0.9f, 0.9f, 1}, 1, 0},
    {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.9f, 0.9f, 0.9f, 1}, 1, 1},
    {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.9f, 0.9f, 0.9f, 1}, 0, 1},
    /* Right face (x=+0.5) */
    {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {0.9f, 0.9f, 0.9f, 1}, 0, 0},
    {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0.9f, 0.9f, 0.9f, 1}, 1, 0},
    {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0.9f, 0.9f, 0.9f, 1}, 1, 1},
    {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {0.9f, 0.9f, 0.9f, 1}, 0, 1},
    /* Left face (x=-0.5) */
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {0.9f, 0.9f, 0.9f, 1}, 0, 0},
    {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0.9f, 0.9f, 0.9f, 1}, 1, 0},
    {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0.9f, 0.9f, 0.9f, 1}, 1, 1},
    {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {0.9f, 0.9f, 0.9f, 1}, 0, 1},
};
static const uint32_t CUBE_IDX[] = {
    0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
    12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20,
};

/* =========================================================================
 * Helpers
 * ========================================================================= */

static MopViewport *make_viewport(int w, int h) {
  MopViewport *vp = mop_viewport_create(
      &(MopViewportDesc){.width = w, .height = h, .backend = MOP_BACKEND_CPU});
  return vp;
}

static MopMesh *add_cube(MopViewport *vp, MopVec3 pos, MopVec3 scale,
                         uint32_t id) {
  MopMesh *mesh =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                               .vertex_count = 24,
                                               .indices = CUBE_IDX,
                                               .index_count = 36,
                                               .object_id = id});
  if (mesh) {
    mop_mesh_set_position(mesh, pos);
    mop_mesh_set_scale(mesh, scale);
  }
  return mesh;
}

/* Average RGB over a (2*radius+1)² block centered at (cx,cy). */
static void sample_block(const uint8_t *fb, int w, int h, int cx, int cy,
                         int radius, float *avg_r, float *avg_g, float *avg_b) {
  float sr = 0, sg = 0, sb = 0;
  int count = 0;
  for (int dy = -radius; dy <= radius; dy++) {
    for (int dx = -radius; dx <= radius; dx++) {
      int px = cx + dx, py = cy + dy;
      if (px < 0 || py < 0 || px >= w || py >= h)
        continue;
      int idx = (py * w + px) * 4;
      sr += fb[idx + 0];
      sg += fb[idx + 1];
      sb += fb[idx + 2];
      count++;
    }
  }
  if (count > 0) {
    *avg_r = sr / count;
    *avg_g = sg / count;
    *avg_b = sb / count;
  } else {
    *avg_r = *avg_g = *avg_b = 0;
  }
}

/* =========================================================================
 * 1. test_grid_no_bleed_floating_object
 *
 *    A large cube floats well above Y=0 (bottom at Y=1). Camera looks
 *    straight at the front face. Grid must not bleed onto the cube.
 *    Compare chrome ON vs OFF at the center pixel.
 * ========================================================================= */

static void test_grid_no_bleed_floating_object(void) {
  TEST_BEGIN("grid_no_bleed_floating_object");

  int w = 128, h = 96;
  MopViewport *vp = make_viewport(w, h);
  TEST_ASSERT(vp != NULL);

  /* Cube: center Y=3, scale 4 → Y spans [1, 5], far above grid */
  add_cube(vp, (MopVec3){0, 3, 0}, (MopVec3){4, 4, 4}, 1);
  mop_viewport_set_camera(vp, (MopVec3){0, 3, 8}, (MopVec3){0, 3, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_set_ambient(vp, 0.5f);

  /* Render with grid ON */
  mop_viewport_render(vp);
  int fb_w, fb_h;
  const uint8_t *fb = mop_viewport_read_color(vp, &fb_w, &fb_h);
  TEST_ASSERT(fb != NULL);

  float r1, g1, b1;
  sample_block(fb, fb_w, fb_h, w / 2, h / 2, 3, &r1, &g1, &b1);

  /* Render with grid OFF */
  mop_viewport_set_chrome(vp, false);
  mop_viewport_render(vp);
  fb = mop_viewport_read_color(vp, &fb_w, &fb_h);
  TEST_ASSERT(fb != NULL);

  float r2, g2, b2;
  sample_block(fb, fb_w, fb_h, w / 2, h / 2, 3, &r2, &g2, &b2);

  /* Object-interior pixels must be identical (tolerance 3 for rounding) */
  float diff = fabsf(r1 - r2) + fabsf(g1 - g2) + fabsf(b1 - b2);
  TEST_ASSERT_MSG(diff < 3.0f, "grid must not bleed onto floating object");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 2. test_grid_no_bleed_steep_angle
 *
 *    Cube sits on the grid (bottom at Y=0, top at Y=2). Camera looks
 *    straight down at the top face. Even at steep angles, the grid
 *    must not bleed onto the top face.
 * ========================================================================= */

static void test_grid_no_bleed_steep_angle(void) {
  TEST_BEGIN("grid_no_bleed_steep_angle");

  int w = 128, h = 96;
  MopViewport *vp = make_viewport(w, h);
  TEST_ASSERT(vp != NULL);

  /* Cube: center Y=1, scale 2 → Y spans [0, 2] */
  add_cube(vp, (MopVec3){0, 1, 0}, (MopVec3){2, 2, 2}, 1);
  /* Camera almost directly above, looking down at top face */
  mop_viewport_set_camera(vp, (MopVec3){0.0f, 10.0f, 0.01f}, (MopVec3){0, 1, 0},
                          (MopVec3){0, 0, -1}, 60.0f, 0.1f, 100.0f);
  mop_viewport_set_ambient(vp, 0.5f);

  /* Render with grid ON */
  mop_viewport_render(vp);
  int fb_w, fb_h;
  const uint8_t *fb = mop_viewport_read_color(vp, &fb_w, &fb_h);
  TEST_ASSERT(fb != NULL);

  float r1, g1, b1;
  sample_block(fb, fb_w, fb_h, w / 2, h / 2, 3, &r1, &g1, &b1);

  /* Render with grid OFF */
  mop_viewport_set_chrome(vp, false);
  mop_viewport_render(vp);
  fb = mop_viewport_read_color(vp, &fb_w, &fb_h);
  TEST_ASSERT(fb != NULL);

  float r2, g2, b2;
  sample_block(fb, fb_w, fb_h, w / 2, h / 2, 3, &r2, &g2, &b2);

  float diff = fabsf(r1 - r2) + fabsf(g1 - g2) + fabsf(b1 - b2);
  TEST_ASSERT_MSG(diff < 3.0f,
                  "grid must not bleed onto top face at steep angle");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 3. test_grid_no_bleed_side_face
 *
 *    Cube sits on the grid (bottom at Y=0). Camera looks at the front
 *    face center (Y=1, well above Y=0). The side face pixels must be
 *    unaffected by the grid.
 * ========================================================================= */

static void test_grid_no_bleed_side_face(void) {
  TEST_BEGIN("grid_no_bleed_side_face");

  int w = 128, h = 96;
  MopViewport *vp = make_viewport(w, h);
  TEST_ASSERT(vp != NULL);

  /* Cube: center Y=1, scale 2 → Y spans [0, 2] */
  add_cube(vp, (MopVec3){0, 1, 0}, (MopVec3){2, 2, 2}, 1);
  /* Camera at eye level looking at front face center */
  mop_viewport_set_camera(vp, (MopVec3){0, 1, 5}, (MopVec3){0, 1, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_set_ambient(vp, 0.5f);

  /* Render with grid ON */
  mop_viewport_render(vp);
  int fb_w, fb_h;
  const uint8_t *fb = mop_viewport_read_color(vp, &fb_w, &fb_h);
  TEST_ASSERT(fb != NULL);

  float r1, g1, b1;
  sample_block(fb, fb_w, fb_h, w / 2, h / 2, 3, &r1, &g1, &b1);

  /* Render with grid OFF */
  mop_viewport_set_chrome(vp, false);
  mop_viewport_render(vp);
  fb = mop_viewport_read_color(vp, &fb_w, &fb_h);
  TEST_ASSERT(fb != NULL);

  float r2, g2, b2;
  sample_block(fb, fb_w, fb_h, w / 2, h / 2, 3, &r2, &g2, &b2);

  float diff = fabsf(r1 - r2) + fabsf(g1 - g2) + fabsf(b1 - b2);
  TEST_ASSERT_MSG(diff < 3.0f, "grid must not bleed onto side face at Y=1");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 4. test_grid_visible_at_origin
 *
 *    Empty scene, camera looking at origin. The X-axis (red) and Z-axis
 *    (blue) should be visible near screen center. Scan for pixels whose
 *    color signature matches the axis theme colors.
 * ========================================================================= */

static void test_grid_visible_at_origin(void) {
  TEST_BEGIN("grid_visible_at_origin");

  int w = 128, h = 96;
  MopViewport *vp = make_viewport(w, h);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){3, 3, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  mop_viewport_render(vp);
  int fb_w, fb_h;
  const uint8_t *fb = mop_viewport_read_color(vp, &fb_w, &fb_h);
  TEST_ASSERT(fb != NULL);

  /* Scan a 21×21 block centered on screen for axis-colored pixels.
   * Default theme: axis_x ≈ red (R >> G,B), axis_z ≈ blue (B >> R,G).
   * After alpha blending with gray background, the dominant channel
   * should still be clearly stronger than the other two. */
  int found_axis = 0;
  int scan = 10;
  for (int dy = -scan; dy <= scan && !found_axis; dy++) {
    for (int dx = -scan; dx <= scan && !found_axis; dx++) {
      int px = w / 2 + dx, py = h / 2 + dy;
      if (px < 0 || py < 0 || px >= fb_w || py >= fb_h)
        continue;
      int idx = (py * fb_w + px) * 4;
      uint8_t r = fb[idx + 0], g = fb[idx + 1], b = fb[idx + 2];
      /* Red axis: R channel dominant by wide margin */
      if (r > 100 && r > g + 40 && r > b + 40) {
        found_axis = 1;
        break;
      }
      /* Blue axis: B channel dominant */
      if (b > 100 && b > r + 40 && b > g + 20) {
        found_axis = 1;
        break;
      }
    }
  }
  TEST_ASSERT_MSG(found_axis, "grid axis lines should be visible at origin");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 5. test_grid_no_bleed_multiple_angles
 *
 *    Rotate camera around a cube at Y=2 and verify no bleed at each angle.
 *    This catches angle-dependent bleed that a single-angle test might miss.
 * ========================================================================= */

static void test_grid_no_bleed_multiple_angles(void) {
  TEST_BEGIN("grid_no_bleed_multiple_angles");

  int w = 128, h = 96;

  /* Test 8 camera positions around a cube floating above the grid */
  static const float angles[] = {0.0f,   0.785f, 1.571f, 2.356f,
                                 3.142f, 3.927f, 4.712f, 5.497f};
  int angle_count = (int)(sizeof(angles) / sizeof(angles[0]));
  float max_diff = 0.0f;

  for (int i = 0; i < angle_count; i++) {
    float a = angles[i];
    float ex = 5.0f * sinf(a), ez = 5.0f * cosf(a);

    MopViewport *vp = make_viewport(w, h);
    TEST_ASSERT(vp != NULL);

    /* Cube at Y=2, scale 3 → bottom at Y=0.5 */
    add_cube(vp, (MopVec3){0, 2, 0}, (MopVec3){3, 3, 3}, 1);
    mop_viewport_set_camera(vp, (MopVec3){ex, 3.0f, ez}, (MopVec3){0, 2, 0},
                            (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
    mop_viewport_set_ambient(vp, 0.5f);

    /* Chrome ON */
    mop_viewport_render(vp);
    int fb_w, fb_h;
    const uint8_t *fb = mop_viewport_read_color(vp, &fb_w, &fb_h);
    TEST_ASSERT(fb != NULL);

    float r1, g1, b1;
    sample_block(fb, fb_w, fb_h, w / 2, h / 2, 3, &r1, &g1, &b1);

    /* Chrome OFF */
    mop_viewport_set_chrome(vp, false);
    mop_viewport_render(vp);
    fb = mop_viewport_read_color(vp, &fb_w, &fb_h);
    TEST_ASSERT(fb != NULL);

    float r2, g2, b2;
    sample_block(fb, fb_w, fb_h, w / 2, h / 2, 3, &r2, &g2, &b2);

    float diff = fabsf(r1 - r2) + fabsf(g1 - g2) + fabsf(b1 - b2);
    if (diff > max_diff)
      max_diff = diff;

    mop_viewport_destroy(vp);
  }

  TEST_ASSERT_MSG(max_diff < 3.0f, "grid must not bleed at any viewing angle");

  TEST_END();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
  TEST_SUITE_BEGIN("grid_depth");

  TEST_RUN(test_grid_no_bleed_floating_object);
  TEST_RUN(test_grid_no_bleed_steep_angle);
  TEST_RUN(test_grid_no_bleed_side_face);
  TEST_RUN(test_grid_visible_at_origin);
  TEST_RUN(test_grid_no_bleed_multiple_angles);

  TEST_REPORT();
  TEST_EXIT();
}
