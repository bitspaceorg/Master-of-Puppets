/*
 * Master of Puppets — Skeletal Skinning Tests
 * test_skinning.c — Phase 6A: Bone matrices, CPU skinning, hierarchy
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

static MopViewport *make_viewport(void) {
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  return mop_viewport_create(&desc);
}

/* Build a skinned vertex format: position + normal + color + uv + joints +
 * weights */
static MopVertexFormat make_skinned_format(void) {
  MopVertexFormat fmt = {0};
  fmt.attrib_count = 6;
  fmt.attribs[0] = (MopVertexAttrib){MOP_ATTRIB_POSITION, MOP_FORMAT_FLOAT3, 0};
  fmt.attribs[1] = (MopVertexAttrib){MOP_ATTRIB_NORMAL, MOP_FORMAT_FLOAT3, 12};
  fmt.attribs[2] = (MopVertexAttrib){MOP_ATTRIB_COLOR, MOP_FORMAT_FLOAT4, 24};
  fmt.attribs[3] =
      (MopVertexAttrib){MOP_ATTRIB_TEXCOORD0, MOP_FORMAT_FLOAT2, 40};
  fmt.attribs[4] = (MopVertexAttrib){MOP_ATTRIB_JOINTS, MOP_FORMAT_UBYTE4, 48};
  fmt.attribs[5] = (MopVertexAttrib){MOP_ATTRIB_WEIGHTS, MOP_FORMAT_FLOAT4, 52};
  fmt.stride = 68; /* 48 + 4 + 16 */
  return fmt;
}

/* Build a simple skinned triangle: 3 vertices, 1 bone */
static void build_skinned_triangle(uint8_t *buf, uint32_t stride) {
  float positions[3][3] = {{0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
  float normals[3][3] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  float colors[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
  float uv[3][2] = {{0.5f, 0}, {0, 1}, {1, 1}};
  uint8_t joints[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  float weights[3][4] = {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};

  for (int v = 0; v < 3; v++) {
    uint8_t *vert = buf + (size_t)v * stride;
    memcpy(vert + 0, positions[v], 12);
    memcpy(vert + 12, normals[v], 12);
    memcpy(vert + 24, colors[v], 16);
    memcpy(vert + 40, uv[v], 8);
    memcpy(vert + 48, joints[v], 4);
    memcpy(vert + 52, weights[v], 16);
  }
}

/* ---- Tests ---- */

static void test_bone_matrices_null_safety(void) {
  TEST_BEGIN("bone_matrices_null_safety");
  /* NULL mesh or viewport should not crash */
  MopMat4 identity = mop_mat4_identity();
  mop_mesh_set_bone_matrices(NULL, NULL, &identity, 1);

  MopViewport *vp = make_viewport();
  mop_mesh_set_bone_matrices(NULL, vp, &identity, 1);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_bone_matrices_requires_skinned_format(void) {
  TEST_BEGIN("bone_matrices_requires_skinned_format");
  MopViewport *vp = make_viewport();

  /* Standard MopVertex mesh (no joints/weights) */
  MopVertex verts[3] = {
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{-1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &desc);
  TEST_ASSERT(mesh != NULL);

  /* Should silently return — standard format has no joints */
  MopMat4 identity = mop_mat4_identity();
  mop_mesh_set_bone_matrices(mesh, vp, &identity, 1);

  /* Render should succeed without issues */
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_bone_matrices_skinned_mesh(void) {
  TEST_BEGIN("bone_matrices_skinned_mesh");
  MopViewport *vp = make_viewport();

  MopVertexFormat fmt = make_skinned_format();
  uint8_t vertex_data[3 * 68];
  build_skinned_triangle(vertex_data, fmt.stride);
  uint32_t indices[3] = {0, 1, 2};

  MopMeshDescEx desc = {.vertex_data = vertex_data,
                        .vertex_count = 3,
                        .indices = indices,
                        .index_count = 3,
                        .object_id = 1,
                        .vertex_format = &fmt};
  MopMesh *mesh = mop_viewport_add_mesh_ex(vp, &desc);
  TEST_ASSERT(mesh != NULL);

  /* Set identity bone matrix — should work */
  MopMat4 identity = mop_mat4_identity();
  mop_mesh_set_bone_matrices(mesh, vp, &identity, 1);

  /* Render should apply skinning */
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_bone_matrices_translation(void) {
  TEST_BEGIN("bone_matrices_translation");
  MopViewport *vp = make_viewport();

  MopVertexFormat fmt = make_skinned_format();
  uint8_t vertex_data[3 * 68];
  build_skinned_triangle(vertex_data, fmt.stride);
  uint32_t indices[3] = {0, 1, 2};

  MopMeshDescEx desc = {.vertex_data = vertex_data,
                        .vertex_count = 3,
                        .indices = indices,
                        .index_count = 3,
                        .object_id = 1,
                        .vertex_format = &fmt};
  MopMesh *mesh = mop_viewport_add_mesh_ex(vp, &desc);
  TEST_ASSERT(mesh != NULL);

  /* Translate bone by (1, 2, 3) */
  MopMat4 xlate = mop_mat4_identity();
  xlate.d[12] = 1.0f;
  xlate.d[13] = 2.0f;
  xlate.d[14] = 3.0f;
  mop_mesh_set_bone_matrices(mesh, vp, &xlate, 1);

  /* Render — skinning should apply the translation */
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_bone_matrices_multiple_bones(void) {
  TEST_BEGIN("bone_matrices_multiple_bones");
  MopViewport *vp = make_viewport();

  MopVertexFormat fmt = make_skinned_format();
  /* Create vertices with different bone assignments */
  uint8_t vertex_data[3 * 68];
  build_skinned_triangle(vertex_data, fmt.stride);

  /* Assign vertex 0 to bone 1, vertex 1 to bone 0, vertex 2 to bone 1 */
  uint8_t j0[4] = {1, 0, 0, 0};
  uint8_t j1[4] = {0, 0, 0, 0};
  uint8_t j2[4] = {1, 0, 0, 0};
  memcpy(vertex_data + 0 * 68 + 48, j0, 4);
  memcpy(vertex_data + 1 * 68 + 48, j1, 4);
  memcpy(vertex_data + 2 * 68 + 48, j2, 4);

  uint32_t indices[3] = {0, 1, 2};
  MopMeshDescEx desc = {.vertex_data = vertex_data,
                        .vertex_count = 3,
                        .indices = indices,
                        .index_count = 3,
                        .object_id = 1,
                        .vertex_format = &fmt};
  MopMesh *mesh = mop_viewport_add_mesh_ex(vp, &desc);
  TEST_ASSERT(mesh != NULL);

  /* Two bones: bone 0 = identity, bone 1 = translate(5,0,0) */
  MopMat4 bones[2];
  bones[0] = mop_mat4_identity();
  bones[1] = mop_mat4_identity();
  bones[1].d[12] = 5.0f;
  mop_mesh_set_bone_matrices(mesh, vp, bones, 2);

  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_bone_hierarchy_set(void) {
  TEST_BEGIN("bone_hierarchy_set");
  MopViewport *vp = make_viewport();

  MopVertexFormat fmt = make_skinned_format();
  uint8_t vertex_data[3 * 68];
  build_skinned_triangle(vertex_data, fmt.stride);
  uint32_t indices[3] = {0, 1, 2};

  MopMeshDescEx desc = {.vertex_data = vertex_data,
                        .vertex_count = 3,
                        .indices = indices,
                        .index_count = 3,
                        .object_id = 1,
                        .vertex_format = &fmt};
  MopMesh *mesh = mop_viewport_add_mesh_ex(vp, &desc);
  TEST_ASSERT(mesh != NULL);

  /* Set 3 bones first */
  MopMat4 bones[3];
  bones[0] = mop_mat4_identity();
  bones[1] = mop_mat4_identity();
  bones[1].d[12] = 1.0f;
  bones[2] = mop_mat4_identity();
  bones[2].d[12] = 2.0f;
  mop_mesh_set_bone_matrices(mesh, vp, bones, 3);

  /* Set hierarchy: bone 0 is root, bone 1 parent=0, bone 2 parent=1 */
  int32_t parents[3] = {-1, 0, 1};
  mop_mesh_set_bone_hierarchy(mesh, parents, 3);

  /* Render with skeleton overlay enabled */
  mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_SKELETON, true);
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_bone_hierarchy_null_safety(void) {
  TEST_BEGIN("bone_hierarchy_null_safety");
  /* Should not crash on NULL */
  mop_mesh_set_bone_hierarchy(NULL, NULL, 0);

  MopViewport *vp = make_viewport();

  /* Standard mesh with no bones */
  MopVertex verts[3] = {
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{-1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc mdesc = {.vertices = verts,
                       .vertex_count = 3,
                       .indices = indices,
                       .index_count = 3,
                       .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &mdesc);

  /* bone_count mismatch — should be rejected */
  int32_t parents[1] = {-1};
  mop_mesh_set_bone_hierarchy(mesh, parents, 1);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_skeleton_overlay_no_crash(void) {
  TEST_BEGIN("skeleton_overlay_no_crash");
  MopViewport *vp = make_viewport();

  /* Enable skeleton overlay with no skinned meshes — should be a no-op */
  mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_SKELETON, true);
  mop_viewport_render(vp);

  /* Add a non-skinned mesh and render again */
  MopVertex verts[3] = {
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{-1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  mop_viewport_add_mesh(vp, &desc);
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("skinning");

  TEST_RUN(test_bone_matrices_null_safety);
  TEST_RUN(test_bone_matrices_requires_skinned_format);
  TEST_RUN(test_bone_matrices_skinned_mesh);
  TEST_RUN(test_bone_matrices_translation);
  TEST_RUN(test_bone_matrices_multiple_bones);
  TEST_RUN(test_bone_hierarchy_set);
  TEST_RUN(test_bone_hierarchy_null_safety);
  TEST_RUN(test_skeleton_overlay_no_crash);

  TEST_REPORT();
  TEST_EXIT();
}
