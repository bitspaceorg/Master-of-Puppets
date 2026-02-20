/*
 * Master of Puppets — Input Tests
 * test_input.c — Event queue, state transitions, selection
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include "test_harness.h"

static void test_poll_empty(void) {
    TEST_BEGIN("input_poll_empty_queue");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    MopEvent ev;
    bool has = mop_viewport_poll_event(vp, &ev);
    TEST_ASSERT(!has);
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_selected_initially_zero(void) {
    TEST_BEGIN("input_selected_initially_zero");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    TEST_ASSERT(mop_viewport_get_selected(vp) == 0);
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_deselect_on_empty(void) {
    TEST_BEGIN("input_deselect_on_empty");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    /* Deselect when nothing selected — should not crash, no event emitted */
    MopInputEvent ev = { .type = MOP_INPUT_DESELECT };
    mop_viewport_input(vp, &ev);
    MopEvent out;
    TEST_ASSERT(!mop_viewport_poll_event(vp, &out));
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_toggle_wireframe(void) {
    TEST_BEGIN("input_toggle_wireframe");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    MopInputEvent ev = { .type = MOP_INPUT_TOGGLE_WIREFRAME };
    mop_viewport_input(vp, &ev);
    /* Toggle again should go back to solid */
    mop_viewport_input(vp, &ev);
    /* Just verify no crash — render_mode is internal */
    mop_viewport_render(vp);
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_reset_view(void) {
    TEST_BEGIN("input_reset_view");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    MopInputEvent ev = { .type = MOP_INPUT_RESET_VIEW };
    mop_viewport_input(vp, &ev);
    mop_viewport_render(vp);
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_scroll_zoom(void) {
    TEST_BEGIN("input_scroll_zoom");
    MopViewportDesc vd = { .width = 64, .height = 64, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    MopInputEvent ev = { .type = MOP_INPUT_SCROLL, .scroll = 5.0f };
    mop_viewport_input(vp, &ev);
    /* Just verify no crash */
    mop_viewport_render(vp);
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_pointer_click_on_empty(void) {
    TEST_BEGIN("input_pointer_click_empty");
    MopViewportDesc vd = { .width = 128, .height = 128, .backend = MOP_BACKEND_CPU };
    MopViewport *vp = mop_viewport_create(&vd);
    mop_viewport_set_camera(vp,
        (MopVec3){0, 100, 0}, (MopVec3){0, 100, -1}, (MopVec3){0, 1, 0},
        60.0f, 0.1f, 100.0f);
    mop_viewport_render(vp);
    /* Click in empty space */
    MopInputEvent down = { .type = MOP_INPUT_POINTER_DOWN, .x = 64, .y = 64 };
    MopInputEvent up = { .type = MOP_INPUT_POINTER_UP, .x = 64, .y = 64 };
    mop_viewport_input(vp, &down);
    mop_viewport_input(vp, &up);
    TEST_ASSERT(mop_viewport_get_selected(vp) == 0);
    mop_viewport_destroy(vp);
    TEST_END();
}

static void test_null_input(void) {
    TEST_BEGIN("input_null_safety");
    /* These should not crash */
    mop_viewport_input(NULL, NULL);
    MopInputEvent ev = { .type = MOP_INPUT_POINTER_DOWN };
    mop_viewport_input(NULL, &ev);
    MopEvent out;
    TEST_ASSERT(!mop_viewport_poll_event(NULL, &out));
    TEST_ASSERT(mop_viewport_get_selected(NULL) == 0);
    TEST_END();
}

int main(void) {
    TEST_SUITE_BEGIN("input");

    TEST_RUN(test_poll_empty);
    TEST_RUN(test_selected_initially_zero);
    TEST_RUN(test_deselect_on_empty);
    TEST_RUN(test_toggle_wireframe);
    TEST_RUN(test_reset_view);
    TEST_RUN(test_scroll_zoom);
    TEST_RUN(test_pointer_click_on_empty);
    TEST_RUN(test_null_input);

    TEST_REPORT();
    TEST_EXIT();
}
