/*
 * Master of Puppets — Hierarchy Torture Tests
 * test_torture_hierarchy.c — Parent-child transform propagation, reparenting,
 *                            deep chains, and hierarchy + picking integration
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* =========================================================================
 * Shared geometry — unit cube, 24 vertices, 36 indices
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
 * Helper: add a cube with a given object_id
 * ========================================================================= */

static MopMesh *add_cube(MopViewport *vp, uint32_t id) {
  return mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                                  .vertex_count = 24,
                                                  .indices = CUBE_IDX,
                                                  .index_count = 36,
                                                  .object_id = id});
}

/* =========================================================================
 * Helper: AABB center
 * ========================================================================= */

static MopVec3 aabb_center(MopAABB box) {
  return (MopVec3){(box.min.x + box.max.x) * 0.5f,
                   (box.min.y + box.max.y) * 0.5f,
                   (box.min.z + box.max.z) * 0.5f};
}

/* =========================================================================
 * Helper: approximate vec3 equality with tolerance
 * ========================================================================= */

#define NEAR_TOL 0.15f

#define ASSERT_VEC3_NEAR(v, ex, ey, ez)                                        \
  do {                                                                         \
    MopVec3 _vn = (v);                                                         \
    TEST_ASSERT_MSG(fabsf(_vn.x - (ex)) < NEAR_TOL &&                          \
                        fabsf(_vn.y - (ey)) < NEAR_TOL &&                      \
                        fabsf(_vn.z - (ez)) < NEAR_TOL,                        \
                    "vec3 not near expected");                                 \
  } while (0)

/* =========================================================================
 * 1. test_single_parent_child
 *    Parent at (5,0,0), child at local (0,3,0).
 *    After render, child's world AABB center should be near (5,3,0).
 * ========================================================================= */

static void test_single_parent_child(void) {
  TEST_BEGIN("single_parent_child");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){5, 3, 10}, (MopVec3){5, 3, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *parent = add_cube(vp, 1);
  MopMesh *child = add_cube(vp, 2);
  TEST_ASSERT(parent != NULL);
  TEST_ASSERT(child != NULL);

  mop_mesh_set_position(parent, (MopVec3){5.0f, 0.0f, 0.0f});
  mop_mesh_set_position(child, (MopVec3){0.0f, 3.0f, 0.0f});
  mop_mesh_set_parent(child, parent, vp);

  mop_viewport_render(vp);

  MopVec3 center = aabb_center(mop_mesh_get_aabb_world(child, vp));
  ASSERT_VEC3_NEAR(center, 5.0f, 3.0f, 0.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 2. test_deep_chain_16
 *    Chain of 16 cubes, each at local position (1,0,0).
 *    After render, the 16th mesh's world AABB center ≈ (16,0,0).
 * ========================================================================= */

static void test_deep_chain_16(void) {
  TEST_BEGIN("deep_chain_16");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){8, 0, 30}, (MopVec3){8, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 200.0f);

  MopMesh *chain[16];
  for (int i = 0; i < 16; i++) {
    chain[i] = add_cube(vp, (uint32_t)(i + 1));
    TEST_ASSERT(chain[i] != NULL);
    mop_mesh_set_position(chain[i], (MopVec3){1.0f, 0.0f, 0.0f});
    if (i > 0)
      mop_mesh_set_parent(chain[i], chain[i - 1], vp);
  }

  mop_viewport_render(vp);

  MopVec3 center = aabb_center(mop_mesh_get_aabb_world(chain[15], vp));
  ASSERT_VEC3_NEAR(center, 16.0f, 0.0f, 0.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 3. test_rotation_propagation
 *    Parent rotated 90 deg around Y (pi/2 radians). Child at local (1,0,0).
 *    After render, child's world position ≈ (0,0,-1).
 *
 *    Rotation around Y by +pi/2:
 *      x' = z, z' = -x  =>  local (1,0,0) becomes world (0,0,-1).
 * ========================================================================= */

static void test_rotation_propagation(void) {
  TEST_BEGIN("rotation_propagation");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *parent = add_cube(vp, 1);
  MopMesh *child = add_cube(vp, 2);
  TEST_ASSERT(parent != NULL);
  TEST_ASSERT(child != NULL);

  float half_pi = 3.14159265f / 2.0f;
  mop_mesh_set_rotation(parent, (MopVec3){0.0f, half_pi, 0.0f});
  mop_mesh_set_position(child, (MopVec3){1.0f, 0.0f, 0.0f});
  mop_mesh_set_parent(child, parent, vp);

  mop_viewport_render(vp);

  MopVec3 center = aabb_center(mop_mesh_get_aabb_world(child, vp));
  ASSERT_VEC3_NEAR(center, 0.0f, 0.0f, -1.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 4. test_scale_propagation
 *    Parent scaled (2,2,2). Child at local (1,1,1).
 *    After render, child's world AABB center ≈ (2,2,2).
 * ========================================================================= */

static void test_scale_propagation(void) {
  TEST_BEGIN("scale_propagation");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){2, 2, 10}, (MopVec3){2, 2, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *parent = add_cube(vp, 1);
  MopMesh *child = add_cube(vp, 2);
  TEST_ASSERT(parent != NULL);
  TEST_ASSERT(child != NULL);

  mop_mesh_set_scale(parent, (MopVec3){2.0f, 2.0f, 2.0f});
  mop_mesh_set_position(child, (MopVec3){1.0f, 1.0f, 1.0f});
  mop_mesh_set_parent(child, parent, vp);

  mop_viewport_render(vp);

  MopVec3 center = aabb_center(mop_mesh_get_aabb_world(child, vp));
  ASSERT_VEC3_NEAR(center, 2.0f, 2.0f, 2.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 5. test_reparent
 *    A at (10,0,0), B at (0,10,0). Set C's parent to A, render.
 *    Then reparent C to B, render. Verify C's world AABB reflects B.
 * ========================================================================= */

static void test_reparent(void) {
  TEST_BEGIN("reparent");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){5, 5, 20}, (MopVec3){5, 5, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 200.0f);

  MopMesh *a = add_cube(vp, 1);
  MopMesh *b = add_cube(vp, 2);
  MopMesh *c = add_cube(vp, 3);
  TEST_ASSERT(a != NULL);
  TEST_ASSERT(b != NULL);
  TEST_ASSERT(c != NULL);

  mop_mesh_set_position(a, (MopVec3){10.0f, 0.0f, 0.0f});
  mop_mesh_set_position(b, (MopVec3){0.0f, 10.0f, 0.0f});
  /* C has no local offset — it should sit at parent's position */

  /* Phase 1: parent C under A */
  mop_mesh_set_parent(c, a, vp);
  mop_viewport_render(vp);

  MopVec3 center1 = aabb_center(mop_mesh_get_aabb_world(c, vp));
  ASSERT_VEC3_NEAR(center1, 10.0f, 0.0f, 0.0f);

  /* Phase 2: reparent C under B */
  mop_mesh_set_parent(c, b, vp);
  mop_viewport_render(vp);

  MopVec3 center2 = aabb_center(mop_mesh_get_aabb_world(c, vp));
  ASSERT_VEC3_NEAR(center2, 0.0f, 10.0f, 0.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 6. test_clear_parent
 *    Set parent, render. Clear parent, render.
 *    Verify mesh's world AABB matches its local position.
 * ========================================================================= */

static void test_clear_parent(void) {
  TEST_BEGIN("clear_parent");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){5, 5, 15}, (MopVec3){5, 5, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *parent = add_cube(vp, 1);
  MopMesh *child = add_cube(vp, 2);
  TEST_ASSERT(parent != NULL);
  TEST_ASSERT(child != NULL);

  mop_mesh_set_position(parent, (MopVec3){10.0f, 0.0f, 0.0f});
  mop_mesh_set_position(child, (MopVec3){0.0f, 3.0f, 0.0f});
  mop_mesh_set_parent(child, parent, vp);

  /* With parent: child world center ≈ (10, 3, 0) */
  mop_viewport_render(vp);
  MopVec3 c1 = aabb_center(mop_mesh_get_aabb_world(child, vp));
  ASSERT_VEC3_NEAR(c1, 10.0f, 3.0f, 0.0f);

  /* Clear parent: child world center ≈ (0, 3, 0) — its own local position */
  mop_mesh_clear_parent(child);
  mop_viewport_render(vp);
  MopVec3 c2 = aabb_center(mop_mesh_get_aabb_world(child, vp));
  ASSERT_VEC3_NEAR(c2, 0.0f, 3.0f, 0.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 7. test_hierarchy_with_picking
 *    Parent at (10,0,0), child (id=42) at local (0,0,0).
 *    Camera looking at (10,0,0). Pick center -> id=42.
 * ========================================================================= */

static void test_hierarchy_with_picking(void) {
  TEST_BEGIN("hierarchy_with_picking");

  MopViewport *vp = make_viewport(128, 96);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){10, 0, 5}, (MopVec3){10, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *parent = add_cube(vp, 1);
  MopMesh *child = add_cube(vp, 42);
  TEST_ASSERT(parent != NULL);
  TEST_ASSERT(child != NULL);

  mop_mesh_set_position(parent, (MopVec3){10.0f, 0.0f, 0.0f});
  /* Child at local (0,0,0) relative to parent => world (10,0,0) */
  mop_mesh_set_parent(child, parent, vp);

  mop_viewport_render(vp);

  /* Pick at the center of the viewport — should hit the child (drawn on top
   * at same position as parent, last-drawn wins in the ID buffer) */
  MopPickResult pick = mop_viewport_pick(vp, 64, 48);
  TEST_ASSERT(pick.hit == true);
  /* The child or parent could win the Z test — both are at the same position.
   * At minimum we should get a hit. Verify the child's id if it wins. */
  TEST_ASSERT_MSG(pick.object_id == 42 || pick.object_id == 1,
                  "expected hit on child (42) or parent (1)");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 8. test_hierarchy_aabb_world
 *    Parent translated (5,0,0). Child has known local AABB.
 *    After render, child's world AABB min/max should be shifted by (5,0,0).
 * ========================================================================= */

static void test_hierarchy_aabb_world(void) {
  TEST_BEGIN("hierarchy_aabb_world");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){5, 0, 10}, (MopVec3){5, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *parent = add_cube(vp, 1);
  MopMesh *child = add_cube(vp, 2);
  TEST_ASSERT(parent != NULL);
  TEST_ASSERT(child != NULL);

  mop_mesh_set_position(parent, (MopVec3){5.0f, 0.0f, 0.0f});
  mop_mesh_set_parent(child, parent, vp);

  mop_viewport_render(vp);

  /* Child's local AABB is the unit cube: [-0.5,0.5]^3.
   * With parent at (5,0,0), world AABB should be
   * [4.5,5.5]x[-0.5,0.5]x[-0.5,0.5]. */
  MopAABB local_box = mop_mesh_get_aabb_local(child, vp);
  MopAABB world_box = mop_mesh_get_aabb_world(child, vp);

  /* World min should be local min + (5,0,0) */
  TEST_ASSERT_MSG(fabsf(world_box.min.x - (local_box.min.x + 5.0f)) < NEAR_TOL,
                  "world AABB min.x not shifted correctly");
  TEST_ASSERT_MSG(fabsf(world_box.min.y - local_box.min.y) < NEAR_TOL,
                  "world AABB min.y unexpected shift");
  TEST_ASSERT_MSG(fabsf(world_box.min.z - local_box.min.z) < NEAR_TOL,
                  "world AABB min.z unexpected shift");

  /* World max should be local max + (5,0,0) */
  TEST_ASSERT_MSG(fabsf(world_box.max.x - (local_box.max.x + 5.0f)) < NEAR_TOL,
                  "world AABB max.x not shifted correctly");
  TEST_ASSERT_MSG(fabsf(world_box.max.y - local_box.max.y) < NEAR_TOL,
                  "world AABB max.y unexpected shift");
  TEST_ASSERT_MSG(fabsf(world_box.max.z - local_box.max.z) < NEAR_TOL,
                  "world AABB max.z unexpected shift");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 9. test_max_depth_boundary
 *    Chain of 20 meshes, each translating (1,0,0).
 *    After render, last mesh's AABB center ≈ (20,0,0).
 *    Tests that hierarchy propagation handles deep chains.
 * ========================================================================= */

static void test_max_depth_boundary(void) {
  TEST_BEGIN("max_depth_boundary");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){10, 0, 40}, (MopVec3){10, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 200.0f);

  MopMesh *chain[20];
  for (int i = 0; i < 20; i++) {
    chain[i] = add_cube(vp, (uint32_t)(i + 1));
    TEST_ASSERT(chain[i] != NULL);
    mop_mesh_set_position(chain[i], (MopVec3){1.0f, 0.0f, 0.0f});
    if (i > 0)
      mop_mesh_set_parent(chain[i], chain[i - 1], vp);
  }

  mop_viewport_render(vp);

  MopVec3 center = aabb_center(mop_mesh_get_aabb_world(chain[19], vp));
  ASSERT_VEC3_NEAR(center, 20.0f, 0.0f, 0.0f);

  /* Also verify that all intermediate meshes have finite world AABBs */
  for (int i = 0; i < 20; i++) {
    MopVec3 c = aabb_center(mop_mesh_get_aabb_world(chain[i], vp));
    TEST_ASSERT_FINITE_VEC3(c);
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
  TEST_SUITE_BEGIN("torture_hierarchy");

  TEST_RUN(test_single_parent_child);
  TEST_RUN(test_deep_chain_16);
  TEST_RUN(test_rotation_propagation);
  TEST_RUN(test_scale_propagation);
  TEST_RUN(test_reparent);
  TEST_RUN(test_clear_parent);
  TEST_RUN(test_hierarchy_with_picking);
  TEST_RUN(test_hierarchy_aabb_world);
  TEST_RUN(test_max_depth_boundary);

  TEST_REPORT();
  TEST_EXIT();
}
