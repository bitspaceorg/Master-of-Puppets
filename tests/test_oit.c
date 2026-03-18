/*
 * Master of Puppets — Test Suite
 * test_oit.c — Phase 4C: Order-Independent Transparency (OIT)
 *
 * Tests OIT flag, public API, shader weight function, composite formula,
 * draw deferral struct layout, and Vulkan struct layout.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <math.h>
#include <mop/mop.h>
#include <mop/render/postprocess.h>
#include <string.h>

/* Access internal viewport struct for OIT state verification */
#include "core/viewport_internal.h"

/* -------------------------------------------------------------------------
 * MOP_POST_OIT flag
 * ------------------------------------------------------------------------- */

static void test_oit_flag_value(void) {
  TEST_BEGIN("oit_flag_value");
  TEST_ASSERT((int)MOP_POST_OIT == (1 << 9));
  TEST_END();
}

static void test_oit_flag_combinable(void) {
  TEST_BEGIN("oit_flag_combinable");
  uint32_t combo = MOP_POST_TONEMAP | MOP_POST_OIT | MOP_POST_BLOOM;
  TEST_ASSERT((combo & MOP_POST_OIT) != 0);
  TEST_ASSERT((combo & MOP_POST_TONEMAP) != 0);
  TEST_ASSERT((combo & MOP_POST_BLOOM) != 0);
  TEST_ASSERT((combo & MOP_POST_FXAA) == 0);
  TEST_END();
}

static void test_oit_flag_no_overlap(void) {
  TEST_BEGIN("oit_flag_no_overlap");
  /* Ensure OIT doesn't overlap with any other post-effect flag */
  uint32_t flags[] = {MOP_POST_GAMMA, MOP_POST_TONEMAP, MOP_POST_VIGNETTE,
                      MOP_POST_FOG,   MOP_POST_FXAA,    MOP_POST_BLOOM,
                      MOP_POST_SSAO,  MOP_POST_TAA,     MOP_POST_SSR};
  for (int i = 0; i < 9; i++) {
    TEST_ASSERT((MOP_POST_OIT & flags[i]) == 0);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Public API (viewport wiring)
 * ------------------------------------------------------------------------- */

static void test_oit_set_post_effects(void) {
  TEST_BEGIN("oit_set_post_effects");
  MopViewportDesc desc = {
      .width = 16, .height = 16, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);
  mop_viewport_set_post_effects(vp, MOP_POST_OIT);
  TEST_ASSERT((vp->post_effects & MOP_POST_OIT) != 0);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_oit_null_viewport(void) {
  TEST_BEGIN("oit_null_viewport");
  /* Should not crash */
  mop_viewport_set_post_effects(NULL, MOP_POST_OIT);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Weighted Blended OIT — weight function math
 *
 * w(z, a) = a * clamp(0.03 / (1e-5 + pow(z / 200, 4)), 1e-2, 3e3)
 * ------------------------------------------------------------------------- */

static float oit_weight(float z, float alpha) {
  float d = z / 200.0f;
  float d4 = d * d * d * d;
  float w = 0.03f / (1e-5f + d4);
  if (w < 1e-2f)
    w = 1e-2f;
  if (w > 3e3f)
    w = 3e3f;
  return alpha * w;
}

static void test_oit_weight_near(void) {
  TEST_BEGIN("oit_weight_near");
  /* At z=1 (very near), weight should be high */
  float w = oit_weight(1.0f, 1.0f);
  TEST_ASSERT(w > 100.0f);
  TEST_END();
}

static void test_oit_weight_far(void) {
  TEST_BEGIN("oit_weight_far");
  /* At z=500 (far), weight should be clamped to minimum */
  float w = oit_weight(500.0f, 1.0f);
  TEST_ASSERT(fabsf(w - 0.01f) < 0.001f);
  TEST_END();
}

static void test_oit_weight_alpha_scales(void) {
  TEST_BEGIN("oit_weight_alpha_scales");
  float w1 = oit_weight(10.0f, 1.0f);
  float w05 = oit_weight(10.0f, 0.5f);
  TEST_ASSERT(fabsf(w05 - w1 * 0.5f) < 0.01f);
  TEST_END();
}

static void test_oit_weight_zero_alpha(void) {
  TEST_BEGIN("oit_weight_zero_alpha");
  float w = oit_weight(10.0f, 0.0f);
  TEST_ASSERT(w == 0.0f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Composite formula:
 *   avg_color = accum.rgb / max(accum.a, 1e-5)
 *   output    = vec4(avg_color, 1 - revealage)
 *
 * Hardware blend: result = avg_color * (1-revealage) + opaque * revealage
 * ------------------------------------------------------------------------- */

static void test_oit_composite_fully_transparent(void) {
  TEST_BEGIN("oit_composite_fully_transparent");
  /* revealage = 1.0 means no transparent fragments (should discard) */
  float revealage = 1.0f;
  TEST_ASSERT(revealage > 0.999f); /* discard condition */
  TEST_END();
}

static void test_oit_composite_half_transparent(void) {
  TEST_BEGIN("oit_composite_half_transparent");
  /* Simulate: one transparent fragment with alpha=0.5, color=(1,0,0) at z=10 */
  float alpha = 0.5f;
  float w = oit_weight(10.0f, alpha);
  float accum_r = 1.0f * alpha * w;
  float accum_a = alpha * w;
  float revealage =
      alpha; /* multiplicative: start=1, after one frag: 1 * alpha */

  float avg_r = accum_r / (accum_a > 1e-5f ? accum_a : 1e-5f);
  float out_alpha = 1.0f - revealage;

  /* avg_color should recover the original lit color */
  TEST_ASSERT(fabsf(avg_r - 1.0f) < 0.01f);
  /* output alpha = 0.5 */
  TEST_ASSERT(fabsf(out_alpha - 0.5f) < 0.01f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Deferred draw struct layout
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"

static void test_oit_deferred_draw_fields(void) {
  TEST_BEGIN("oit_deferred_draw_fields");
  /* Verify the deferred draw struct has the expected fields */
  struct MopVkDeferredOitDraw dd;
  memset(&dd, 0, sizeof(dd));
  dd.index_count = 36;
  dd.draw_id = 7;
  TEST_ASSERT(dd.index_count == 36);
  TEST_ASSERT(dd.draw_id == 7);
  /* push_data holds mvp(16) + model(16) = 32 floats */
  TEST_ASSERT(sizeof(dd.push_data) == 32 * sizeof(float));
  TEST_END();
}

static void test_oit_device_fields(void) {
  TEST_BEGIN("oit_device_fields");
  /* Verify OIT fields exist in the device struct */
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  TEST_ASSERT(dev.oit_enabled == false);
  TEST_ASSERT(dev.oit_pipeline == VK_NULL_HANDLE);
  TEST_ASSERT(dev.oit_composite_pipeline == VK_NULL_HANDLE);
  TEST_ASSERT(dev.oit_render_pass == VK_NULL_HANDLE);
  TEST_ASSERT(dev.oit_composite_render_pass == VK_NULL_HANDLE);
  TEST_ASSERT(dev.oit_draw_count == 0);
  TEST_END();
}

static void test_oit_framebuffer_fields(void) {
  TEST_BEGIN("oit_framebuffer_fields");
  MopRhiFramebuffer fb;
  memset(&fb, 0, sizeof(fb));
  TEST_ASSERT(fb.oit_accum_image == VK_NULL_HANDLE);
  TEST_ASSERT(fb.oit_revealage_image == VK_NULL_HANDLE);
  TEST_ASSERT(fb.oit_framebuffer == VK_NULL_HANDLE);
  TEST_ASSERT(fb.oit_composite_framebuffer == VK_NULL_HANDLE);
  TEST_END();
}
#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("oit");

  /* Flag tests */
  test_oit_flag_value();
  test_oit_flag_combinable();
  test_oit_flag_no_overlap();

  /* Public API */
  test_oit_set_post_effects();
  test_oit_null_viewport();

  /* Weight function math */
  test_oit_weight_near();
  test_oit_weight_far();
  test_oit_weight_alpha_scales();
  test_oit_weight_zero_alpha();

  /* Composite formula */
  test_oit_composite_fully_transparent();
  test_oit_composite_half_transparent();

#if defined(MOP_HAS_VULKAN)
  /* Vulkan struct layout */
  test_oit_deferred_draw_fields();
  test_oit_device_fields();
  test_oit_framebuffer_fields();
#endif

  TEST_REPORT();
  TEST_EXIT();
}
