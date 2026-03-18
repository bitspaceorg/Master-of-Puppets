/*
 * Master of Puppets — Dynamic Resource Container Tests
 * test_dynamic_resources.c — Phase 0A: Dynamic arrays, growth beyond fixed
 * limits
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

static void test_many_lights_dynamic(void) {
  TEST_BEGIN("many_lights_dynamic");
  MopViewport *vp = make_viewport();

  /* Add 64 lights — well beyond old MOP_MAX_LIGHTS=8 */
  for (int i = 0; i < 64; i++) {
    MopLight light = {
        .type = MOP_LIGHT_POINT,
        .position = {(float)i, 5.0f, 0},
        .color = {1, 1, 1, 1},
        .intensity = 1.0f,
        .range = 10.0f,
        .active = true,
    };
    MopLight *l = mop_viewport_add_light(vp, &light);
    TEST_ASSERT(l != NULL);
  }

  /* Default + 64 added = 65 */
  TEST_ASSERT(mop_viewport_light_count(vp) >= 65);

  /* Render should work with many lights */
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void noop_overlay(MopViewport *vp, void *ud) {
  (void)vp;
  (void)ud;
}

static void test_many_overlays_dynamic(void) {
  TEST_BEGIN("many_overlays_dynamic");
  MopViewport *vp = make_viewport();

  /* Register 32 custom overlays — beyond old MOP_MAX_OVERLAYS=16 */
  uint32_t handles[32];
  for (int i = 0; i < 32; i++) {
    handles[i] = mop_viewport_add_overlay(vp, "test_dyn", noop_overlay, NULL);
    TEST_ASSERT(handles[i] != UINT32_MAX);
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_many_hooks_dynamic(void) {
  TEST_BEGIN("many_hooks_dynamic");
  MopViewport *vp = make_viewport();

  /* Register 128 hooks — testing dynamic growth */
  for (int i = 0; i < 128; i++) {
    uint32_t h = mop_viewport_add_hook(
        vp, MOP_STAGE_PRE_SCENE,
        (MopPipelineHookFn)(void (*)(MopViewport *, void *))0, NULL);
    /* NULL fn is fine — hooks check fn != NULL before calling */
    (void)h;
  }

  /* Render should work */
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_many_meshes_dynamic(void) {
  TEST_BEGIN("many_meshes_dynamic");
  MopViewport *vp = make_viewport();

  MopVertex verts[3] = {
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{-1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};

  /* Add 200 meshes — beyond old MOP_MAX_MESHES */
  for (int i = 0; i < 200; i++) {
    MopMeshDesc desc = {.vertices = verts,
                        .vertex_count = 3,
                        .indices = indices,
                        .index_count = 3,
                        .object_id = (uint32_t)(i + 1)};
    MopMesh *m = mop_viewport_add_mesh(vp, &desc);
    TEST_ASSERT(m != NULL);
  }

  /* Render should work */
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_light_add_remove_cycle(void) {
  TEST_BEGIN("light_add_remove_cycle");
  MopViewport *vp = make_viewport();

  /* Add and remove lights repeatedly to test reuse of freed slots */
  for (int cycle = 0; cycle < 5; cycle++) {
    MopLight lights[8];
    MopLight *ptrs[8];
    for (int i = 0; i < 8; i++) {
      lights[i] = (MopLight){
          .type = MOP_LIGHT_POINT,
          .position = {(float)i, (float)cycle, 0},
          .color = {1, 1, 1, 1},
          .intensity = 1.0f,
          .range = 10.0f,
          .active = true,
      };
      ptrs[i] = mop_viewport_add_light(vp, &lights[i]);
      TEST_ASSERT(ptrs[i] != NULL);
    }
    for (int i = 0; i < 8; i++) {
      mop_viewport_remove_light(vp, ptrs[i]);
    }
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_mesh_remove_and_readd(void) {
  TEST_BEGIN("mesh_remove_and_readd");
  MopViewport *vp = make_viewport();

  MopVertex verts[3] = {
      {{0, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{-1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};

  /* Add, render, remove, re-add */
  for (int i = 0; i < 10; i++) {
    MopMeshDesc desc = {.vertices = verts,
                        .vertex_count = 3,
                        .indices = indices,
                        .index_count = 3,
                        .object_id = 42};
    MopMesh *m = mop_viewport_add_mesh(vp, &desc);
    TEST_ASSERT(m != NULL);
    mop_viewport_render(vp);
    mop_viewport_remove_mesh(vp, m);
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("dynamic_resources");

  TEST_RUN(test_many_lights_dynamic);
  TEST_RUN(test_many_overlays_dynamic);
  TEST_RUN(test_many_hooks_dynamic);
  TEST_RUN(test_many_meshes_dynamic);
  TEST_RUN(test_light_add_remove_cycle);
  TEST_RUN(test_mesh_remove_and_readd);

  TEST_REPORT();
  TEST_EXIT();
}
