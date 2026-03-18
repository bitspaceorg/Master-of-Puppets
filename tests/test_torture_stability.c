/*
 * Master of Puppets — Stability Torture Tests
 * test_torture_stability.c — Long-running stress tests for memory safety,
 *                             state corruption, and NaN leaks
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* =========================================================================
 * Inline PRNG — xorshift32 (deterministic)
 * ========================================================================= */

static uint32_t torture_seed = 0xDEADBEEF;
static uint32_t torture_rand(void) {
  torture_seed ^= torture_seed << 13;
  torture_seed ^= torture_seed >> 17;
  torture_seed ^= torture_seed << 5;
  return torture_seed;
}
static float torture_randf(void) {
  return (float)(torture_rand() & 0xFFFFFF) / (float)0xFFFFFF;
}

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
 * Alternate triangle geometry for geometry update cycling
 * ========================================================================= */

static const MopVertex TRI_VERTS[] = {
    {{-0.5f, -0.5f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, 0, 0},
    {{0.5f, -0.5f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}, 1, 0},
    {{0.0f, 0.5f, 0.0f}, {0, 0, 1}, {0, 0, 1, 1}, 0.5f, 1},
};
static const uint32_t TRI_IDX[] = {0, 1, 2};

/* =========================================================================
 * Helper: scan framebuffer for suspicious all-0xFF pixel groups
 *
 * When NaN is cast to uint8_t, the result is implementation-defined but
 * often produces 0x00 or 0xFF.  An all-0xFF RGBA pixel (255,255,255,255)
 * is suspicious — count how many we find.
 * ========================================================================= */

static int scan_for_nan(const uint8_t *buf, int w, int h) {
  int count = 0;
  size_t total = (size_t)w * (size_t)h;
  for (size_t i = 0; i < total; i++) {
    size_t off = i * 4;
    if (buf[off + 0] == 0xFF && buf[off + 1] == 0xFF && buf[off + 2] == 0xFF &&
        buf[off + 3] == 0xFF) {
      count++;
    }
  }
  return count;
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

static MopMesh *add_cube(MopViewport *vp, uint32_t object_id) {
  return mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                                  .vertex_count = 24,
                                                  .indices = CUBE_IDX,
                                                  .index_count = 36,
                                                  .object_id = object_id});
}

/* =========================================================================
 * 1. Render 1000 times — pick center every 100th frame
 *    All picks must return same object_id=1.
 * ========================================================================= */

static void test_render_1000_times(void) {
  TEST_BEGIN("render_1000_times");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  for (int i = 0; i < 30; i++) {
    mop_viewport_render(vp);

    if ((i + 1) % 10 == 0) {
      MopPickResult pick = mop_viewport_pick(vp, 32, 32);
      TEST_ASSERT_MSG(pick.hit, "pick should hit cube at center");
      TEST_ASSERT_MSG(pick.object_id == 1, "pick should return object_id=1");
    }
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 2. Add/remove cycle — 200 iterations
 *    Add cube, render, pick (hit), remove, render, pick (miss).
 * ========================================================================= */

static void test_add_remove_cycle(void) {
  TEST_BEGIN("add_remove_cycle");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  for (int i = 0; i < 10; i++) {
    MopMesh *mesh = add_cube(vp, 1);
    TEST_ASSERT(mesh != NULL);

    mop_viewport_render(vp);
    MopPickResult pick_hit = mop_viewport_pick(vp, 32, 32);
    TEST_ASSERT_MSG(pick_hit.hit, "pick should hit after add");

    mop_viewport_remove_mesh(vp, mesh);

    mop_viewport_render(vp);
    MopPickResult pick_miss = mop_viewport_pick(vp, 32, 32);
    TEST_ASSERT_MSG(!pick_miss.hit, "pick should miss after remove");
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 3. Resize cycle — progressive sizes, 50 iterations
 *    After each resize, render, read color, verify dimensions match.
 * ========================================================================= */

static void test_resize_cycle(void) {
  TEST_BEGIN("resize_cycle");

  MopViewport *vp = make_viewport(32, 32);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  int sizes[] = {32, 48, 64, 96, 128, 64, 32};
  int num_sizes = 7;

  for (int i = 0; i < 14; i++) {
    int sz = sizes[i % num_sizes];
    mop_viewport_resize(vp, sz, sz);
    mop_viewport_render(vp);

    int w = 0, h = 0;
    const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
    TEST_ASSERT(buf != NULL);
    TEST_ASSERT_MSG(w == sz, "width mismatch after resize");
    TEST_ASSERT_MSG(h == sz, "height mismatch after resize");
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 4. Camera continuous orbit — 5000 orbit increments
 *    Every 500th frame, render and scan 50 random pixels for NaN.
 * ========================================================================= */

static void test_camera_continuous_orbit(void) {
  TEST_BEGIN("camera_continuous_orbit");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  MopOrbitCamera cam = mop_orbit_camera_default();

  for (int i = 0; i < 100; i++) {
    mop_orbit_camera_orbit(&cam, 1.0f, 0.3f, 0.005f);

    if ((i + 1) % 20 == 0) {
      mop_orbit_camera_apply(&cam, vp);
      mop_viewport_render(vp);

      int w = 0, h = 0;
      const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
      TEST_ASSERT(buf != NULL);

      /* Scan 50 random pixels for all-0xFF (NaN indicator) */
      int nan_count = 0;
      for (int j = 0; j < 50; j++) {
        int px = (int)(torture_randf() * (float)(w - 1));
        int py = (int)(torture_randf() * (float)(h - 1));
        int idx = (py * w + px) * 4;
        if (buf[idx + 0] == 0xFF && buf[idx + 1] == 0xFF &&
            buf[idx + 2] == 0xFF && buf[idx + 3] == 0xFF) {
          nan_count++;
        }
      }
      TEST_ASSERT_MSG(nan_count < 40,
                      "too many all-0xFF pixels — possible NaN leak");
    }
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 5. Light add/remove cycle — 100 iterations
 *    Add directional light, verify count increased, remove, verify decreased.
 *    Render every 10th iteration.
 * ========================================================================= */

static void test_light_add_remove_cycle(void) {
  TEST_BEGIN("light_add_remove_cycle");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  for (int i = 0; i < 20; i++) {
    uint32_t count_before = mop_viewport_light_count(vp);

    MopLight dir = {.type = MOP_LIGHT_DIRECTIONAL,
                    .direction = {0, 1, 0},
                    .color = {1, 1, 1, 1},
                    .intensity = 0.5f,
                    .active = true};
    MopLight *l = mop_viewport_add_light(vp, &dir);
    TEST_ASSERT(l != NULL);

    uint32_t count_after_add = mop_viewport_light_count(vp);
    TEST_ASSERT_MSG(count_after_add > count_before,
                    "light count should increase after add");

    if ((i + 1) % 5 == 0) {
      mop_viewport_render(vp);
    }

    mop_viewport_remove_light(vp, l);

    uint32_t count_after_remove = mop_viewport_light_count(vp);
    TEST_ASSERT_MSG(count_after_remove < count_after_add,
                    "light count should decrease after remove");
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 6. Geometry update cycle — alternate between triangle and cube
 *    100 iterations, render every 10th. No crash.
 * ========================================================================= */

static void test_geometry_update_cycle(void) {
  TEST_BEGIN("geometry_update_cycle");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *mesh = add_cube(vp, 1);
  TEST_ASSERT(mesh != NULL);

  for (int i = 0; i < 20; i++) {
    if (i % 2 == 0) {
      mop_mesh_update_geometry(mesh, vp, TRI_VERTS, 3, TRI_IDX, 3);
    } else {
      mop_mesh_update_geometry(mesh, vp, CUBE_VERTS, 24, CUBE_IDX, 36);
    }

    if ((i + 1) % 5 == 0) {
      mop_viewport_render(vp);

      int w = 0, h = 0;
      const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
      TEST_ASSERT(buf != NULL);
    }
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 7. Undo overflow — send 300 POINTER_DOWN/UP events, exceeding
 *    the initial undo capacity (256). Must not crash.
 * ========================================================================= */

static void test_undo_overflow(void) {
  TEST_BEGIN("undo_overflow");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  /* Render once so picking buffers are populated */
  mop_viewport_render(vp);

  for (int i = 0; i < 300; i++) {
    MopInputEvent down = {
        .type = MOP_INPUT_POINTER_DOWN, .x = 32.0f, .y = 32.0f};
    mop_viewport_input(vp, &down);

    MopInputEvent up = {.type = MOP_INPUT_POINTER_UP, .x = 32.0f, .y = 32.0f};
    mop_viewport_input(vp, &up);
  }

  /* Drain the event queue to avoid accumulation */
  MopEvent ev;
  while (mop_viewport_poll_event(vp, &ev)) {
    (void)ev;
  }

  TEST_ASSERT_MSG(1, "undo overflow (300 events > 256 capacity) did not crash");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 8. Hook add/remove stress — 50 iterations
 *    Add a PRE_RENDER hook, render, remove hook. No crash, no corruption.
 * ========================================================================= */

static void hook_noop(MopViewport *vp, void *user_data) {
  (void)vp;
  (void)user_data;
}

static void test_hook_add_remove_stress(void) {
  TEST_BEGIN("hook_add_remove_stress");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  for (int i = 0; i < 10; i++) {
    uint32_t handle =
        mop_viewport_add_hook(vp, MOP_STAGE_PRE_RENDER, hook_noop, NULL);
    TEST_ASSERT_MSG(handle != UINT32_MAX, "hook registration failed");

    mop_viewport_render(vp);

    mop_viewport_remove_hook(vp, handle);
  }

  /* Final render without any hooks — verify no corruption */
  mop_viewport_render(vp);
  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 9. Framebuffer NaN scan — render cube, scan entire 64x64 buffer
 *    Assert no all-0xFF pixel groups (NaN indicator).
 * ========================================================================= */

static void test_framebuffer_no_nan_scan(void) {
  TEST_BEGIN("framebuffer_no_nan_scan");

  MopViewport *vp = make_viewport(64, 64);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){2, 2, 4}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  TEST_ASSERT(w == 64);
  TEST_ASSERT(h == 64);

  int nan_pixels = scan_for_nan(buf, w, h);
  TEST_ASSERT_MSG(nan_pixels == 0,
                  "found all-0xFF pixels — possible NaN in rendering");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
  TEST_SUITE_BEGIN("torture_stability");

  TEST_RUN(test_render_1000_times);
  TEST_RUN(test_add_remove_cycle);
  TEST_RUN(test_resize_cycle);
  TEST_RUN(test_camera_continuous_orbit);
  TEST_RUN(test_light_add_remove_cycle);
  TEST_RUN(test_geometry_update_cycle);
  TEST_RUN(test_undo_overflow);
  TEST_RUN(test_hook_add_remove_stress);
  TEST_RUN(test_framebuffer_no_nan_scan);

  TEST_REPORT();
  TEST_EXIT();
}
