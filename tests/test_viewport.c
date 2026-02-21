/*
 * Master of Puppets — Viewport Tests
 * test_viewport.c — Lifecycle, resize, mesh management, render, picking
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* Minimal triangle for testing */
static const MopVertex TRI_VERTS[] = {
    {{0.0f, 0.5f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}},
    {{-0.5f, -0.5f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}},
    {{0.5f, -0.5f, 0.0f}, {0, 0, 1}, {0, 0, 1, 1}},
};
static const uint32_t TRI_IDX[] = {0, 1, 2};

/* Cube geometry for pick testing */
static const MopVertex CUBE_VERTS[] = {
    /* Front face (z=+0.5) */
    {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    /* Back face (z=-0.5) */
    {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    /* Top face (y=+0.5) */
    {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    /* Bottom face (y=-0.5) */
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    /* Right face (x=+0.5) */
    {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    /* Left face (x=-0.5) */
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
};
static const uint32_t CUBE_IDX[] = {
    0,  1,  2,  2,  3,  0,  /* front */
    4,  5,  6,  6,  7,  4,  /* back */
    8,  9,  10, 10, 11, 8,  /* top */
    12, 13, 14, 14, 15, 12, /* bottom */
    16, 17, 18, 18, 19, 16, /* right */
    20, 21, 22, 22, 23, 20, /* left */
};

static void test_create_destroy(void) {
  TEST_BEGIN("viewport_create_destroy");
  MopViewportDesc desc = {
      .width = 320, .height = 240, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_create_null_desc(void) {
  TEST_BEGIN("viewport_create_null_desc");
  MopViewport *vp = mop_viewport_create(NULL);
  TEST_ASSERT(vp == NULL);
  TEST_END();
}

static void test_create_zero_size(void) {
  TEST_BEGIN("viewport_create_zero_size");
  MopViewportDesc desc = {
      .width = 0, .height = 240, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp == NULL);
  TEST_END();
}

static void test_resize(void) {
  TEST_BEGIN("viewport_resize");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);
  mop_viewport_resize(vp, 128, 128);
  mop_viewport_render(vp);
  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  TEST_ASSERT(w == 128);
  TEST_ASSERT(h == 128);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_add_remove_mesh(void) {
  TEST_BEGIN("viewport_add_remove_mesh");
  MopViewportDesc vd = {.width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&vd);
  TEST_ASSERT(vp != NULL);
  MopMeshDesc md = {.vertices = TRI_VERTS,
                    .vertex_count = 3,
                    .indices = TRI_IDX,
                    .index_count = 3,
                    .object_id = 42};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &md);
  TEST_ASSERT(mesh != NULL);
  mop_viewport_remove_mesh(vp, mesh);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_render_non_null(void) {
  TEST_BEGIN("viewport_render_returns_non_null");
  MopViewportDesc vd = {.width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&vd);
  mop_viewport_render(vp);
  int w, h;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  TEST_ASSERT(w == 64);
  TEST_ASSERT(h == 64);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_pick_cube(void) {
  TEST_BEGIN("viewport_pick_cube_center");
  MopViewportDesc vd = {
      .width = 128, .height = 128, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&vd);
  TEST_ASSERT(vp != NULL);

  /* Place camera looking straight at origin */
  mop_viewport_set_camera(vp, (MopVec3){0, 0, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMeshDesc md = {.vertices = CUBE_VERTS,
                    .vertex_count = 24,
                    .indices = CUBE_IDX,
                    .index_count = 36,
                    .object_id = 1};
  MopMesh *cube = mop_viewport_add_mesh(vp, &md);
  TEST_ASSERT(cube != NULL);

  mop_viewport_render(vp);

  /* Pick center of framebuffer — should hit the cube */
  MopPickResult pick = mop_viewport_pick(vp, 64, 64);
  TEST_ASSERT(pick.hit == true);
  TEST_ASSERT(pick.object_id == 1);
  TEST_ASSERT(pick.depth >= 0.0f && pick.depth <= 1.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_pick_empty(void) {
  TEST_BEGIN("viewport_pick_empty_space");
  MopViewportDesc vd = {.width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&vd);
  mop_viewport_set_camera(vp, (MopVec3){0, 100, 0}, (MopVec3){0, 100, -1},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_render(vp);
  /* No mesh at all — pick should miss */
  MopPickResult pick = mop_viewport_pick(vp, 32, 32);
  /* Grid may or may not be hit here, but object_id should be 0 (non-pickable)
   */
  if (pick.hit) {
    TEST_ASSERT(pick.object_id == 0);
  }
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_pick_out_of_bounds(void) {
  TEST_BEGIN("viewport_pick_out_of_bounds");
  MopViewportDesc vd = {.width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&vd);
  mop_viewport_render(vp);
  MopPickResult pick = mop_viewport_pick(vp, -1, -1);
  TEST_ASSERT(pick.hit == false);
  pick = mop_viewport_pick(vp, 200, 200);
  TEST_ASSERT(pick.hit == false);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_backend_query(void) {
  TEST_BEGIN("viewport_get_backend");
  MopViewportDesc vd = {.width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&vd);
  TEST_ASSERT(mop_viewport_get_backend(vp) == MOP_BACKEND_CPU);
  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("viewport");

  TEST_RUN(test_create_destroy);
  TEST_RUN(test_create_null_desc);
  TEST_RUN(test_create_zero_size);
  TEST_RUN(test_resize);
  TEST_RUN(test_add_remove_mesh);
  TEST_RUN(test_render_non_null);
  TEST_RUN(test_pick_cube);
  TEST_RUN(test_pick_empty);
  TEST_RUN(test_pick_out_of_bounds);
  TEST_RUN(test_backend_query);

  TEST_REPORT();
  TEST_EXIT();
}
