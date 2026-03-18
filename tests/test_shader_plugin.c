/*
 * Master of Puppets — Shader Plugin Tests
 * test_shader_plugin.c — Phase 5A: Registration, dispatch, unregistration
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>
#include <mop/render/shader_plugin.h>

static MopViewport *make_viewport(void) {
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  return mop_viewport_create(&desc);
}

/* Draw callback tracking */
static int s_draw_called = 0;
static int s_draw_width = 0;
static int s_draw_height = 0;

static void test_draw_fn(const MopShaderDrawContext *ctx, void *user_data) {
  (void)user_data;
  s_draw_called++;
  s_draw_width = ctx->width;
  s_draw_height = ctx->height;
}

/* (stage_tracking_fn removed — not needed for current tests) */

/* ---- Tests ---- */

static void test_register_null_viewport(void) {
  TEST_BEGIN("register_null_viewport");
  MopShaderPluginDesc desc = {.name = "test",
                              .stage = MOP_SHADER_PLUGIN_POST_OPAQUE,
                              .draw = test_draw_fn};
  MopShaderPlugin *p = mop_viewport_register_shader(NULL, &desc);
  TEST_ASSERT(p == NULL);
  TEST_END();
}

static void test_register_null_desc(void) {
  TEST_BEGIN("register_null_desc");
  MopViewport *vp = make_viewport();
  MopShaderPlugin *p = mop_viewport_register_shader(vp, NULL);
  TEST_ASSERT(p == NULL);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_register_no_name(void) {
  TEST_BEGIN("register_no_name");
  MopViewport *vp = make_viewport();
  MopShaderPluginDesc desc = {.name = NULL,
                              .stage = MOP_SHADER_PLUGIN_POST_OPAQUE,
                              .draw = test_draw_fn};
  MopShaderPlugin *p = mop_viewport_register_shader(vp, &desc);
  TEST_ASSERT(p == NULL);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_register_success(void) {
  TEST_BEGIN("register_success");
  MopViewport *vp = make_viewport();
  MopShaderPluginDesc desc = {
      .name = "my_plugin",
      .stage = MOP_SHADER_PLUGIN_POST_OPAQUE,
      .draw = test_draw_fn,
  };
  MopShaderPlugin *p = mop_viewport_register_shader(vp, &desc);
  TEST_ASSERT(p != NULL);

  /* Check accessor */
  const char *name = mop_shader_plugin_get_name(p);
  TEST_ASSERT(name != NULL);
  TEST_ASSERT(strcmp(name, "my_plugin") == 0);

  /* No SPIR-V provided — shader modules should be NULL */
  TEST_ASSERT(mop_shader_plugin_get_vertex(p) == NULL);
  TEST_ASSERT(mop_shader_plugin_get_fragment(p) == NULL);
  TEST_ASSERT(mop_shader_plugin_get_compute(p) == NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_unregister(void) {
  TEST_BEGIN("unregister");
  MopViewport *vp = make_viewport();
  MopShaderPluginDesc desc = {
      .name = "temp_plugin",
      .stage = MOP_SHADER_PLUGIN_OVERLAY,
      .draw = test_draw_fn,
  };
  MopShaderPlugin *p = mop_viewport_register_shader(vp, &desc);
  TEST_ASSERT(p != NULL);

  /* Unregister — should not crash */
  mop_viewport_unregister_shader(vp, p);

  /* Double unregister should be safe (no-op) */
  mop_viewport_unregister_shader(vp, NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_plugin_invoked_on_render(void) {
  TEST_BEGIN("plugin_invoked_on_render");
  MopViewport *vp = make_viewport();

  MopShaderPluginDesc desc = {
      .name = "counter",
      .stage = MOP_SHADER_PLUGIN_POST_OPAQUE,
      .draw = test_draw_fn,
  };
  MopShaderPlugin *p = mop_viewport_register_shader(vp, &desc);
  TEST_ASSERT(p != NULL);

  s_draw_called = 0;
  mop_viewport_render(vp);
  TEST_ASSERT(s_draw_called == 1);

  /* Context should carry viewport dimensions */
  TEST_ASSERT(s_draw_width == 64);
  TEST_ASSERT(s_draw_height == 64);

  /* Render again */
  mop_viewport_render(vp);
  TEST_ASSERT(s_draw_called == 2);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_multiple_plugins(void) {
  TEST_BEGIN("multiple_plugins");
  MopViewport *vp = make_viewport();

  s_draw_called = 0;

  MopShaderPluginDesc desc1 = {
      .name = "plugin_1",
      .stage = MOP_SHADER_PLUGIN_POST_OPAQUE,
      .draw = test_draw_fn,
  };
  MopShaderPluginDesc desc2 = {
      .name = "plugin_2",
      .stage = MOP_SHADER_PLUGIN_POST_SCENE,
      .draw = test_draw_fn,
  };
  MopShaderPluginDesc desc3 = {
      .name = "plugin_3",
      .stage = MOP_SHADER_PLUGIN_OVERLAY,
      .draw = test_draw_fn,
  };

  TEST_ASSERT(mop_viewport_register_shader(vp, &desc1) != NULL);
  TEST_ASSERT(mop_viewport_register_shader(vp, &desc2) != NULL);
  TEST_ASSERT(mop_viewport_register_shader(vp, &desc3) != NULL);

  mop_viewport_render(vp);
  TEST_ASSERT(s_draw_called == 3);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_all_stages(void) {
  TEST_BEGIN("all_stages");
  MopViewport *vp = make_viewport();

  /* Register one plugin per stage */
  for (int s = 0; s < MOP_SHADER_PLUGIN_STAGE_COUNT; s++) {
    MopShaderPluginDesc desc = {
        .name = "stage_test",
        .stage = (MopShaderPluginStage)s,
        .draw = test_draw_fn,
    };
    MopShaderPlugin *p = mop_viewport_register_shader(vp, &desc);
    TEST_ASSERT(p != NULL);
  }

  s_draw_called = 0;
  mop_viewport_render(vp);
  TEST_ASSERT(s_draw_called == MOP_SHADER_PLUGIN_STAGE_COUNT);

  mop_viewport_destroy(vp);
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("shader_plugin");

  TEST_RUN(test_register_null_viewport);
  TEST_RUN(test_register_null_desc);
  TEST_RUN(test_register_no_name);
  TEST_RUN(test_register_success);
  TEST_RUN(test_unregister);
  TEST_RUN(test_plugin_invoked_on_render);
  TEST_RUN(test_multiple_plugins);
  TEST_RUN(test_all_stages);

  TEST_REPORT();
  TEST_EXIT();
}
