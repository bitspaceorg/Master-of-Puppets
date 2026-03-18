/*
 * Master of Puppets — Bindless Resources Tests
 * test_bindless.c — Phase 2A: struct layout, texture registry, draw call AABB
 *
 * These tests validate the data structures and public API behavior that
 * underpin the bindless rendering path.  Since the Vulkan backend is not
 * available in the test environment (CPU backend only), we verify:
 *   - Struct sizes and field offsets (must match GLSL std430 layout)
 *   - Texture lifecycle (create/destroy) and AABB caching
 *   - Rendering with textured meshes
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>
#include <stddef.h>

/* Include internal headers for struct validation — test-only */
#include "rhi/rhi.h"

#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"
#endif

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

/* ---- Struct layout tests ---- */

#if defined(MOP_HAS_VULKAN)
static void test_object_data_size(void) {
  TEST_BEGIN("object_data_size");
  /* MopVkObjectData must be 144 bytes (std430 aligned) */
  TEST_ASSERT(sizeof(MopVkObjectData) == 144);
  TEST_END();
}

static void test_object_data_offsets(void) {
  TEST_BEGIN("object_data_offsets");
  /* Validate field offsets match GLSL std430 layout */
  TEST_ASSERT(offsetof(MopVkObjectData, model) == 0);
  TEST_ASSERT(offsetof(MopVkObjectData, ambient) == 64);
  TEST_ASSERT(offsetof(MopVkObjectData, opacity) == 68);
  TEST_ASSERT(offsetof(MopVkObjectData, object_id) == 72);
  TEST_ASSERT(offsetof(MopVkObjectData, blend_mode) == 76);
  TEST_ASSERT(offsetof(MopVkObjectData, metallic) == 80);
  TEST_ASSERT(offsetof(MopVkObjectData, roughness) == 84);
  TEST_ASSERT(offsetof(MopVkObjectData, base_tex_idx) == 88);
  TEST_ASSERT(offsetof(MopVkObjectData, normal_tex_idx) == 92);
  TEST_ASSERT(offsetof(MopVkObjectData, emissive) == 96);
  TEST_ASSERT(offsetof(MopVkObjectData, mr_tex_idx) == 112);
  TEST_ASSERT(offsetof(MopVkObjectData, ao_tex_idx) == 116);
  TEST_ASSERT(offsetof(MopVkObjectData, bound_sphere) == 128);
  TEST_END();
}

static void test_frame_globals_size(void) {
  TEST_BEGIN("frame_globals_size");
  /* MopVkFrameGlobals must be 496 bytes */
  TEST_ASSERT(sizeof(MopVkFrameGlobals) == 496);
  TEST_END();
}

static void test_frame_globals_offsets(void) {
  TEST_BEGIN("frame_globals_offsets");
  TEST_ASSERT(offsetof(MopVkFrameGlobals, light_dir) == 0);
  TEST_ASSERT(offsetof(MopVkFrameGlobals, cam_pos) == 16);
  TEST_ASSERT(offsetof(MopVkFrameGlobals, shadows_enabled) == 32);
  TEST_ASSERT(offsetof(MopVkFrameGlobals, cascade_count) == 36);
  TEST_ASSERT(offsetof(MopVkFrameGlobals, num_lights) == 40);
  TEST_ASSERT(offsetof(MopVkFrameGlobals, exposure) == 44);
  TEST_ASSERT(offsetof(MopVkFrameGlobals, cascade_vp) == 48);
  TEST_ASSERT(offsetof(MopVkFrameGlobals, cascade_splits) == 304);
  TEST_ASSERT(offsetof(MopVkFrameGlobals, view_proj) == 320);
  TEST_ASSERT(offsetof(MopVkFrameGlobals, frustum_planes) == 384);
  TEST_ASSERT(offsetof(MopVkFrameGlobals, total_draws) == 480);
  TEST_END();
}

static void test_light_ssbo_size(void) {
  TEST_BEGIN("light_ssbo_size");
  /* MopVkLight must be 64 bytes (std430 aligned) */
  TEST_ASSERT(sizeof(MopVkLight) == 64);
  TEST_END();
}
#endif /* MOP_HAS_VULKAN */

/* ---- Texture lifecycle ---- */

static void test_texture_create_destroy(void) {
  TEST_BEGIN("texture_create_destroy");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  /* Create a simple 2x2 texture */
  uint8_t pixels[4 * 4] = {255, 0, 0,   255, 0,   255, 0, 255,
                           0,   0, 255, 255, 255, 255, 0, 255};
  MopTexture *tex = mop_viewport_create_texture(vp, 2, 2, pixels);
  TEST_ASSERT(tex != NULL);

  /* Destroy should not crash */
  mop_viewport_destroy_texture(vp, tex);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_texture_assign_to_mesh(void) {
  TEST_BEGIN("texture_assign_to_mesh");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  MopMeshDesc mdesc = {.vertices = CUBE_VERTS,
                       .vertex_count = 24,
                       .indices = CUBE_IDX,
                       .index_count = 36,
                       .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &mdesc);
  TEST_ASSERT(mesh != NULL);

  uint8_t pixels[4 * 4] = {255, 0, 0,   255, 0,   255, 0, 255,
                           0,   0, 255, 255, 255, 255, 0, 255};
  MopTexture *tex = mop_viewport_create_texture(vp, 2, 2, pixels);
  TEST_ASSERT(tex != NULL);

  mop_mesh_set_texture(mesh, tex);

  /* Render should succeed with textured mesh */
  MopRenderResult result = mop_viewport_render(vp);
  TEST_ASSERT(result == MOP_RENDER_OK);

  mop_viewport_destroy_texture(vp, tex);
  mop_viewport_destroy(vp);
  TEST_END();
}

/* ---- AABB caching ---- */

static void test_aabb_cached_on_mesh(void) {
  TEST_BEGIN("aabb_cached_on_mesh");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  MopMeshDesc mdesc = {.vertices = CUBE_VERTS,
                       .vertex_count = 24,
                       .indices = CUBE_IDX,
                       .index_count = 36,
                       .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &mdesc);

  /* Get local AABB — should compute and cache from vertex data */
  MopAABB aabb = mop_mesh_get_aabb_local(mesh, vp);
  TEST_ASSERT_FLOAT_EQ(aabb.min.x, -0.5f);
  TEST_ASSERT_FLOAT_EQ(aabb.min.y, -0.5f);
  TEST_ASSERT_FLOAT_EQ(aabb.min.z, -0.5f);
  TEST_ASSERT_FLOAT_EQ(aabb.max.x, 0.5f);
  TEST_ASSERT_FLOAT_EQ(aabb.max.y, 0.5f);
  TEST_ASSERT_FLOAT_EQ(aabb.max.z, 0.5f);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_aabb_world_with_transform(void) {
  TEST_BEGIN("aabb_world_with_transform");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  MopMeshDesc mdesc = {.vertices = CUBE_VERTS,
                       .vertex_count = 24,
                       .indices = CUBE_IDX,
                       .index_count = 36,
                       .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &mdesc);

  /* Translate cube to (5, 0, 0) */
  mop_mesh_set_position(mesh, (MopVec3){5.0f, 0.0f, 0.0f});
  mop_viewport_render(vp); /* forces transform update */

  MopAABB world_aabb = mop_mesh_get_aabb_world(mesh, vp);

  /* World AABB should be centered at (5, 0, 0) */
  TEST_ASSERT_FLOAT_EQ(world_aabb.min.x, 4.5f);
  TEST_ASSERT_FLOAT_EQ(world_aabb.max.x, 5.5f);
  TEST_ASSERT_FLOAT_EQ(world_aabb.min.y, -0.5f);
  TEST_ASSERT_FLOAT_EQ(world_aabb.max.y, 0.5f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* ---- Render with multiple textured meshes ---- */

static void test_render_multiple_textured_meshes(void) {
  TEST_BEGIN("render_multiple_textured_meshes");
  MopViewportDesc desc = {
      .width = 128, .height = 128, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  uint8_t red[4] = {255, 0, 0, 255};
  uint8_t blue[4] = {0, 0, 255, 255};
  MopTexture *tex_r = mop_viewport_create_texture(vp, 1, 1, red);
  MopTexture *tex_b = mop_viewport_create_texture(vp, 1, 1, blue);

  MopMeshDesc mdesc1 = {.vertices = CUBE_VERTS,
                        .vertex_count = 24,
                        .indices = CUBE_IDX,
                        .index_count = 36,
                        .object_id = 1};
  MopMeshDesc mdesc2 = {.vertices = CUBE_VERTS,
                        .vertex_count = 24,
                        .indices = CUBE_IDX,
                        .index_count = 36,
                        .object_id = 2};
  MopMesh *m1 = mop_viewport_add_mesh(vp, &mdesc1);
  MopMesh *m2 = mop_viewport_add_mesh(vp, &mdesc2);
  mop_mesh_set_texture(m1, tex_r);
  mop_mesh_set_texture(m2, tex_b);
  mop_mesh_set_position(m2, (MopVec3){3.0f, 0.0f, 0.0f});

  MopRenderResult result = mop_viewport_render(vp);
  TEST_ASSERT(result == MOP_RENDER_OK);

  mop_viewport_destroy_texture(vp, tex_r);
  mop_viewport_destroy_texture(vp, tex_b);
  mop_viewport_destroy(vp);
  TEST_END();
}

/* ---- Draw call AABB propagation ---- */

static void test_draw_call_aabb_fields(void) {
  TEST_BEGIN("draw_call_aabb_fields");
  /* Verify the RHI draw call struct has AABB fields */
  MopRhiDrawCall call;
  memset(&call, 0, sizeof(call));
  call.aabb_min = (MopVec3){-1.0f, -2.0f, -3.0f};
  call.aabb_max = (MopVec3){1.0f, 2.0f, 3.0f};
  TEST_ASSERT_FLOAT_EQ(call.aabb_min.x, -1.0f);
  TEST_ASSERT_FLOAT_EQ(call.aabb_max.z, 3.0f);
  TEST_END();
}

/* ---- Frustum extraction and AABB testing ---- */

static void test_frustum_extraction(void) {
  TEST_BEGIN("frustum_extraction");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  /* Set a known camera */
  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_render(vp);

  MopFrustum frustum = mop_viewport_get_frustum(vp);
  /* All 6 planes should have non-zero normals */
  for (int i = 0; i < 6; i++) {
    float len = sqrtf(frustum.planes[i].x * frustum.planes[i].x +
                      frustum.planes[i].y * frustum.planes[i].y +
                      frustum.planes[i].z * frustum.planes[i].z);
    TEST_ASSERT(len > 0.01f);
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_frustum_aabb_inside(void) {
  TEST_BEGIN("frustum_aabb_inside");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_render(vp);

  MopFrustum frustum = mop_viewport_get_frustum(vp);

  /* An AABB at the origin should be inside the frustum */
  MopAABB inside = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
  int result = mop_frustum_test_aabb(&frustum, inside);
  TEST_ASSERT(result >= 0); /* inside or intersecting */

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_frustum_aabb_outside(void) {
  TEST_BEGIN("frustum_aabb_outside");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);
  mop_viewport_render(vp);

  MopFrustum frustum = mop_viewport_get_frustum(vp);

  /* An AABB far behind the camera should be outside */
  MopAABB behind = {{-1, -1, 50}, {1, 1, 200}};
  int result = mop_frustum_test_aabb(&frustum, behind);
  TEST_ASSERT(result == -1); /* fully outside */

  mop_viewport_destroy(vp);
  TEST_END();
}

/* ---- Scene AABB ---- */

static void test_scene_aabb(void) {
  TEST_BEGIN("scene_aabb");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  MopMeshDesc mdesc1 = {.vertices = CUBE_VERTS,
                        .vertex_count = 24,
                        .indices = CUBE_IDX,
                        .index_count = 36,
                        .object_id = 1};
  MopMeshDesc mdesc2 = {.vertices = CUBE_VERTS,
                        .vertex_count = 24,
                        .indices = CUBE_IDX,
                        .index_count = 36,
                        .object_id = 2};
  MopMesh *m1 = mop_viewport_add_mesh(vp, &mdesc1);
  MopMesh *m2 = mop_viewport_add_mesh(vp, &mdesc2);
  (void)m1;
  mop_mesh_set_position(m2, (MopVec3){10.0f, 0.0f, 0.0f});
  mop_viewport_render(vp);

  MopAABB scene = mop_viewport_get_scene_aabb(vp);
  /* Scene AABB should span from -0.5 to 10.5 on x-axis */
  TEST_ASSERT(scene.min.x <= -0.4f);
  TEST_ASSERT(scene.max.x >= 10.4f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* ---- Main ---- */

int main(void) {
  TEST_SUITE_BEGIN("bindless");

#if defined(MOP_HAS_VULKAN)
  TEST_RUN(test_object_data_size);
  TEST_RUN(test_object_data_offsets);
  TEST_RUN(test_frame_globals_size);
  TEST_RUN(test_frame_globals_offsets);
  TEST_RUN(test_light_ssbo_size);
#endif

  TEST_RUN(test_texture_create_destroy);
  TEST_RUN(test_texture_assign_to_mesh);
  TEST_RUN(test_aabb_cached_on_mesh);
  TEST_RUN(test_aabb_world_with_transform);
  TEST_RUN(test_render_multiple_textured_meshes);
  TEST_RUN(test_draw_call_aabb_fields);
  TEST_RUN(test_frustum_extraction);
  TEST_RUN(test_frustum_aabb_inside);
  TEST_RUN(test_frustum_aabb_outside);
  TEST_RUN(test_scene_aabb);

  TEST_REPORT();
  TEST_EXIT();
}
