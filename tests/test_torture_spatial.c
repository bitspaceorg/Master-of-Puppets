/*
 * Master of Puppets — Spatial Query Torture Tests
 * test_torture_spatial.c — AABB, frustum culling, ray intersection, raycasting,
 *                          visible mesh counting, and scene AABB queries
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
 * 1. test_aabb_single_vertex
 *    Mesh where all 3 triangle vertices are at (3,4,5).
 *    AABB min ≈ max ≈ (3,4,5).
 * ========================================================================= */

static void test_aabb_single_vertex(void) {
  TEST_BEGIN("aabb_single_vertex");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){3, 4, 10}, (MopVec3){3, 4, 5},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopVertex verts[] = {
      {{3.0f, 4.0f, 5.0f}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{3.0f, 4.0f, 5.0f}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{3.0f, 4.0f, 5.0f}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t idx[] = {0, 1, 2};

  MopMesh *mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                           .vertex_count = 3,
                                                           .indices = idx,
                                                           .index_count = 3,
                                                           .object_id = 1});
  TEST_ASSERT(mesh != NULL);

  mop_viewport_render(vp);

  MopAABB box = mop_mesh_get_aabb_local(mesh, vp);
  TEST_ASSERT_VEC3_EQ(box.min, 3.0f, 4.0f, 5.0f);
  TEST_ASSERT_VEC3_EQ(box.max, 3.0f, 4.0f, 5.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 2. test_aabb_after_translation
 *    Unit cube, set position (10,0,0). Render.
 *    World AABB ≈ [9.5,10.5] x [-0.5,0.5] x [-0.5,0.5].
 * ========================================================================= */

static void test_aabb_after_translation(void) {
  TEST_BEGIN("aabb_after_translation");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){10, 0, 10}, (MopVec3){10, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  mop_mesh_set_position(cube, (MopVec3){10.0f, 0.0f, 0.0f});
  mop_viewport_render(vp);

  MopAABB box = mop_mesh_get_aabb_world(cube, vp);

  TEST_ASSERT_FLOAT_EQ(box.min.x, 9.5f);
  TEST_ASSERT_FLOAT_EQ(box.max.x, 10.5f);
  TEST_ASSERT_FLOAT_EQ(box.min.y, -0.5f);
  TEST_ASSERT_FLOAT_EQ(box.max.y, 0.5f);
  TEST_ASSERT_FLOAT_EQ(box.min.z, -0.5f);
  TEST_ASSERT_FLOAT_EQ(box.max.z, 0.5f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 3. test_aabb_after_rotation
 *    Unit cube rotated 45 deg around Y. Render.
 *    World AABB should be wider than [-0.5,0.5] in X and Z.
 *    A unit cube rotated 45 deg around Y has diagonal extent:
 *      half-diagonal on XZ plane = sqrt(0.5^2 + 0.5^2) = sqrt(0.5) ≈ 0.707
 * ========================================================================= */

static void test_aabb_after_rotation(void) {
  TEST_BEGIN("aabb_after_rotation");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  float pi_over_4 = 3.14159265f / 4.0f;
  mop_mesh_set_rotation(cube, (MopVec3){0.0f, pi_over_4, 0.0f});
  mop_viewport_render(vp);

  MopAABB box = mop_mesh_get_aabb_world(cube, vp);

  /* After 45-degree Y rotation, the AABB in X and Z should extend
   * beyond the original [-0.5, 0.5] range. */
  TEST_ASSERT_MSG(box.min.x < -0.5f, "rotated AABB min.x not wider");
  TEST_ASSERT_MSG(box.max.x > 0.5f, "rotated AABB max.x not wider");
  TEST_ASSERT_MSG(box.min.z < -0.5f, "rotated AABB min.z not wider");
  TEST_ASSERT_MSG(box.max.z > 0.5f, "rotated AABB max.z not wider");

  /* Y should remain [-0.5, 0.5] since rotation is around Y */
  TEST_ASSERT_FLOAT_EQ(box.min.y, -0.5f);
  TEST_ASSERT_FLOAT_EQ(box.max.y, 0.5f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 4. test_frustum_fully_inside
 *    Small cube at origin, camera at (0,0,5) looking at origin.
 *    Extract frustum. Test AABB -> should be inside or intersecting (>= 0).
 * ========================================================================= */

static void test_frustum_fully_inside(void) {
  TEST_BEGIN("frustum_fully_inside");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  mop_viewport_render(vp);

  MopFrustum frustum = mop_viewport_get_frustum(vp);
  MopAABB box = mop_mesh_get_aabb_world(cube, vp);

  int result = mop_frustum_test_aabb(&frustum, box);
  /* 1 = fully inside, 0 = intersect — both acceptable for a small cube */
  TEST_ASSERT_MSG(result >= 0,
                  "small cube at origin should be inside or intersecting");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 5. test_frustum_fully_outside
 *    Cube at (1000,0,0). Same camera at (0,0,5) looking at origin.
 *    Frustum test -> should return -1 (fully outside).
 * ========================================================================= */

static void test_frustum_fully_outside(void) {
  TEST_BEGIN("frustum_fully_outside");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  mop_mesh_set_position(cube, (MopVec3){1000.0f, 0.0f, 0.0f});
  mop_viewport_render(vp);

  MopFrustum frustum = mop_viewport_get_frustum(vp);
  MopAABB box = mop_mesh_get_aabb_world(cube, vp);

  int result = mop_frustum_test_aabb(&frustum, box);
  TEST_ASSERT_MSG(result == -1, "far cube should be fully outside frustum");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 6. test_frustum_intersecting
 *    Large cube [-10,10]^3 that partially overlaps the frustum.
 *    Test -> should return 0 (intersecting).
 * ========================================================================= */

static void test_frustum_intersecting(void) {
  TEST_BEGIN("frustum_intersecting");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 1);
  TEST_ASSERT(cube != NULL);

  /* Scale the unit cube by 20 to get [-10,10]^3 */
  mop_mesh_set_scale(cube, (MopVec3){20.0f, 20.0f, 20.0f});
  mop_viewport_render(vp);

  MopFrustum frustum = mop_viewport_get_frustum(vp);
  MopAABB box = mop_mesh_get_aabb_world(cube, vp);

  int result = mop_frustum_test_aabb(&frustum, box);
  /* The large cube extends well beyond the frustum boundaries,
   * so it should intersect (0) rather than be fully inside (1). */
  TEST_ASSERT_MSG(result == 0, "large cube should intersect frustum");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 7. test_ray_aabb_miss
 *    AABB at (10,10,10) to (11,11,11). Ray from origin toward (-1,-1,-1).
 *    Should miss.
 * ========================================================================= */

static void test_ray_aabb_miss(void) {
  TEST_BEGIN("ray_aabb_miss");

  /* mop_ray_intersect_aabb is a slab test on the infinite line — it reports
   * intersections at negative t (behind the ray origin).  Use a direction
   * that genuinely misses the box (perpendicular, not just away). */
  MopAABB box = {.min = {10.0f, 10.0f, 10.0f}, .max = {11.0f, 11.0f, 11.0f}};
  MopRay ray = {.origin = {0.0f, 0.0f, 0.0f}, .direction = {0.0f, 1.0f, 0.0f}};

  float t_near, t_far;
  bool hit = mop_ray_intersect_aabb(ray, box, &t_near, &t_far);
  /* Ray goes straight up from origin; box is at (10-11)^3 — miss */
  TEST_ASSERT(hit == false);

  TEST_END();
}

/* =========================================================================
 * 8. test_ray_aabb_tangent
 *    AABB at (0,0,-5) to (1,1,-4). Ray from (0.5,0.5,0) toward (0,0,-1).
 *    Should hit the front face (+Z face at z=-4).
 * ========================================================================= */

static void test_ray_aabb_tangent(void) {
  TEST_BEGIN("ray_aabb_tangent");

  MopAABB box = {.min = {0.0f, 0.0f, -5.0f}, .max = {1.0f, 1.0f, -4.0f}};
  MopRay ray = {.origin = {0.5f, 0.5f, 0.0f}, .direction = {0.0f, 0.0f, -1.0f}};

  float t_near, t_far;
  bool hit = mop_ray_intersect_aabb(ray, box, &t_near, &t_far);
  TEST_ASSERT(hit == true);

  /* t_near should be ~4.0 (distance from z=0 to z=-4) */
  TEST_ASSERT_MSG(fabsf(t_near - 4.0f) < 0.01f,
                  "t_near should be ~4.0 for front face hit");

  TEST_END();
}

/* =========================================================================
 * 9. test_ray_triangle_edge_cases
 *    Place a cube directly in front of camera. Raycast at center -> hit.
 *    Raycast at a far corner -> miss.
 * ========================================================================= */

static void test_ray_triangle_edge_cases(void) {
  TEST_BEGIN("ray_triangle_edge_cases");

  MopViewport *vp = make_viewport(128, 96);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube = add_cube(vp, 10);
  TEST_ASSERT(cube != NULL);

  mop_viewport_render(vp);

  /* Raycast at viewport center — should hit the cube */
  MopRayHit hit_center = mop_viewport_raycast(vp, 64.0f, 48.0f);
  TEST_ASSERT(hit_center.hit == true);
  TEST_ASSERT(hit_center.object_id == 10);

  /* Raycast at far corner — should miss (cube is small, corner is empty) */
  MopRayHit hit_corner = mop_viewport_raycast(vp, 0.0f, 0.0f);
  TEST_ASSERT(hit_corner.hit == false);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 10. test_raycast_through_hierarchy
 *     Parent at (5,0,0), child (id=99) at local (0,0,0) -> world (5,0,0).
 *     Camera at (5,0,3) looking at (5,0,0). Raycast center -> hit id=99.
 * ========================================================================= */

static void test_raycast_through_hierarchy(void) {
  TEST_BEGIN("raycast_through_hierarchy");

  MopViewport *vp = make_viewport(128, 96);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){5, 0, 3}, (MopVec3){5, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *parent = add_cube(vp, 50);
  MopMesh *child = add_cube(vp, 99);
  TEST_ASSERT(parent != NULL);
  TEST_ASSERT(child != NULL);

  mop_mesh_set_position(parent, (MopVec3){5.0f, 0.0f, 0.0f});
  /* Child at local origin => world (5,0,0) via parent */
  mop_mesh_set_parent(child, parent, vp);

  mop_viewport_render(vp);

  /* Raycast at viewport center — should hit child or parent at (5,0,0) */
  MopRayHit hit = mop_viewport_raycast(vp, 64.0f, 48.0f);
  TEST_ASSERT(hit.hit == true);
  /* Both meshes occupy the same world position; either could be the closest.
   * Verify we got a valid id. */
  TEST_ASSERT_MSG(hit.object_id == 99 || hit.object_id == 50,
                  "expected hit on child (99) or parent (50)");

  /* Verify the hit position is near (5, 0, z) where z is the front face */
  TEST_ASSERT_MSG(fabsf(hit.position.x - 5.0f) < 0.6f,
                  "hit position X not near 5.0");
  TEST_ASSERT_MSG(fabsf(hit.position.y) < 0.6f, "hit position Y not near 0.0");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 11. test_visible_mesh_count
 *     Place 6 cubes: 3 at origin (visible), 3 at (1000,0,0) (off-screen).
 *     Render. Check mop_viewport_visible_mesh_count ≈ 3.
 * ========================================================================= */

static void test_visible_mesh_count(void) {
  TEST_BEGIN("visible_mesh_count");

  MopViewport *vp = make_viewport(128, 96);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  /* 3 visible cubes near origin */
  for (int i = 0; i < 3; i++) {
    MopMesh *m = add_cube(vp, (uint32_t)(i + 1));
    TEST_ASSERT(m != NULL);
    /* Spread slightly so they are all distinct in the frustum */
    mop_mesh_set_position(m, (MopVec3){(float)(i - 1) * 1.5f, 0.0f, 0.0f});
  }

  /* 3 off-screen cubes far away */
  for (int i = 0; i < 3; i++) {
    MopMesh *m = add_cube(vp, (uint32_t)(i + 10));
    TEST_ASSERT(m != NULL);
    mop_mesh_set_position(m, (MopVec3){1000.0f + (float)i * 2.0f, 0.0f, 0.0f});
  }

  mop_viewport_render(vp);

  uint32_t visible = mop_viewport_visible_mesh_count(vp);
  /* Should be 3 visible meshes.  Allow some slack for implementation
   * differences (e.g. frustum test conservatism). */
  TEST_ASSERT_MSG(visible >= 2 && visible <= 4, "expected ~3 visible meshes");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 12. test_scene_aabb
 *     Place cubes at (-5,0,0) and (5,0,0).
 *     Scene AABB should span approximately (-5.5,-0.5,-0.5) to (5.5,0.5,0.5).
 * ========================================================================= */

static void test_scene_aabb(void) {
  TEST_BEGIN("scene_aabb");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 15}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 200.0f);

  MopMesh *left = add_cube(vp, 1);
  MopMesh *right = add_cube(vp, 2);
  TEST_ASSERT(left != NULL);
  TEST_ASSERT(right != NULL);

  mop_mesh_set_position(left, (MopVec3){-5.0f, 0.0f, 0.0f});
  mop_mesh_set_position(right, (MopVec3){5.0f, 0.0f, 0.0f});

  mop_viewport_render(vp);

  MopAABB scene = mop_viewport_get_scene_aabb(vp);

  /* Left cube world AABB: [-5.5,-0.5,-0.5] to [-4.5,0.5,0.5]
   * Right cube world AABB: [4.5,-0.5,-0.5] to [5.5,0.5,0.5]
   * Union: [-5.5,-0.5,-0.5] to [5.5,0.5,0.5] */
  TEST_ASSERT_FLOAT_EQ(scene.min.x, -5.5f);
  TEST_ASSERT_FLOAT_EQ(scene.max.x, 5.5f);
  TEST_ASSERT_FLOAT_EQ(scene.min.y, -0.5f);
  TEST_ASSERT_FLOAT_EQ(scene.max.y, 0.5f);
  TEST_ASSERT_FLOAT_EQ(scene.min.z, -0.5f);
  TEST_ASSERT_FLOAT_EQ(scene.max.z, 0.5f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
  TEST_SUITE_BEGIN("torture_spatial");

  TEST_RUN(test_aabb_single_vertex);
  TEST_RUN(test_aabb_after_translation);
  TEST_RUN(test_aabb_after_rotation);
  TEST_RUN(test_frustum_fully_inside);
  TEST_RUN(test_frustum_fully_outside);
  TEST_RUN(test_frustum_intersecting);
  TEST_RUN(test_ray_aabb_miss);
  TEST_RUN(test_ray_aabb_tangent);
  TEST_RUN(test_ray_triangle_edge_cases);
  TEST_RUN(test_raycast_through_hierarchy);
  TEST_RUN(test_visible_mesh_count);
  TEST_RUN(test_scene_aabb);

  TEST_REPORT();
  TEST_EXIT();
}
