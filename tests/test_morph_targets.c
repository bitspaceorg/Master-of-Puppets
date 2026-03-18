/*
 * Master of Puppets — Morph Target Tests
 * test_morph_targets.c — Phase 6C: Blend shapes setup, weight updates
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

/* ---- Tests ---- */

static void test_morph_null_safety(void) {
  TEST_BEGIN("morph_null_safety");
  float targets[9] = {0};
  float weights[1] = {1.0f};
  mop_mesh_set_morph_targets(NULL, NULL, targets, weights, 1);
  mop_mesh_set_morph_weights(NULL, weights, 1);
  TEST_END();
}

static void test_morph_set_targets(void) {
  TEST_BEGIN("morph_set_targets");
  MopViewport *vp = make_viewport();

  MopVertex verts[3] = {
      {{0, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &desc);
  TEST_ASSERT(mesh != NULL);

  /* One morph target: move all vertices up by 1 */
  float target_deltas[9] = {0, 1, 0, 0, 1, 0, 0, 1, 0};
  float weights[1] = {0.5f};
  mop_mesh_set_morph_targets(mesh, vp, target_deltas, weights, 1);

  /* Render should apply morph blending */
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_morph_update_weights(void) {
  TEST_BEGIN("morph_update_weights");
  MopViewport *vp = make_viewport();

  MopVertex verts[3] = {
      {{0, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &desc);
  TEST_ASSERT(mesh != NULL);

  float target_deltas[9] = {0, 1, 0, 0, 1, 0, 0, 1, 0};
  float weights[1] = {0.0f};
  mop_mesh_set_morph_targets(mesh, vp, target_deltas, weights, 1);

  /* First render with weight=0 */
  mop_viewport_render(vp);

  /* Update weight to 1.0 and render again */
  float new_weights[1] = {1.0f};
  mop_mesh_set_morph_weights(mesh, new_weights, 1);
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_morph_multiple_targets(void) {
  TEST_BEGIN("morph_multiple_targets");
  MopViewport *vp = make_viewport();

  MopVertex verts[3] = {
      {{0, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &desc);
  TEST_ASSERT(mesh != NULL);

  /* Two morph targets: target 0 = up, target 1 = right */
  float deltas[18] = {
      /* Target 0: +Y */
      0,
      1,
      0,
      0,
      1,
      0,
      0,
      1,
      0,
      /* Target 1: +X */
      1,
      0,
      0,
      1,
      0,
      0,
      1,
      0,
      0,
  };
  float weights[2] = {0.5f, 0.3f};
  mop_mesh_set_morph_targets(mesh, vp, deltas, weights, 2);

  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_morph_weight_mismatch(void) {
  TEST_BEGIN("morph_weight_mismatch");
  MopViewport *vp = make_viewport();

  MopVertex verts[3] = {
      {{0, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &desc);
  TEST_ASSERT(mesh != NULL);

  float deltas[9] = {0, 1, 0, 0, 1, 0, 0, 1, 0};
  float w1[1] = {1.0f};
  mop_mesh_set_morph_targets(mesh, vp, deltas, w1, 1);

  /* Try to update with wrong count — should be silently rejected */
  float w2[2] = {0.5f, 0.5f};
  mop_mesh_set_morph_weights(mesh, w2, 2);

  /* Render should still work fine */
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_morph_zero_weight(void) {
  TEST_BEGIN("morph_zero_weight");
  MopViewport *vp = make_viewport();

  MopVertex verts[3] = {
      {{0, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &desc);
  TEST_ASSERT(mesh != NULL);

  /* All weights zero — mesh should remain at base pose */
  float deltas[9] = {0, 100, 0, 0, 100, 0, 0, 100, 0};
  float weights[1] = {0.0f};
  mop_mesh_set_morph_targets(mesh, vp, deltas, weights, 1);

  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_morph_mesh_removal_cleanup(void) {
  TEST_BEGIN("morph_mesh_removal_cleanup");
  MopViewport *vp = make_viewport();

  MopVertex verts[3] = {
      {{0, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMeshDesc desc = {.vertices = verts,
                      .vertex_count = 3,
                      .indices = indices,
                      .index_count = 3,
                      .object_id = 1};
  MopMesh *mesh = mop_viewport_add_mesh(vp, &desc);

  float deltas[9] = {0, 1, 0, 0, 1, 0, 0, 1, 0};
  float weights[1] = {1.0f};
  mop_mesh_set_morph_targets(mesh, vp, deltas, weights, 1);

  /* Remove mesh — should free morph data cleanly */
  mop_viewport_remove_mesh(vp, mesh);

  /* Viewport destruction should be clean */
  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("morph_targets");

  TEST_RUN(test_morph_null_safety);
  TEST_RUN(test_morph_set_targets);
  TEST_RUN(test_morph_update_weights);
  TEST_RUN(test_morph_multiple_targets);
  TEST_RUN(test_morph_weight_mismatch);
  TEST_RUN(test_morph_zero_weight);
  TEST_RUN(test_morph_mesh_removal_cleanup);

  TEST_REPORT();
  TEST_EXIT();
}
