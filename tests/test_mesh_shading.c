/*
 * Master of Puppets — Test Suite
 * test_mesh_shading.c — Phase 10: Mesh Shading Tests
 *
 * Tests validate:
 *   - Meshlet struct layout and constants
 *   - Meshlet builder (greedy clustering)
 *   - Meshlet bounding sphere correctness
 *   - Meshlet normal cone computation
 *   - Meshlet packing limits (64 verts, 124 tris)
 *   - Meshlet builder null safety
 *   - Count estimate function
 *   - Vulkan mesh shading feature flags
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <math.h>
#include <mop/mop.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef MOP_HAS_VULKAN
#include "backend/vulkan/vulkan_internal.h"
#endif

/* -------------------------------------------------------------------------
 * Helper: create a simple quad mesh (2 triangles, 4 vertices)
 * ------------------------------------------------------------------------- */

static void make_quad(MopVertex *verts, uint32_t *indices) {
  memset(verts, 0, 4 * sizeof(MopVertex));
  verts[0].position = (MopVec3){0, 0, 0};
  verts[0].normal = (MopVec3){0, 0, 1};
  verts[1].position = (MopVec3){1, 0, 0};
  verts[1].normal = (MopVec3){0, 0, 1};
  verts[2].position = (MopVec3){1, 1, 0};
  verts[2].normal = (MopVec3){0, 0, 1};
  verts[3].position = (MopVec3){0, 1, 0};
  verts[3].normal = (MopVec3){0, 0, 1};
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 0;
  indices[4] = 2;
  indices[5] = 3;
}

/* Helper: create a larger mesh to test multi-meshlet packing */
static void make_grid(uint32_t rows, uint32_t cols, MopVertex **out_verts,
                      uint32_t *out_vc, uint32_t **out_indices,
                      uint32_t *out_ic) {
  uint32_t vc = (rows + 1) * (cols + 1);
  uint32_t tc = rows * cols * 2;
  uint32_t ic = tc * 3;

  MopVertex *v = (MopVertex *)calloc(vc, sizeof(MopVertex));
  uint32_t *idx = (uint32_t *)malloc(ic * sizeof(uint32_t));

  for (uint32_t r = 0; r <= rows; r++) {
    for (uint32_t c = 0; c <= cols; c++) {
      uint32_t vi = r * (cols + 1) + c;
      v[vi].position = (MopVec3){(float)c, (float)r, 0};
      v[vi].normal = (MopVec3){0, 0, 1};
    }
  }

  uint32_t ii = 0;
  for (uint32_t r = 0; r < rows; r++) {
    for (uint32_t c = 0; c < cols; c++) {
      uint32_t tl = r * (cols + 1) + c;
      uint32_t tr = tl + 1;
      uint32_t bl = (r + 1) * (cols + 1) + c;
      uint32_t br = bl + 1;
      idx[ii++] = tl;
      idx[ii++] = tr;
      idx[ii++] = bl;
      idx[ii++] = tr;
      idx[ii++] = br;
      idx[ii++] = bl;
    }
  }

  *out_verts = v;
  *out_vc = vc;
  *out_indices = idx;
  *out_ic = ic;
}

/* -------------------------------------------------------------------------
 * Meshlet constants
 * ------------------------------------------------------------------------- */

static void test_meshlet_constants(void) {
  TEST_BEGIN("meshlet_constants");
  TEST_ASSERT(MOP_MESHLET_MAX_VERTICES == 64);
  TEST_ASSERT(MOP_MESHLET_MAX_TRIANGLES == 124);
  TEST_ASSERT(MOP_MESHLET_MAX_INDICES == 124 * 3);
  TEST_END();
}

static void test_meshlet_struct_size(void) {
  TEST_BEGIN("meshlet_struct_size");
  /* MopMeshlet should be 32 bytes (suitable for GPU SSBO) */
  TEST_ASSERT(sizeof(MopMeshlet) == 32);
  /* MopMeshletCone should be 32 bytes */
  TEST_ASSERT(sizeof(MopMeshletCone) == 32);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Count estimate
 * ------------------------------------------------------------------------- */

static void test_meshlet_count_estimate(void) {
  TEST_BEGIN("meshlet_count_estimate");
  TEST_ASSERT(mop_meshlet_count_estimate(0) == 0);
  TEST_ASSERT(mop_meshlet_count_estimate(1) == 1);
  TEST_ASSERT(mop_meshlet_count_estimate(124) == 1);
  TEST_ASSERT(mop_meshlet_count_estimate(125) == 2);
  TEST_ASSERT(mop_meshlet_count_estimate(248) == 2);
  TEST_ASSERT(mop_meshlet_count_estimate(249) == 3);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Null safety
 * ------------------------------------------------------------------------- */

static void test_meshlet_null_safety(void) {
  TEST_BEGIN("meshlet_null_safety");
  bool ok = mop_meshlet_build(NULL, 0, NULL, 0, NULL);
  TEST_ASSERT(!ok);

  MopMeshletData data;
  ok = mop_meshlet_build(NULL, 0, NULL, 0, &data);
  TEST_ASSERT(!ok);

  mop_meshlet_free(NULL); /* should not crash */
  TEST_ASSERT(1);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Simple quad (2 triangles → 1 meshlet)
 * ------------------------------------------------------------------------- */

static void test_meshlet_simple_quad(void) {
  TEST_BEGIN("meshlet_simple_quad");
  MopVertex verts[4];
  uint32_t indices[6];
  make_quad(verts, indices);

  MopMeshletData data;
  bool ok = mop_meshlet_build(verts, 4, indices, 6, &data);
  TEST_ASSERT(ok);
  TEST_ASSERT(data.meshlet_count == 1);
  TEST_ASSERT(data.meshlets[0].vertex_count == 4);
  TEST_ASSERT(data.meshlets[0].triangle_count == 2);
  TEST_ASSERT(data.vertex_index_count == 4);
  TEST_ASSERT(data.prim_index_count == 6);

  /* All primitive indices should be < vertex_count */
  for (uint32_t i = 0; i < data.prim_index_count; i++) {
    TEST_ASSERT(data.prim_indices[i] < data.meshlets[0].vertex_count);
  }

  mop_meshlet_free(&data);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Bounding sphere
 * ------------------------------------------------------------------------- */

static void test_meshlet_bounding_sphere(void) {
  TEST_BEGIN("meshlet_bounding_sphere");
  MopVertex verts[4];
  uint32_t indices[6];
  make_quad(verts, indices);

  MopMeshletData data;
  bool ok = mop_meshlet_build(verts, 4, indices, 6, &data);
  TEST_ASSERT(ok);

  /* Center should be approximately (0.5, 0.5, 0) */
  float cx = data.meshlets[0].center[0];
  float cy = data.meshlets[0].center[1];
  float cz = data.meshlets[0].center[2];
  TEST_ASSERT(cx > 0.4f && cx < 0.6f);
  TEST_ASSERT(cy > 0.4f && cy < 0.6f);
  TEST_ASSERT(cz > -0.1f && cz < 0.1f);

  /* Radius should be ~0.707 (sqrt(2)/2 = half diagonal of unit square) */
  float r = data.meshlets[0].radius;
  TEST_ASSERT(r > 0.65f && r < 0.75f);

  mop_meshlet_free(&data);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Normal cone
 * ------------------------------------------------------------------------- */

static void test_meshlet_normal_cone(void) {
  TEST_BEGIN("meshlet_normal_cone");
  MopVertex verts[4];
  uint32_t indices[6];
  make_quad(verts, indices);

  MopMeshletData data;
  bool ok = mop_meshlet_build(verts, 4, indices, 6, &data);
  TEST_ASSERT(ok);

  /* Quad faces +Z, so cone axis should be approximately (0, 0, 1) */
  float az = data.cones[0].axis[2];
  TEST_ASSERT(az > 0.9f);

  /* Cutoff should be 1.0 for coplanar triangles */
  float cutoff = data.cones[0].cutoff;
  TEST_ASSERT(cutoff > 0.99f);

  mop_meshlet_free(&data);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Multi-meshlet (large grid forces >1 meshlet)
 * ------------------------------------------------------------------------- */

static void test_meshlet_multi_meshlet(void) {
  TEST_BEGIN("meshlet_multi_meshlet");

  /* 50×50 grid = 2500 cells × 2 tris = 5000 triangles, ~51×51 = 2601 verts
   * Should produce ~5000/124 ≈ 41 meshlets (more due to vertex limits) */
  MopVertex *verts;
  uint32_t *indices, vc, ic;
  make_grid(50, 50, &verts, &vc, &indices, &ic);

  MopMeshletData data;
  bool ok = mop_meshlet_build(verts, vc, indices, ic, &data);
  TEST_ASSERT(ok);
  TEST_ASSERT(data.meshlet_count > 1);

  /* Verify all meshlets respect limits */
  for (uint32_t i = 0; i < data.meshlet_count; i++) {
    TEST_ASSERT(data.meshlets[i].vertex_count <= MOP_MESHLET_MAX_VERTICES);
    TEST_ASSERT(data.meshlets[i].triangle_count <= MOP_MESHLET_MAX_TRIANGLES);
    TEST_ASSERT(data.meshlets[i].radius > 0.0f);
  }

  /* Verify total triangles match */
  uint32_t total_tris = 0;
  for (uint32_t i = 0; i < data.meshlet_count; i++)
    total_tris += data.meshlets[i].triangle_count;
  TEST_ASSERT(total_tris == ic / 3);

  /* Verify prim indices are all in range */
  for (uint32_t m = 0; m < data.meshlet_count; m++) {
    const MopMeshlet *ml = &data.meshlets[m];
    for (uint32_t t = 0; t < ml->triangle_count * 3; t++) {
      uint8_t local_idx = data.prim_indices[ml->triangle_offset + t];
      TEST_ASSERT(local_idx < ml->vertex_count);
    }
  }

  mop_meshlet_free(&data);
  free(verts);
  free(indices);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Single triangle (edge case)
 * ------------------------------------------------------------------------- */

static void test_meshlet_single_triangle(void) {
  TEST_BEGIN("meshlet_single_triangle");
  MopVertex verts[3];
  memset(verts, 0, sizeof(verts));
  verts[0].position = (MopVec3){0, 0, 0};
  verts[1].position = (MopVec3){1, 0, 0};
  verts[2].position = (MopVec3){0, 1, 0};
  uint32_t indices[] = {0, 1, 2};

  MopMeshletData data;
  bool ok = mop_meshlet_build(verts, 3, indices, 3, &data);
  TEST_ASSERT(ok);
  TEST_ASSERT(data.meshlet_count == 1);
  TEST_ASSERT(data.meshlets[0].vertex_count == 3);
  TEST_ASSERT(data.meshlets[0].triangle_count == 1);
  TEST_ASSERT(data.prim_index_count == 3);

  mop_meshlet_free(&data);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Invalid input
 * ------------------------------------------------------------------------- */

static void test_meshlet_invalid_input(void) {
  TEST_BEGIN("meshlet_invalid_input");
  /* Zero-init: builder must reject on index_count alone; contents are
   * never read.  Satisfies gcc -Wmaybe-uninitialized. */
  MopVertex v[3] = {0};
  uint32_t idx[] = {0, 1, 2, 3}; /* 4 indices, not multiple of 3 */

  MopMeshletData data;
  bool ok = mop_meshlet_build(v, 3, idx, 4, &data);
  TEST_ASSERT(!ok);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Meshlet data free and reuse
 * ------------------------------------------------------------------------- */

static void test_meshlet_free_zeroes(void) {
  TEST_BEGIN("meshlet_free_zeroes");
  MopVertex verts[4];
  uint32_t indices[6];
  make_quad(verts, indices);

  MopMeshletData data;
  bool ok = mop_meshlet_build(verts, 4, indices, 6, &data);
  TEST_ASSERT(ok);
  TEST_ASSERT(data.meshlet_count > 0);

  mop_meshlet_free(&data);
  TEST_ASSERT(data.meshlet_count == 0);
  TEST_ASSERT(data.meshlets == NULL);
  TEST_ASSERT(data.vertex_indices == NULL);
  TEST_ASSERT(data.prim_indices == NULL);
  TEST_ASSERT(data.cones == NULL);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Vulkan mesh shader device fields
 * ------------------------------------------------------------------------- */

#ifdef MOP_HAS_VULKAN

static void test_vk_mesh_shader_fields(void) {
  TEST_BEGIN("vk_mesh_shader_fields");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));

  TEST_ASSERT(dev.has_mesh_shader == false);
  TEST_ASSERT(dev.max_mesh_output_vertices == 0);
  TEST_ASSERT(dev.max_mesh_output_prims == 0);
  TEST_ASSERT(dev.max_mesh_workgroup_size == 0);
  TEST_ASSERT(dev.max_task_workgroup_size == 0);

  dev.has_mesh_shader = true;
  dev.max_mesh_output_vertices = 256;
  dev.max_mesh_output_prims = 256;
  dev.max_mesh_workgroup_size = 128;
  dev.max_task_workgroup_size = 128;

  TEST_ASSERT(dev.has_mesh_shader == true);
  TEST_ASSERT(dev.max_mesh_output_vertices == 256);
  TEST_END();
}

static void test_vk_mesh_shader_pipeline_fields(void) {
  TEST_BEGIN("vk_mesh_shader_pipeline_fields");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));

  /* Pipeline resources should be zero-initialized */
  TEST_ASSERT(dev.meshlet_task == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_mesh == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_pipeline == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_pipeline_layout == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_desc_layout == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_desc_set == VK_NULL_HANDLE);

  /* GPU buffer resources should be zero-initialized */
  TEST_ASSERT(dev.meshlet_ssbo == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_ssbo_mem == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_cone_ssbo == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_cone_ssbo_mem == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_vert_idx_ssbo == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_vert_idx_ssbo_mem == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_prim_idx_ssbo == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_prim_idx_ssbo_mem == VK_NULL_HANDLE);
  TEST_ASSERT(dev.meshlet_total_count == 0);
  TEST_END();
}

#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
  printf("=== Mesh Shading Tests (Phase 10) ===\n");

  /* Meshlet structure and constants */
  test_meshlet_constants();
  test_meshlet_struct_size();
  test_meshlet_count_estimate();

  /* Null safety and invalid input */
  test_meshlet_null_safety();
  test_meshlet_invalid_input();

  /* Meshlet builder */
  test_meshlet_simple_quad();
  test_meshlet_bounding_sphere();
  test_meshlet_normal_cone();
  test_meshlet_multi_meshlet();
  test_meshlet_single_triangle();
  test_meshlet_free_zeroes();

#ifdef MOP_HAS_VULKAN
  /* Vulkan mesh shading */
  test_vk_mesh_shader_fields();
  test_vk_mesh_shader_pipeline_fields();
#endif

  TEST_REPORT();
  TEST_EXIT();
}
