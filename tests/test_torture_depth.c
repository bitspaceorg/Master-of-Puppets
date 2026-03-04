/*
 * Master of Puppets — Depth Buffer Torture Tests
 * test_torture_depth.c — Depth ordering, precision, near/far extremes,
 *                        z-fighting, large world coords, clear value
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* =========================================================================
 * Shared geometry — full-screen-ish quad (Z-facing camera)
 *
 * A quad in the XY plane at z=0. Tests position it along Z via
 * mop_mesh_set_position. Normal faces +Z (toward camera at z>0).
 * ========================================================================= */

static const MopVertex QUAD_VERTS[] = {
    {{-1.0f, -1.0f, 0.0f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{1.0f, -1.0f, 0.0f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{1.0f, 1.0f, 0.0f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{-1.0f, 1.0f, 0.0f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
};
static const uint32_t QUAD_IDX[] = {
    0, 1, 2, 2, 3, 0,
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
 * Helper: add a quad mesh at a given Z position with a given object_id
 * ========================================================================= */

static MopMesh *add_quad_at_z(MopViewport *vp, float z, uint32_t id) {
  MopMesh *mesh =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = QUAD_VERTS,
                                               .vertex_count = 4,
                                               .indices = QUAD_IDX,
                                               .index_count = 6,
                                               .object_id = id});
  if (mesh)
    mop_mesh_set_position(mesh, (MopVec3){0.0f, 0.0f, z});
  return mesh;
}

/* =========================================================================
 * 1. test_depth_order_two_planes
 *    Front quad at z=-1 (id=1), back at z=-2 (id=2).
 *    Camera at z=0 looking -Z. Pick center -> id=1.
 * ========================================================================= */

static void test_depth_order_two_planes(void) {
  TEST_BEGIN("depth_order_two_planes");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 0}, (MopVec3){0, 0, -1},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *front = add_quad_at_z(vp, -1.0f, 1);
  MopMesh *back = add_quad_at_z(vp, -2.0f, 2);
  TEST_ASSERT(front != NULL);
  TEST_ASSERT(back != NULL);

  mop_viewport_render(vp);

  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick.hit == true);
  TEST_ASSERT(pick.object_id == 1);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 2. test_coplanar_epsilon_separation
 *    Loop over epsilon in {1e-2, 1e-3, 1e-4, 1e-5, 1e-6}:
 *    front quad at z=-5, back at z=-(5+eps). Assert front wins for eps>=1e-3.
 * ========================================================================= */

static void test_coplanar_epsilon_separation(void) {
  TEST_BEGIN("coplanar_epsilon_separation");

  float epsilons[] = {1e-2f, 1e-3f, 1e-4f, 1e-5f, 1e-6f};
  int count = (int)(sizeof(epsilons) / sizeof(epsilons[0]));

  for (int i = 0; i < count; i++) {
    float eps = epsilons[i];

    MopViewport *vp = make_viewport(64, 48);
    TEST_ASSERT(vp != NULL);

    mop_viewport_set_camera(vp, (MopVec3){0, 0, 0}, (MopVec3){0, 0, -1},
                            (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

    MopMesh *front = add_quad_at_z(vp, -5.0f, 10);
    MopMesh *back = add_quad_at_z(vp, -(5.0f + eps), 20);
    TEST_ASSERT(front != NULL);
    TEST_ASSERT(back != NULL);

    mop_viewport_render(vp);

    MopPickResult pick = mop_viewport_pick(vp, 32, 24);

    /* For sufficiently large epsilon, the depth buffer must resolve
     * the front quad as the winner. Below 1e-3 we may hit z-fighting. */
    if (eps >= 1e-3f) {
      TEST_ASSERT_MSG(pick.hit == true, "expected hit for large epsilon");
      TEST_ASSERT_MSG(pick.object_id == 10,
                      "front quad should win for eps >= 1e-3");
    }

    mop_viewport_destroy(vp);
  }

  TEST_END();
}

/* =========================================================================
 * 3. test_depth_at_near_plane
 *    Quad at z just past near plane. Pick center -> hit, depth close to 0.0.
 * ========================================================================= */

static void test_depth_at_near_plane(void) {
  TEST_BEGIN("depth_at_near_plane");

  float near_plane = 0.1f;
  float far_plane = 100.0f;

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 0}, (MopVec3){0, 0, -1},
                          (MopVec3){0, 1, 0}, 60.0f, near_plane, far_plane);

  /* Place quad just past the near plane */
  MopMesh *mesh = add_quad_at_z(vp, -(near_plane + 0.01f), 1);
  TEST_ASSERT(mesh != NULL);

  mop_viewport_render(vp);

  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick.hit == true);
  /* Depth near the near plane should be close to 0.0 */
  TEST_ASSERT_MSG(pick.depth < 0.1f,
                  "depth at near plane should be close to 0.0");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 4. test_depth_at_far_plane
 *    Quad near far plane. Pick center -> depth close to 1.0.
 * ========================================================================= */

static void test_depth_at_far_plane(void) {
  TEST_BEGIN("depth_at_far_plane");

  float near_plane = 0.1f;
  float far_plane = 100.0f;

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 0}, (MopVec3){0, 0, -1},
                          (MopVec3){0, 1, 0}, 60.0f, near_plane, far_plane);

  /* Place quad close to the far plane */
  MopMesh *mesh = add_quad_at_z(vp, -(far_plane - 1.0f), 1);
  TEST_ASSERT(mesh != NULL);
  /* Scale up so it still covers the center pixel at this distance */
  mop_mesh_set_scale(mesh, (MopVec3){200.0f, 200.0f, 1.0f});

  mop_viewport_render(vp);

  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick.hit == true);
  /* Depth near the far plane should be close to 1.0 */
  TEST_ASSERT_MSG(pick.depth > 0.9f,
                  "depth at far plane should be close to 1.0");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 5. test_depth_gradient_monotonic
 *    10 quads at z=-1,-2,...,-10 with different object_ids.
 *    Assert depth values are strictly increasing with distance.
 * ========================================================================= */

static void test_depth_gradient_monotonic(void) {
  TEST_BEGIN("depth_gradient_monotonic");

  /* Render each quad alone in its own viewport and record its depth.
   * This avoids occlusion — we want the depth value for each distance. */
  float depths[10];

  for (int i = 0; i < 10; i++) {
    MopViewport *vp = make_viewport(64, 48);
    TEST_ASSERT(vp != NULL);

    mop_viewport_set_camera(vp, (MopVec3){0, 0, 0}, (MopVec3){0, 0, -1},
                            (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

    float z = -(float)(i + 1);
    MopMesh *mesh = add_quad_at_z(vp, z, (uint32_t)(i + 1));
    TEST_ASSERT(mesh != NULL);

    mop_viewport_render(vp);

    MopPickResult pick = mop_viewport_pick(vp, 32, 24);
    TEST_ASSERT(pick.hit == true);
    depths[i] = pick.depth;

    mop_viewport_destroy(vp);
  }

  /* Verify strict monotonic increase: closer quads have smaller depth */
  for (int i = 1; i < 10; i++) {
    TEST_ASSERT_MSG(depths[i] > depths[i - 1],
                    "depth must increase with distance");
  }

  TEST_END();
}

/* =========================================================================
 * 6. test_large_world_coords
 *    Camera at (10000,10000,10003), target at (10000,10000,10000).
 *    Cube at (10000,10000,10000). Render and pick center -> should hit.
 * ========================================================================= */

static void test_large_world_coords(void) {
  TEST_BEGIN("large_world_coords");

  /* Unit cube geometry — minimal inline definition */
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
      0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
      12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20,
  };

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){10000.0f, 10000.0f, 10003.0f},
                          (MopVec3){10000.0f, 10000.0f, 10000.0f},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                               .vertex_count = 24,
                                               .indices = CUBE_IDX,
                                               .index_count = 36,
                                               .object_id = 42});
  TEST_ASSERT(cube != NULL);
  mop_mesh_set_position(cube, (MopVec3){10000.0f, 10000.0f, 10000.0f});

  mop_viewport_render(vp);

  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT_MSG(pick.hit == true,
                  "cube at large world coords should be hittable");
  TEST_ASSERT(pick.object_id == 42);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 7. test_extreme_near_far_resolution
 *    near=0.001, far=100000. Two quads at z=-50 and z=-50.01.
 *    Just assert no crash (documents the z-fighting zone).
 * ========================================================================= */

static void test_extreme_near_far_resolution(void) {
  TEST_BEGIN("extreme_near_far_resolution");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 0}, (MopVec3){0, 0, -1},
                          (MopVec3){0, 1, 0}, 60.0f, 0.001f, 100000.0f);

  MopMesh *front = add_quad_at_z(vp, -50.0f, 1);
  MopMesh *back = add_quad_at_z(vp, -50.01f, 2);
  TEST_ASSERT(front != NULL);
  TEST_ASSERT(back != NULL);

  /* Scale up so they cover the center pixel at z=-50 */
  mop_mesh_set_scale(front, (MopVec3){100.0f, 100.0f, 1.0f});
  mop_mesh_set_scale(back, (MopVec3){100.0f, 100.0f, 1.0f});

  mop_viewport_render(vp);

  /* We do NOT assert which quad wins — with near=0.001 and far=100000,
   * the depth buffer has very poor precision at z=-50. This test
   * documents the z-fighting zone and verifies no crash. */
  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT_MSG(pick.hit == true, "should hit one of the two quads");

  /* Verify no NaN in depth */
  TEST_ASSERT_NO_NAN(pick.depth);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 8. test_depth_clear_value
 *    Empty scene (no meshes). Render. Pick at center -> hit==false,
 *    depth==1.0f (clear value).
 * ========================================================================= */

static void test_depth_clear_value(void) {
  TEST_BEGIN("depth_clear_value");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  /* No meshes added — empty scene */
  mop_viewport_render(vp);

  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick.hit == false);
  TEST_ASSERT_FLOAT_EQ(pick.depth, 1.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
  TEST_SUITE_BEGIN("torture_depth");

  TEST_RUN(test_depth_order_two_planes);
  TEST_RUN(test_coplanar_epsilon_separation);
  TEST_RUN(test_depth_at_near_plane);
  TEST_RUN(test_depth_at_far_plane);
  TEST_RUN(test_depth_gradient_monotonic);
  TEST_RUN(test_large_world_coords);
  TEST_RUN(test_extreme_near_far_resolution);
  TEST_RUN(test_depth_clear_value);

  TEST_REPORT();
  TEST_EXIT();
}
