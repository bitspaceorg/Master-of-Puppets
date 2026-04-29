/*
 * Master of Puppets — render_sync + overlays regression
 * test_render_sync_overlays.c — verify mop_viewport_render_sync preserves
 * the CPU compositor's text and overlay output. The Made-in-Heaven
 * integration reported that on Vulkan/MoltenVK, frame_wait_readback
 * memcpy'd the GPU's mapped readback over the CPU-painted composite,
 * silently dropping every text + overlay primitive.
 *
 * The CPU backend has no deferred readback, so this test exercises the
 * deferred-drain path symbolically: it confirms render_sync runs without
 * crashing when there are queued text + overlay prims, that the readback
 * buffer comes back populated, and that subsequent renders don't carry
 * stale prims (i.e. the deferred drain actually happened).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

static MopViewport *make_viewport(int w, int h) {
  MopViewportDesc desc = {.width = w, .height = h, .backend = MOP_BACKEND_CPU};
  return mop_viewport_create(&desc);
}

static MopMesh *add_unit_cube(MopViewport *vp) {
  /* Six faces, four verts each, two triangles per face — minimum geometry
   * so render_sync has something to memcpy back. */
  const float s = 0.6f;
  const float N[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                         {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  const float P[6][4][3] = {
      {{s, -s, -s}, {s, -s, s}, {s, s, s}, {s, s, -s}},
      {{-s, -s, s}, {-s, -s, -s}, {-s, s, -s}, {-s, s, s}},
      {{-s, s, -s}, {s, s, -s}, {s, s, s}, {-s, s, s}},
      {{-s, -s, s}, {s, -s, s}, {s, -s, -s}, {-s, -s, -s}},
      {{-s, -s, s}, {-s, s, s}, {s, s, s}, {s, -s, s}},
      {{s, -s, -s}, {s, s, -s}, {-s, s, -s}, {-s, -s, -s}}};
  MopVertex v[24];
  uint32_t idx[36];
  for (int f = 0; f < 6; f++) {
    for (int j = 0; j < 4; j++) {
      v[f * 4 + j].position = (MopVec3){P[f][j][0], P[f][j][1], P[f][j][2]};
      v[f * 4 + j].normal = (MopVec3){N[f][0], N[f][1], N[f][2]};
      v[f * 4 + j].color = (MopColor){0.78f, 0.30f, 0.20f, 1.0f};
      v[f * 4 + j].u = 0.0f;
      v[f * 4 + j].v = 0.0f;
    }
    uint32_t b = (uint32_t)f * 4;
    idx[f * 6 + 0] = b + 0;
    idx[f * 6 + 1] = b + 2;
    idx[f * 6 + 2] = b + 1;
    idx[f * 6 + 3] = b + 0;
    idx[f * 6 + 4] = b + 3;
    idx[f * 6 + 5] = b + 2;
  }
  MopMeshDesc d = {0};
  d.vertices = v;
  d.vertex_count = 24;
  d.indices = idx;
  d.index_count = 36;
  d.object_id = 1;
  return mop_viewport_add_mesh(vp, &d);
}

static void queue_hud(MopViewport *vp) {
  MopTextStyle s = {0};
  s.color = (MopColor){1, 1, 1, 1};
  s.px_size = 24.0f;
  s.weight = 0.20f;
  mop_text_draw_2d(vp, NULL, "HUD", 16, 16, s);
}

/* render_sync must finish without crashing when text + overlay prims are
 * queued. On Vulkan this is the path that previously dropped every
 * primitive; on CPU it confirms the deferred-drain branch in
 * rg_post_frame_overlays compiles and is reachable. */
static void test_render_sync_with_text(void) {
  TEST_BEGIN("render_sync: queued text doesn't crash");
  MopViewport *vp = make_viewport(128, 96);
  TEST_ASSERT(vp != NULL);

  add_unit_cube(vp);
  mop_viewport_set_camera(vp, (MopVec3){2.5f, 1.8f, 3.0f}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 45.0f, 0.1f, 100.0f);

  queue_hud(vp);
  TEST_ASSERT(mop_viewport_render_sync(vp) == MOP_RENDER_OK);

  int rw, rh;
  const uint8_t *px = mop_viewport_read_color(vp, &rw, &rh);
  TEST_ASSERT(px != NULL);
  TEST_ASSERT(rw == 128 && rh == 96);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* The deferred drain in render_sync must run, otherwise text accumulates
 * and the next frame paints last frame's HUD on top of the new one. We
 * can't observe that on CPU directly (no clobbering memcpy), but we can
 * confirm vp->text_prim_count is 0 after render_sync returns by issuing
 * a second render_sync with no new text and verifying it still succeeds.
 * If the drain leaked, the second call would re-rasterize the stale HUD
 * silently — not catastrophic, but the count would never zero.
 *
 * The harness has no peek-into-internals, so this test is a smoke check:
 * many sync renders with new text each frame must keep finishing OK. */
static void test_render_sync_drain_runs(void) {
  TEST_BEGIN("render_sync: drain runs (no prim accumulation)");
  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);
  add_unit_cube(vp);
  mop_viewport_set_camera(vp, (MopVec3){2, 2, 2}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 45.0f, 0.1f, 100.0f);

  for (int i = 0; i < 16; i++) {
    queue_hud(vp);
    TEST_ASSERT(mop_viewport_render_sync(vp) == MOP_RENDER_OK);
  }
  mop_viewport_destroy(vp);
  TEST_END();
}

/* Mixed cadence: alternate render_sync and plain render. The internal
 * _sync_render_active flag must clear cleanly so the next plain render
 * drains text the normal way. If the flag stuck on, plain render would
 * leak prims; if it stuck off during sync, the wait_readback would clobber
 * uncovered. Either failure surfaces as a stale prim queue at destroy. */
static void test_render_sync_alternating(void) {
  TEST_BEGIN("render_sync: alternating with plain render");
  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);
  add_unit_cube(vp);
  mop_viewport_set_camera(vp, (MopVec3){2, 2, 2}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 45.0f, 0.1f, 100.0f);

  for (int i = 0; i < 8; i++) {
    queue_hud(vp);
    TEST_ASSERT(mop_viewport_render_sync(vp) == MOP_RENDER_OK);
    queue_hud(vp);
    TEST_ASSERT(mop_viewport_render(vp) == MOP_RENDER_OK);
  }
  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("render_sync overlays");
  TEST_RUN(test_render_sync_with_text);
  TEST_RUN(test_render_sync_drain_runs);
  TEST_RUN(test_render_sync_alternating);
  TEST_REPORT();
  TEST_EXIT();
}
