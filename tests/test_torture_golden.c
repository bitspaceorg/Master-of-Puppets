/*
 * Master of Puppets — Golden Determinism Torture Tests
 * test_torture_golden.c — Verify that identical scenes produce identical
 *                          framebuffer hashes across repeated renders
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
 * FNV-1a hash helper — deterministic hash of an RGBA8 framebuffer
 * ========================================================================= */

static uint64_t hash_framebuffer(const uint8_t *buf, int w, int h) {
  uint64_t hash = 14695981039346656037ULL;
  size_t len = (size_t)w * (size_t)h * 4;
  for (size_t i = 0; i < len; i++) {
    hash ^= buf[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

/* =========================================================================
 * Helper: create a small CPU viewport with chrome disabled
 * ========================================================================= */

static MopViewport *make_viewport(int w, int h) {
  MopViewport *vp = mop_viewport_create(
      &(MopViewportDesc){.width = w, .height = h, .backend = MOP_BACKEND_CPU});
  if (vp)
    mop_viewport_set_chrome(vp, false);
  return vp;
}

/* =========================================================================
 * 1. Empty scene — no meshes, just clear color
 *    Render 10 times. All hashes must be identical.
 * ========================================================================= */

static void test_empty_scene_deterministic(void) {
  TEST_BEGIN("empty_scene_deterministic");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  /* First render — establish baseline hash */
  mop_viewport_render(vp);
  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  uint64_t baseline = hash_framebuffer(buf, w, h);

  /* Render 9 more times and verify each hash matches */
  for (int i = 1; i < 10; i++) {
    mop_viewport_render(vp);
    buf = mop_viewport_read_color(vp, &w, &h);
    TEST_ASSERT(buf != NULL);
    uint64_t current = hash_framebuffer(buf, w, h);
    TEST_ASSERT_MSG(current == baseline,
                    "empty scene hash differs across renders");
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 2. Single triangle — fixed geometry, camera, clear color
 *    Render twice. Same hash.
 * ========================================================================= */

static void test_single_triangle_deterministic(void) {
  TEST_BEGIN("single_triangle_deterministic");

  MopVertex tri_verts[] = {
      {{-0.5f, -0.5f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, 0, 0},
      {{0.5f, -0.5f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}, 1, 0},
      {{0.0f, 0.5f, 0.0f}, {0, 0, 1}, {0, 0, 1, 1}, 0.5f, 1},
  };
  uint32_t tri_idx[] = {0, 1, 2};

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_clear_color(vp, (MopColor){0.1f, 0.1f, 0.1f, 1.0f});
  mop_viewport_set_camera(vp, (MopVec3){0, 0, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *mesh =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = tri_verts,
                                               .vertex_count = 3,
                                               .indices = tri_idx,
                                               .index_count = 3,
                                               .object_id = 1});
  TEST_ASSERT(mesh != NULL);

  /* Render 1 */
  mop_viewport_render(vp);
  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  uint64_t hash1 = hash_framebuffer(buf, w, h);

  /* Render 2 */
  mop_viewport_render(vp);
  buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  uint64_t hash2 = hash_framebuffer(buf, w, h);

  TEST_ASSERT_MSG(hash1 == hash2,
                  "single triangle hash differs across renders");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 3. Cube — fixed cube, camera, light dir
 *    Render twice. Same hash.
 * ========================================================================= */

static void test_cube_deterministic(void) {
  TEST_BEGIN("cube_deterministic");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){2, 2, 4}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_set_light_dir(vp, (MopVec3){0.3f, 1.0f, 0.5f});

  MopMesh *cube =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                               .vertex_count = 24,
                                               .indices = CUBE_IDX,
                                               .index_count = 36,
                                               .object_id = 1});
  TEST_ASSERT(cube != NULL);

  /* Render 1 */
  mop_viewport_render(vp);
  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  uint64_t hash1 = hash_framebuffer(buf, w, h);

  /* Render 2 */
  mop_viewport_render(vp);
  buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  uint64_t hash2 = hash_framebuffer(buf, w, h);

  TEST_ASSERT_MSG(hash1 == hash2, "cube hash differs across renders");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 4. Resize — start at 64x64, resize to 128x128, render twice at 128x128
 *    Both hashes at new size must match.
 * ========================================================================= */

static void test_resize_deterministic(void) {
  TEST_BEGIN("resize_deterministic");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                               .vertex_count = 24,
                                               .indices = CUBE_IDX,
                                               .index_count = 36,
                                               .object_id = 1});
  TEST_ASSERT(cube != NULL);

  /* Render once at 64x64 (warm up) */
  mop_viewport_render(vp);

  /* Resize to 128x128 */
  mop_viewport_resize(vp, 128, 128);

  /* Render 1 at new size */
  mop_viewport_render(vp);
  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  TEST_ASSERT(w == 128);
  TEST_ASSERT(h == 128);
  uint64_t hash1 = hash_framebuffer(buf, w, h);

  /* Render 2 at new size */
  mop_viewport_render(vp);
  buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  uint64_t hash2 = hash_framebuffer(buf, w, h);

  TEST_ASSERT_MSG(hash1 == hash2, "post-resize hash differs across renders");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 5. Multi-light — 3 lights (directional, point, spot), cube
 *    Render twice. Same hash.
 * ========================================================================= */

static void test_multilight_deterministic(void) {
  TEST_BEGIN("multilight_deterministic");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){2, 2, 4}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                               .vertex_count = 24,
                                               .indices = CUBE_IDX,
                                               .index_count = 36,
                                               .object_id = 1});
  TEST_ASSERT(cube != NULL);

  /* Clear default lights and add exactly 3 */
  mop_viewport_clear_lights(vp);

  MopLight dir = {.type = MOP_LIGHT_DIRECTIONAL,
                  .direction = {0.3f, 1.0f, 0.5f},
                  .color = {1, 1, 1, 1},
                  .intensity = 1.0f,
                  .active = true};
  MopLight *l0 = mop_viewport_add_light(vp, &dir);
  TEST_ASSERT(l0 != NULL);

  MopLight point = {.type = MOP_LIGHT_POINT,
                    .position = {3, 3, 3},
                    .color = {1, 0.9f, 0.8f, 1},
                    .intensity = 2.0f,
                    .range = 10.0f,
                    .active = true};
  MopLight *l1 = mop_viewport_add_light(vp, &point);
  TEST_ASSERT(l1 != NULL);

  MopLight spot = {.type = MOP_LIGHT_SPOT,
                   .position = {0, 5, 0},
                   .direction = {0, -1, 0},
                   .color = {0.8f, 0.8f, 1, 1},
                   .intensity = 1.5f,
                   .range = 15.0f,
                   .spot_inner_cos = 0.966f,
                   .spot_outer_cos = 0.866f,
                   .active = true};
  MopLight *l2 = mop_viewport_add_light(vp, &spot);
  TEST_ASSERT(l2 != NULL);

  /* Render 1 */
  mop_viewport_render(vp);
  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  uint64_t hash1 = hash_framebuffer(buf, w, h);

  /* Render 2 */
  mop_viewport_render(vp);
  buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  uint64_t hash2 = hash_framebuffer(buf, w, h);

  TEST_ASSERT_MSG(hash1 == hash2, "multi-light hash differs across renders");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 6. Transparency — opaque cube (id=1) at z=-3, transparent cube (id=2,
 *    opacity=0.5, blend=ALPHA) at z=-1.
 *    Render twice. Same hash.
 * ========================================================================= */

static void test_transparency_deterministic(void) {
  TEST_BEGIN("transparency_deterministic");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  /* Opaque cube at z=-3 (behind) */
  MopMesh *opaque =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                               .vertex_count = 24,
                                               .indices = CUBE_IDX,
                                               .index_count = 36,
                                               .object_id = 1});
  TEST_ASSERT(opaque != NULL);
  mop_mesh_set_position(opaque, (MopVec3){0, 0, -3});

  /* Transparent cube at z=-1 (in front) */
  MopMesh *transparent =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                               .vertex_count = 24,
                                               .indices = CUBE_IDX,
                                               .index_count = 36,
                                               .object_id = 2});
  TEST_ASSERT(transparent != NULL);
  mop_mesh_set_position(transparent, (MopVec3){0, 0, -1});
  mop_mesh_set_opacity(transparent, 0.5f);
  mop_mesh_set_blend_mode(transparent, MOP_BLEND_ALPHA);

  /* Render 1 */
  mop_viewport_render(vp);
  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  uint64_t hash1 = hash_framebuffer(buf, w, h);

  /* Render 2 */
  mop_viewport_render(vp);
  buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  uint64_t hash2 = hash_framebuffer(buf, w, h);

  TEST_ASSERT_MSG(hash1 == hash2, "transparency hash differs across renders");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
  TEST_SUITE_BEGIN("torture_golden");

  TEST_RUN(test_empty_scene_deterministic);
  TEST_RUN(test_single_triangle_deterministic);
  TEST_RUN(test_cube_deterministic);
  TEST_RUN(test_resize_deterministic);
  TEST_RUN(test_multilight_deterministic);
  TEST_RUN(test_transparency_deterministic);

  TEST_REPORT();
  TEST_EXIT();
}
