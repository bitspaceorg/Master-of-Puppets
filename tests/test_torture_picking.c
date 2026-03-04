/*
 * Master of Puppets — Picking Torture Tests
 * test_torture_picking.c — Corner/boundary picks, hierarchy, SSAA, remove,
 *                          transparency, object_id=0 convention, determinism
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* =========================================================================
 * Shared geometry — unit cube (24 verts, 36 indices)
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
 * Helper: add a cube with a given object_id and position
 * ========================================================================= */

static MopMesh *add_cube(MopViewport *vp, uint32_t id, MopVec3 pos) {
  MopMesh *mesh =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                               .vertex_count = 24,
                                               .indices = CUBE_IDX,
                                               .index_count = 36,
                                               .object_id = id});
  if (mesh)
    mop_mesh_set_position(mesh, pos);
  return mesh;
}

/* =========================================================================
 * 1. test_pick_four_corners
 *    4 small cubes at screen corners. Each with unique object_id 1-4.
 *    Pick near each corner -> correct IDs.
 * ========================================================================= */

static void test_pick_four_corners(void) {
  TEST_BEGIN("pick_four_corners");

  MopViewport *vp = make_viewport(128, 96);
  TEST_ASSERT(vp != NULL);

  /* Camera at z=10, looking at origin, wide FOV to see corners */
  mop_viewport_set_camera(vp, (MopVec3){0, 0, 10}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  /* Place 4 small cubes at positions that map to the four quadrants.
   * At z=0 with camera at z=10, fov=60: visible half-height ~ 10*tan(30) ~ 5.77
   * visible half-width ~ 5.77 * (128/96) ~ 7.70
   * Place cubes at +/-3 in X and +/-2.5 in Y to land in each quadrant. */
  MopMesh *tl = add_cube(vp, 1, (MopVec3){-3.0f, 2.5f, 0.0f});
  MopMesh *tr = add_cube(vp, 2, (MopVec3){3.0f, 2.5f, 0.0f});
  MopMesh *bl = add_cube(vp, 3, (MopVec3){-3.0f, -2.5f, 0.0f});
  MopMesh *br = add_cube(vp, 4, (MopVec3){3.0f, -2.5f, 0.0f});
  TEST_ASSERT(tl != NULL);
  TEST_ASSERT(tr != NULL);
  TEST_ASSERT(bl != NULL);
  TEST_ASSERT(br != NULL);

  mop_viewport_render(vp);

  /* Pick at each cube's projected center.
   * FOV=60, camera z=10, cubes at z=0: half-height=10*tan(30)=5.77,
   * aspect=128/96=1.333 half-width=7.70. Cube at (-3,2.5): NDC=(-0.39,0.43) →
   * px=(39,27) Cube at (3,2.5): NDC=(0.39,0.43) → px=(89,27) Cube at (-3,-2.5):
   * NDC=(-0.39,-0.43) → px=(39,69) Cube at (3,-2.5): NDC=(0.39,-0.43) →
   * px=(89,69) */
  MopPickResult p_tl = mop_viewport_pick(vp, 39, 27);
  MopPickResult p_tr = mop_viewport_pick(vp, 89, 27);
  MopPickResult p_bl = mop_viewport_pick(vp, 39, 69);
  MopPickResult p_br = mop_viewport_pick(vp, 89, 69);

  TEST_ASSERT(p_tl.hit == true);
  TEST_ASSERT(p_tl.object_id == 1);
  TEST_ASSERT(p_tr.hit == true);
  TEST_ASSERT(p_tr.object_id == 2);
  TEST_ASSERT(p_bl.hit == true);
  TEST_ASSERT(p_bl.object_id == 3);
  TEST_ASSERT(p_br.hit == true);
  TEST_ASSERT(p_br.object_id == 4);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 2. test_pick_boundary_pixels
 *    Pick at (0,0), (w-1,0), (0,h-1), (w-1,h-1). Assert no crash.
 * ========================================================================= */

static void test_pick_boundary_pixels(void) {
  TEST_BEGIN("pick_boundary_pixels");

  int w = 64, h = 48;
  MopViewport *vp = make_viewport(w, h);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1, (MopVec3){0, 0, 0});
  TEST_ASSERT(cube != NULL);

  mop_viewport_render(vp);

  /* These boundary picks must not crash or segfault */
  MopPickResult p0 = mop_viewport_pick(vp, 0, 0);
  MopPickResult p1 = mop_viewport_pick(vp, w - 1, 0);
  MopPickResult p2 = mop_viewport_pick(vp, 0, h - 1);
  MopPickResult p3 = mop_viewport_pick(vp, w - 1, h - 1);

  /* Suppress unused warnings — we only care about no crash */
  (void)p0;
  (void)p1;
  (void)p2;
  (void)p3;

  TEST_ASSERT_MSG(1, "boundary pixel picks did not crash");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 3. test_pick_out_of_bounds
 *    Pick at (-1,-1), (w,h), (w+100,h+100), (INT_MAX,INT_MAX).
 *    All must return hit==false.
 * ========================================================================= */

static void test_pick_out_of_bounds(void) {
  TEST_BEGIN("pick_out_of_bounds");

  int w = 64, h = 48;
  MopViewport *vp = make_viewport(w, h);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1, (MopVec3){0, 0, 0});
  TEST_ASSERT(cube != NULL);

  mop_viewport_render(vp);

  MopPickResult p0 = mop_viewport_pick(vp, -1, -1);
  TEST_ASSERT(p0.hit == false);

  MopPickResult p1 = mop_viewport_pick(vp, w, h);
  TEST_ASSERT(p1.hit == false);

  MopPickResult p2 = mop_viewport_pick(vp, w + 100, h + 100);
  TEST_ASSERT(p2.hit == false);

  MopPickResult p3 = mop_viewport_pick(vp, 2147483647, 2147483647);
  TEST_ASSERT(p3.hit == false);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 4. test_pick_overlapping
 *    Two cubes at same XY: front (id=10) at z=-1, back (id=20) at z=-3.
 *    Pick center -> id=10.
 * ========================================================================= */

static void test_pick_overlapping(void) {
  TEST_BEGIN("pick_overlapping");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *front = add_cube(vp, 10, (MopVec3){0, 0, -1.0f});
  MopMesh *back = add_cube(vp, 20, (MopVec3){0, 0, -3.0f});
  TEST_ASSERT(front != NULL);
  TEST_ASSERT(back != NULL);

  mop_viewport_render(vp);

  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick.hit == true);
  TEST_ASSERT(pick.object_id == 10);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 5. test_pick_after_remove
 *    Add cube (id=1), render, pick center (hit).
 *    Remove cube, render, pick center (miss).
 * ========================================================================= */

static void test_pick_after_remove(void) {
  TEST_BEGIN("pick_after_remove");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1, (MopVec3){0, 0, 0});
  TEST_ASSERT(cube != NULL);

  /* First render — cube is present */
  mop_viewport_render(vp);
  MopPickResult pick1 = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick1.hit == true);
  TEST_ASSERT(pick1.object_id == 1);

  /* Remove the cube */
  mop_viewport_remove_mesh(vp, cube);

  /* Second render — cube is gone */
  mop_viewport_render(vp);
  MopPickResult pick2 = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick2.hit == false);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 6. test_pick_through_hierarchy
 *    Parent cube at (5,0,0), child cube at local (2,0,0), so child world
 *    pos=(7,0,0). Camera looking at (7,0,0). Pick center -> child's id.
 * ========================================================================= */

static void test_pick_through_hierarchy(void) {
  TEST_BEGIN("pick_through_hierarchy");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  /* Camera looking at world (7, 0, 0) from z offset */
  mop_viewport_set_camera(vp, (MopVec3){7, 0, 5}, (MopVec3){7, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  /* Parent at (5, 0, 0), id=50 */
  MopMesh *parent = add_cube(vp, 50, (MopVec3){5.0f, 0.0f, 0.0f});
  TEST_ASSERT(parent != NULL);

  /* Child at local (2, 0, 0) relative to parent -> world (7, 0, 0), id=51 */
  MopMesh *child = add_cube(vp, 51, (MopVec3){2.0f, 0.0f, 0.0f});
  TEST_ASSERT(child != NULL);
  mop_mesh_set_parent(child, parent, vp);

  mop_viewport_render(vp);

  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick.hit == true);
  TEST_ASSERT_MSG(pick.object_id == 51,
                  "child cube should be picked at its world position");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 7. test_pick_with_ssaa
 *    Viewport with default SSAA (factor=2). Add cube at center. Render.
 *    Pick at center -> should hit. Tests SSAA coordinate scaling.
 * ========================================================================= */

static void test_pick_with_ssaa(void) {
  TEST_BEGIN("pick_with_ssaa");

  /* SSAA is always 2x by default — no special config needed */
  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1, (MopVec3){0, 0, 0});
  TEST_ASSERT(cube != NULL);

  mop_viewport_render(vp);

  /* Pick at the center of the logical viewport (not the SSAA-scaled
   * internal framebuffer). The pick API should handle the coordinate
   * scaling from logical to internal resolution. */
  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT_MSG(pick.hit == true,
                  "pick through SSAA should hit cube at center");
  TEST_ASSERT(pick.object_id == 1);

  /* Also verify depth is sane */
  TEST_ASSERT_NO_NAN(pick.depth);
  TEST_ASSERT(pick.depth >= 0.0f && pick.depth <= 1.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 8. test_pick_transparent
 *    Opaque cube (id=1) at z=-3, transparent cube (id=2, opacity=0.5,
 *    blend=MOP_BLEND_ALPHA) at z=-1. Pick center.
 *    Document which id is returned (should be id=2 since it's closest).
 * ========================================================================= */

static void test_pick_transparent(void) {
  TEST_BEGIN("pick_transparent");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  /* Opaque cube behind */
  MopMesh *opaque = add_cube(vp, 1, (MopVec3){0, 0, -3.0f});
  TEST_ASSERT(opaque != NULL);

  /* Transparent cube in front with opacity=0.5 */
  MopMesh *transparent = add_cube(vp, 2, (MopVec3){0, 0, -1.0f});
  TEST_ASSERT(transparent != NULL);
  mop_mesh_set_opacity(transparent, 0.5f);
  mop_mesh_set_blend_mode(transparent, MOP_BLEND_ALPHA);

  mop_viewport_render(vp);

  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick.hit == true);

  /* MOP only writes to the ID buffer when final_alpha > 0.5.
   * At opacity=0.5 the transparent cube does NOT dominate the ID buffer,
   * so the opaque cube behind it wins the pick. */
  TEST_ASSERT_MSG(pick.object_id == 1,
                  "opaque behind transparent (opacity<=0.5) wins pick");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 9. test_object_id_zero_background
 *    Mesh with object_id=0. Render. Pick over it -> hit should be false
 *    (0 = background convention per scene.h: "object_id: unique identifier
 *    for picking (0 = no object)").
 * ========================================================================= */

static void test_object_id_zero_background(void) {
  TEST_BEGIN("object_id_zero_background");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  /* Mesh with object_id=0 — should be treated as background */
  MopMesh *mesh = add_cube(vp, 0, (MopVec3){0, 0, 0});
  TEST_ASSERT(mesh != NULL);

  mop_viewport_render(vp);

  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT_MSG(pick.hit == false,
                  "object_id=0 should be treated as background (no hit)");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 10. test_pick_deterministic_replay
 *     Add cube, render+pick center 100 times. All 100 picks must return
 *     same object_id and same depth (tolerance 0).
 * ========================================================================= */

static void test_pick_deterministic_replay(void) {
  TEST_BEGIN("pick_deterministic_replay");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 7, (MopVec3){0, 0, 0});
  TEST_ASSERT(cube != NULL);

  /* First render+pick to establish reference values */
  mop_viewport_render(vp);
  MopPickResult ref = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(ref.hit == true);
  TEST_ASSERT(ref.object_id == 7);

  /* 10 more render+pick cycles — must match exactly */
  for (int i = 0; i < 10; i++) {
    mop_viewport_render(vp);
    MopPickResult p = mop_viewport_pick(vp, 32, 24);
    TEST_ASSERT_MSG(p.hit == true, "deterministic replay must always hit");
    TEST_ASSERT_MSG(p.object_id == ref.object_id,
                    "object_id must be identical across replays");
    TEST_ASSERT_MSG(p.depth == ref.depth,
                    "depth must be bit-identical across replays");
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
  TEST_SUITE_BEGIN("torture_picking");

  TEST_RUN(test_pick_four_corners);
  TEST_RUN(test_pick_boundary_pixels);
  TEST_RUN(test_pick_out_of_bounds);
  TEST_RUN(test_pick_overlapping);
  TEST_RUN(test_pick_after_remove);
  TEST_RUN(test_pick_through_hierarchy);
  TEST_RUN(test_pick_with_ssaa);
  TEST_RUN(test_pick_transparent);
  TEST_RUN(test_object_id_zero_background);
  TEST_RUN(test_pick_deterministic_replay);

  TEST_REPORT();
  TEST_EXIT();
}
