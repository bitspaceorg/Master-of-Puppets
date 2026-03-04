/*
 * Master of Puppets — Selection Tests
 * test_selection.c — Tests for MOP sub-element selection system
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* Helper: create a viewport with a simple mesh */
static MopViewport *make_vp_with_mesh(void) {
  MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU});
  if (!vp)
    return NULL;

  MopVertex verts[3] = {
      {{-1, 0, 0}, {0, 1, 0}, {1, 1, 1, 1}, 0, 0},
      {{1, 0, 0}, {0, 1, 0}, {1, 1, 1, 1}, 0, 0},
      {{0, 1, 0}, {0, 1, 0}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t indices[3] = {0, 1, 2};
  MopMesh *m = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                        .vertex_count = 3,
                                                        .indices = indices,
                                                        .index_count = 3,
                                                        .object_id = 42});
  (void)m;
  return vp;
}

static void test_edit_mode_default_none(void) {
  TEST_BEGIN("edit_mode_default_none");
  MopViewport *vp = make_vp_with_mesh();
  TEST_ASSERT(vp != NULL);
  const MopSelection *sel = mop_viewport_get_selection(vp);
  TEST_ASSERT(sel != NULL);
  TEST_ASSERT(sel->mode == MOP_EDIT_NONE);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_selection_add_remove(void) {
  TEST_BEGIN("selection_add_remove");
  MopViewport *vp = make_vp_with_mesh();
  TEST_ASSERT(vp != NULL);

  mop_viewport_select_element(vp, 0);
  mop_viewport_select_element(vp, 5);
  const MopSelection *sel = mop_viewport_get_selection(vp);
  TEST_ASSERT(sel->element_count == 2);

  mop_viewport_deselect_element(vp, 0);
  TEST_ASSERT(sel->element_count == 1);
  TEST_ASSERT(sel->elements[0] == 5);

  mop_viewport_clear_selection(vp);
  TEST_ASSERT(sel->element_count == 0);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_selection_toggle(void) {
  TEST_BEGIN("selection_toggle");
  MopViewport *vp = make_vp_with_mesh();
  TEST_ASSERT(vp != NULL);

  mop_viewport_toggle_element(vp, 3);
  const MopSelection *sel = mop_viewport_get_selection(vp);
  TEST_ASSERT(sel->element_count == 1);

  mop_viewport_toggle_element(vp, 3);
  TEST_ASSERT(sel->element_count == 0);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_selection_null_safety(void) {
  TEST_BEGIN("selection_null_safety");
  mop_viewport_select_element(NULL, 0);
  mop_viewport_deselect_element(NULL, 0);
  mop_viewport_clear_selection(NULL);
  mop_viewport_toggle_element(NULL, 0);
  const MopSelection *sel = mop_viewport_get_selection(NULL);
  TEST_ASSERT(sel == NULL);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("selection");

  TEST_RUN(test_edit_mode_default_none);
  TEST_RUN(test_selection_add_remove);
  TEST_RUN(test_selection_toggle);
  TEST_RUN(test_selection_null_safety);

  TEST_REPORT();
  TEST_EXIT();
}
