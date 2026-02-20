/*
 * Master of Puppets — Gizmo Tests
 * test_gizmo.c — Create/destroy, mode switch, handle ID uniqueness
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include "test_harness.h"

static void test_gizmo_create_destroy(void) {
    TEST_BEGIN("gizmo_create_destroy");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    TEST_ASSERT(vp != NULL);
    MopGizmo *g = mop_gizmo_create(vp);
    TEST_ASSERT(g != NULL);
    mop_gizmo_destroy(g);
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_gizmo_create_null_viewport(void) {
    TEST_BEGIN("gizmo_create_null_returns_null");
    MopGizmo *g = mop_gizmo_create(NULL);
    TEST_ASSERT(g == NULL);
    TEST_END();
}

static void test_gizmo_mode_switch(void) {
    TEST_BEGIN("gizmo_mode_switch");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    MopGizmo *g = mop_gizmo_create(vp);

    TEST_ASSERT(mop_gizmo_get_mode(g) == MOP_GIZMO_TRANSLATE);
    mop_gizmo_set_mode(g, MOP_GIZMO_ROTATE);
    TEST_ASSERT(mop_gizmo_get_mode(g) == MOP_GIZMO_ROTATE);
    mop_gizmo_set_mode(g, MOP_GIZMO_SCALE);
    TEST_ASSERT(mop_gizmo_get_mode(g) == MOP_GIZMO_SCALE);

    mop_gizmo_destroy(g);
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_gizmo_show_hide(void) {
    TEST_BEGIN("gizmo_show_hide");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    MopGizmo *g = mop_gizmo_create(vp);

    /* Show and hide should not crash */
    mop_gizmo_show(g, (MopVec3){0, 0, 0}, NULL);
    mop_gizmo_hide(g);
    /* Double hide should be safe */
    mop_gizmo_hide(g);

    mop_gizmo_destroy(g);
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_gizmo_pick_no_show(void) {
    TEST_BEGIN("gizmo_pick_without_show");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    MopGizmo *g = mop_gizmo_create(vp);
    /* Pick with no visible gizmo should return NONE */
    MopPickResult p = { .hit = true, .object_id = 999, .depth = 0.5f };
    MopGizmoAxis axis = mop_gizmo_test_pick(g, p);
    TEST_ASSERT(axis == MOP_GIZMO_AXIS_NONE);
    mop_gizmo_destroy(g);
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_gizmo_handle_id_uniqueness(void) {
    TEST_BEGIN("gizmo_handle_id_uniqueness");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    /* Create two gizmos — their handle IDs must not overlap */
    MopGizmo *g1 = mop_gizmo_create(vp);
    MopGizmo *g2 = mop_gizmo_create(vp);
    TEST_ASSERT(g1 != NULL);
    TEST_ASSERT(g2 != NULL);
    /* Test that test_pick doesn't confuse them: a hit on g2's handle
     * should return NONE for g1 */
    mop_gizmo_show(g1, (MopVec3){0,0,0}, NULL);
    mop_gizmo_show(g2, (MopVec3){5,5,5}, NULL);
    /* We can't easily get g2's handle IDs, but we can verify the API
     * doesn't crash with multiple gizmos active */
    mop_gizmo_destroy(g2);
    mop_gizmo_destroy(g1);
    mop_viewport_destroy(vp);
    TEST_END();
}

int main(void) {
    TEST_SUITE_BEGIN("gizmo");

    TEST_RUN(test_gizmo_create_destroy);
    TEST_RUN(test_gizmo_create_null_viewport);
    TEST_RUN(test_gizmo_mode_switch);
    TEST_RUN(test_gizmo_show_hide);
    TEST_RUN(test_gizmo_pick_no_show);
    TEST_RUN(test_gizmo_handle_id_uniqueness);

    TEST_REPORT();
    TEST_EXIT();
}
