/*
 * Master of Puppets — Indirect Draw + GPU Culling Tests
 * test_indirect_draw.c — Phase 2B: bounding spheres, frustum culling, indirect
 *
 * Tests validate the CPU-side math used by the indirect draw path:
 *   - Bounding sphere from AABB (center + half-diagonal radius)
 *   - Frustum plane extraction (Gribb-Hartmann)
 *   - Sphere-frustum culling (inside/outside/intersect)
 *   - Visible mesh counting after culling
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>
#include <stddef.h>

/* Include internal header for MopRhiDrawCall validation — test-only */
#include "rhi/rhi.h"

/* ---- Geometry ---- */

static const MopVertex CUBE_VERTS[] = {
    /* Front face */
    {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    /* Back face */
    {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    /* Top face */
    {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    /* Bottom face */
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    /* Right face */
    {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    /* Left face */
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
};

static const uint32_t CUBE_IDX[] = {
    0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
};

/* ---- Bounding sphere from AABB ---- */

static void test_bsphere_from_unit_cube(void) {
  TEST_BEGIN("bsphere_from_unit_cube");
  /* Unit cube AABB: (-0.5,-0.5,-0.5) to (0.5,0.5,0.5)
   * Center = (0,0,0), radius = sqrt(0.25+0.25+0.25) = sqrt(0.75) ≈ 0.866 */
  MopAABB aabb = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
  MopVec3 center = mop_aabb_center(aabb);
  MopVec3 extents = mop_aabb_extents(aabb);
  float radius = sqrtf(extents.x * extents.x + extents.y * extents.y +
                       extents.z * extents.z);

  TEST_ASSERT_FLOAT_EQ(center.x, 0.0f);
  TEST_ASSERT_FLOAT_EQ(center.y, 0.0f);
  TEST_ASSERT_FLOAT_EQ(center.z, 0.0f);
  /* radius = half-diagonal = sqrt(3) * 0.5 ≈ 0.866 */
  TEST_ASSERT(fabsf(radius - 0.866025f) < 0.001f);
  TEST_END();
}

static void test_bsphere_from_offset_box(void) {
  TEST_BEGIN("bsphere_from_offset_box");
  /* Box at (10,20,30) with size (2,4,6) */
  MopAABB aabb = {{9.0f, 18.0f, 27.0f}, {11.0f, 22.0f, 33.0f}};
  MopVec3 center = mop_aabb_center(aabb);
  MopVec3 extents = mop_aabb_extents(aabb);
  float radius = sqrtf(extents.x * extents.x + extents.y * extents.y +
                       extents.z * extents.z);

  TEST_ASSERT_FLOAT_EQ(center.x, 10.0f);
  TEST_ASSERT_FLOAT_EQ(center.y, 20.0f);
  TEST_ASSERT_FLOAT_EQ(center.z, 30.0f);
  /* half-extents = (1,2,3), radius = sqrt(1+4+9) = sqrt(14) ≈ 3.742 */
  TEST_ASSERT(fabsf(radius - 3.7417f) < 0.01f);
  TEST_END();
}

static void test_bsphere_from_flat_box(void) {
  TEST_BEGIN("bsphere_from_flat_box");
  /* Degenerate flat box (zero height) */
  MopAABB aabb = {{0, 0, 0}, {4, 0, 3}};
  MopVec3 center = mop_aabb_center(aabb);
  MopVec3 extents = mop_aabb_extents(aabb);
  float radius = sqrtf(extents.x * extents.x + extents.y * extents.y +
                       extents.z * extents.z);

  TEST_ASSERT_FLOAT_EQ(center.x, 2.0f);
  TEST_ASSERT_FLOAT_EQ(center.y, 0.0f);
  TEST_ASSERT_FLOAT_EQ(center.z, 1.5f);
  /* half-extents = (2,0,1.5), radius = sqrt(4+0+2.25) = sqrt(6.25) = 2.5 */
  TEST_ASSERT_FLOAT_EQ(radius, 2.5f);
  TEST_END();
}

/* ---- Frustum plane extraction ---- */

static void test_frustum_planes_normalized(void) {
  TEST_BEGIN("frustum_planes_normalized");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 10}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_render(vp);

  MopFrustum frustum = mop_viewport_get_frustum(vp);

  /* Each plane normal should be approximately unit-length */
  for (int i = 0; i < 6; i++) {
    float nx = frustum.planes[i].x;
    float ny = frustum.planes[i].y;
    float nz = frustum.planes[i].z;
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    TEST_ASSERT(fabsf(len - 1.0f) < 0.01f);
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_frustum_near_far_planes(void) {
  TEST_BEGIN("frustum_near_far_planes");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 10}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 1.0f, 100.0f);
  mop_viewport_render(vp);

  MopFrustum frustum = mop_viewport_get_frustum(vp);

  /* Near plane (index 4): should face towards camera forward (-Z in view).
   * Far plane (index 5): should face opposite.
   * A point at the camera position (0,0,10) should be behind the near plane
   * (outside the frustum near side). */
  float near_dist = frustum.planes[4].x * 0 + frustum.planes[4].y * 0 +
                    frustum.planes[4].z * 10 + frustum.planes[4].w;
  /* Camera at z=10, near=1 → the camera eye is outside the near plane */
  TEST_ASSERT(near_dist < 0.0f);

  /* A point at the origin (inside frustum) should be positive for near */
  float origin_near = frustum.planes[4].x * 0 + frustum.planes[4].y * 0 +
                      frustum.planes[4].z * 0 + frustum.planes[4].w;
  TEST_ASSERT(origin_near > 0.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* ---- Frustum culling tests ---- */

static void test_visible_mesh_at_origin(void) {
  TEST_BEGIN("visible_mesh_at_origin");
  MopViewportDesc desc = {
      .width = 128, .height = 128, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMeshDesc mdesc = {.vertices = CUBE_VERTS,
                       .vertex_count = 24,
                       .indices = CUBE_IDX,
                       .index_count = 36,
                       .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &mdesc);
  (void)mesh;
  mop_viewport_render(vp);

  /* Cube at origin, camera looking at origin → should be visible */
  uint32_t visible = mop_viewport_visible_mesh_count(vp);
  TEST_ASSERT(visible >= 1);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_culled_mesh_behind_camera(void) {
  TEST_BEGIN("culled_mesh_behind_camera");
  MopViewportDesc desc = {
      .width = 128, .height = 128, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  /* Camera at z=5, looking towards -Z (at origin) */
  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 50.0f);

  MopMeshDesc mdesc = {.vertices = CUBE_VERTS,
                       .vertex_count = 24,
                       .indices = CUBE_IDX,
                       .index_count = 36,
                       .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &mdesc);

  /* Place cube far behind camera (z=+100) */
  mop_mesh_set_position(mesh, (MopVec3){0, 0, 100});
  mop_viewport_render(vp);

  /* Should be culled — 0 visible */
  uint32_t visible = mop_viewport_visible_mesh_count(vp);
  TEST_ASSERT(visible == 0);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_culled_mesh_far_right(void) {
  TEST_BEGIN("culled_mesh_far_right");
  MopViewportDesc desc = {
      .width = 128, .height = 128, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 50.0f);

  MopMeshDesc mdesc = {.vertices = CUBE_VERTS,
                       .vertex_count = 24,
                       .indices = CUBE_IDX,
                       .index_count = 36,
                       .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &mdesc);

  /* Place cube far to the right — outside the 60-deg FOV cone */
  mop_mesh_set_position(mesh, (MopVec3){500, 0, 0});
  mop_viewport_render(vp);

  uint32_t visible = mop_viewport_visible_mesh_count(vp);
  TEST_ASSERT(visible == 0);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_mixed_visible_and_culled(void) {
  TEST_BEGIN("mixed_visible_and_culled");
  MopViewportDesc desc = {
      .width = 128, .height = 128, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 50.0f);

  /* Mesh 1: at origin — visible */
  MopMeshDesc md1 = {.vertices = CUBE_VERTS,
                     .vertex_count = 24,
                     .indices = CUBE_IDX,
                     .index_count = 36,
                     .object_id = 1};
  MopMesh *m1 = mop_viewport_add_mesh(vp, &md1);
  (void)m1;

  /* Mesh 2: behind camera — culled */
  MopMeshDesc md2 = {.vertices = CUBE_VERTS,
                     .vertex_count = 24,
                     .indices = CUBE_IDX,
                     .index_count = 36,
                     .object_id = 2};
  MopMesh *m2 = mop_viewport_add_mesh(vp, &md2);
  mop_mesh_set_position(m2, (MopVec3){0, 0, 100});

  /* Mesh 3: slightly off-center — visible */
  MopMeshDesc md3 = {.vertices = CUBE_VERTS,
                     .vertex_count = 24,
                     .indices = CUBE_IDX,
                     .index_count = 36,
                     .object_id = 3};
  MopMesh *m3 = mop_viewport_add_mesh(vp, &md3);
  mop_mesh_set_position(m3, (MopVec3){1, 0, 0});

  mop_viewport_render(vp);

  uint32_t visible = mop_viewport_visible_mesh_count(vp);
  /* Expect at least 2 visible (meshes 1 and 3), mesh 2 is behind camera */
  TEST_ASSERT(visible >= 2);
  TEST_ASSERT(visible <= 3); /* at most all 3 */

  mop_viewport_destroy(vp);
  TEST_END();
}

/* ---- AABB overlap utility ---- */

static void test_aabb_overlap(void) {
  TEST_BEGIN("aabb_overlap");
  MopAABB a = {{0, 0, 0}, {2, 2, 2}};
  MopAABB b = {{1, 1, 1}, {3, 3, 3}};
  MopAABB c = {{5, 5, 5}, {6, 6, 6}};

  TEST_ASSERT(mop_aabb_overlaps(a, b) == true);
  TEST_ASSERT(mop_aabb_overlaps(a, c) == false);
  TEST_ASSERT(mop_aabb_overlaps(b, c) == false);
  TEST_END();
}

static void test_aabb_union(void) {
  TEST_BEGIN("aabb_union");
  MopAABB a = {{0, 0, 0}, {1, 1, 1}};
  MopAABB b = {{2, 3, 4}, {5, 6, 7}};
  MopAABB u = mop_aabb_union(a, b);

  TEST_ASSERT_FLOAT_EQ(u.min.x, 0.0f);
  TEST_ASSERT_FLOAT_EQ(u.min.y, 0.0f);
  TEST_ASSERT_FLOAT_EQ(u.min.z, 0.0f);
  TEST_ASSERT_FLOAT_EQ(u.max.x, 5.0f);
  TEST_ASSERT_FLOAT_EQ(u.max.y, 6.0f);
  TEST_ASSERT_FLOAT_EQ(u.max.z, 7.0f);
  TEST_END();
}

static void test_aabb_surface_area(void) {
  TEST_BEGIN("aabb_surface_area");
  /* Unit cube: SA = 6 * 1^2 = 6 */
  MopAABB cube = {{0, 0, 0}, {1, 1, 1}};
  TEST_ASSERT_FLOAT_EQ(mop_aabb_surface_area(cube), 6.0f);

  /* 2x3x4 box: SA = 2*(2*3 + 2*4 + 3*4) = 2*(6+8+12) = 52 */
  MopAABB box = {{0, 0, 0}, {2, 3, 4}};
  TEST_ASSERT_FLOAT_EQ(mop_aabb_surface_area(box), 52.0f);
  TEST_END();
}

/* ---- RHI draw call AABB fields ---- */

static void test_rhi_drawcall_aabb_zero_default(void) {
  TEST_BEGIN("rhi_drawcall_aabb_zero_default");
  MopRhiDrawCall call;
  memset(&call, 0, sizeof(call));
  /* Zero AABB means "no bounds available" — skip culling */
  TEST_ASSERT_FLOAT_EQ(call.aabb_min.x, 0.0f);
  TEST_ASSERT_FLOAT_EQ(call.aabb_min.y, 0.0f);
  TEST_ASSERT_FLOAT_EQ(call.aabb_min.z, 0.0f);
  TEST_ASSERT_FLOAT_EQ(call.aabb_max.x, 0.0f);
  TEST_ASSERT_FLOAT_EQ(call.aabb_max.y, 0.0f);
  TEST_ASSERT_FLOAT_EQ(call.aabb_max.z, 0.0f);
  TEST_END();
}

/* ---- Main ---- */

int main(void) {
  TEST_SUITE_BEGIN("indirect_draw");

  /* Bounding sphere */
  TEST_RUN(test_bsphere_from_unit_cube);
  TEST_RUN(test_bsphere_from_offset_box);
  TEST_RUN(test_bsphere_from_flat_box);

  /* Frustum planes */
  TEST_RUN(test_frustum_planes_normalized);
  TEST_RUN(test_frustum_near_far_planes);

  /* Frustum culling */
  TEST_RUN(test_visible_mesh_at_origin);
  TEST_RUN(test_culled_mesh_behind_camera);
  TEST_RUN(test_culled_mesh_far_right);
  TEST_RUN(test_mixed_visible_and_culled);

  /* AABB utilities */
  TEST_RUN(test_aabb_overlap);
  TEST_RUN(test_aabb_union);
  TEST_RUN(test_aabb_surface_area);

  /* Draw call struct */
  TEST_RUN(test_rhi_drawcall_aabb_zero_default);

  TEST_REPORT();
  TEST_EXIT();
}
