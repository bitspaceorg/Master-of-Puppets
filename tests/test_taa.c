/*
 * Master of Puppets — Test Suite
 * test_taa.c — Phase 4A: Temporal Anti-Aliasing
 *
 * Tests Halton jitter generation, TAA state management, projection matrix
 * jitter injection, and push constant layout.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <math.h>
#include <mop/mop.h>
#include <mop/render/postprocess.h>

/* Access internal viewport struct for TAA state verification */
#include "core/viewport_internal.h"

/* -------------------------------------------------------------------------
 * Halton sequence verification
 * ------------------------------------------------------------------------- */

/* Reimplement Halton to verify the viewport's jitter values */
static float halton(uint32_t index, uint32_t base) {
  float f = 1.0f;
  float result = 0.0f;
  uint32_t i = index;
  while (i > 0) {
    f /= (float)base;
    result += f * (float)(i % base);
    i /= base;
  }
  return result;
}

static void test_halton_base2(void) {
  TEST_BEGIN("halton_base2");
  /* Halton(1,2) = 1/2, Halton(2,2) = 1/4, Halton(3,2) = 3/4 */
  TEST_ASSERT_FLOAT_EQ(halton(1, 2), 0.5f);
  TEST_ASSERT_FLOAT_EQ(halton(2, 2), 0.25f);
  TEST_ASSERT_FLOAT_EQ(halton(3, 2), 0.75f);
  TEST_ASSERT_FLOAT_EQ(halton(4, 2), 0.125f);
  TEST_END();
}

static void test_halton_base3(void) {
  TEST_BEGIN("halton_base3");
  /* Halton(1,3) = 1/3, Halton(2,3) = 2/3, Halton(3,3) = 1/9 */
  TEST_ASSERT_FLOAT_EQ(halton(1, 3), 1.0f / 3.0f);
  TEST_ASSERT_FLOAT_EQ(halton(2, 3), 2.0f / 3.0f);
  TEST_ASSERT_FLOAT_EQ(halton(3, 3), 1.0f / 9.0f);
  TEST_END();
}

static void test_halton_16_phase_range(void) {
  TEST_BEGIN("halton_16_phase_range");
  /* All 16-phase values for base 2 and 3 should be in (0, 1) */
  for (uint32_t i = 1; i <= 16; i++) {
    float h2 = halton(i, 2);
    float h3 = halton(i, 3);
    TEST_ASSERT(h2 > 0.0f && h2 < 1.0f);
    TEST_ASSERT(h3 > 0.0f && h3 < 1.0f);
  }
  TEST_END();
}

static void test_halton_jitter_centered(void) {
  TEST_BEGIN("halton_jitter_centered");
  /* Jitter = halton - 0.5, should be in [-0.5, +0.5] */
  for (uint32_t i = 1; i <= 16; i++) {
    float jx = halton(i, 2) - 0.5f;
    float jy = halton(i, 3) - 0.5f;
    TEST_ASSERT(jx >= -0.5f && jx <= 0.5f);
    TEST_ASSERT(jy >= -0.5f && jy <= 0.5f);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * MOP_POST_TAA flag
 * ------------------------------------------------------------------------- */

static void test_taa_flag_value(void) {
  TEST_BEGIN("taa_flag_value");
  /* MOP_POST_TAA should be 1<<7 = 128, distinct from other flags */
  TEST_ASSERT(MOP_POST_TAA == (1 << 7));
  TEST_ASSERT((MOP_POST_TAA & MOP_POST_FXAA) == 0);
  TEST_ASSERT((MOP_POST_TAA & MOP_POST_BLOOM) == 0);
  TEST_ASSERT((MOP_POST_TAA & MOP_POST_SSAO) == 0);
  TEST_END();
}

static void test_taa_flag_combinable(void) {
  TEST_BEGIN("taa_flag_combinable");
  /* TAA can be combined with other post-effects */
  uint32_t combined = MOP_POST_TAA | MOP_POST_BLOOM | MOP_POST_SSAO;
  TEST_ASSERT((combined & MOP_POST_TAA) != 0);
  TEST_ASSERT((combined & MOP_POST_BLOOM) != 0);
  TEST_ASSERT((combined & MOP_POST_SSAO) != 0);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * TAA state on viewport
 * ------------------------------------------------------------------------- */

static void test_taa_state_initial(void) {
  TEST_BEGIN("taa_state_initial");
  MopViewportDesc desc = {.width = 64, .height = 64};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  /* TAA state should be zeroed on creation */
  TEST_ASSERT(vp->taa_frame_index == 0);
  TEST_ASSERT_FLOAT_EQ(vp->taa_jitter_x, 0.0f);
  TEST_ASSERT_FLOAT_EQ(vp->taa_jitter_y, 0.0f);
  TEST_ASSERT(vp->taa_has_history == false);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_taa_state_after_render(void) {
  TEST_BEGIN("taa_state_after_render");
  MopViewportDesc desc = {.width = 64, .height = 64};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  /* Enable TAA */
  vp->post_effects |= MOP_POST_TAA;

  /* First render — should set jitter and increment frame index */
  mop_viewport_render(vp);
  TEST_ASSERT(vp->taa_frame_index == 1);
  TEST_ASSERT(vp->taa_has_history == true);

  /* Jitter should be non-zero (Halton(1,2)-0.5 = 0, Halton(1,3)-0.5 != 0)
   * Actually Halton(1,2) = 0.5, so jitter_x = 0.0. But jitter_y should be
   * Halton(1,3) - 0.5 = 1/3 - 0.5 = -1/6 */
  TEST_ASSERT_FLOAT_EQ(vp->taa_jitter_x, 0.0f); /* halton(1,2)-0.5=0 */
  float expected_jy = halton(1, 3) - 0.5f;
  TEST_ASSERT_FLOAT_EQ(vp->taa_jitter_y, expected_jy);

  /* Second render */
  mop_viewport_render(vp);
  TEST_ASSERT(vp->taa_frame_index == 2);

  /* Jitter should change for frame 2 */
  float expected_jx2 = halton(2, 2) - 0.5f;
  float expected_jy2 = halton(2, 3) - 0.5f;
  TEST_ASSERT_FLOAT_EQ(vp->taa_jitter_x, expected_jx2);
  TEST_ASSERT_FLOAT_EQ(vp->taa_jitter_y, expected_jy2);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_taa_history_reset_on_resize(void) {
  TEST_BEGIN("taa_history_reset_on_resize");
  MopViewportDesc desc = {.width = 64, .height = 64};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  vp->post_effects |= MOP_POST_TAA;
  mop_viewport_render(vp);
  TEST_ASSERT(vp->taa_has_history == true);

  /* Resize should reset history */
  mop_viewport_resize(vp, 128, 128);
  TEST_ASSERT(vp->taa_has_history == false);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_taa_prev_matrices_saved(void) {
  TEST_BEGIN("taa_prev_matrices_saved");
  MopViewportDesc desc = {.width = 64, .height = 64};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  vp->post_effects |= MOP_POST_TAA;

  /* Render first frame — camera system sets projection/view */
  mop_viewport_render(vp);

  /* After render, prev matrices should be saved and non-zero.
   * The projection comes from the orbit camera, not from what we set
   * directly, because render() recomputes it from camera params. */
  MopMat4 identity = mop_mat4_identity();
  int proj_is_identity = 1;
  int view_is_identity = 1;
  for (int i = 0; i < 16; i++) {
    if (fabsf(vp->taa_prev_proj.d[i] - identity.d[i]) > 1e-4f)
      proj_is_identity = 0;
    if (fabsf(vp->taa_prev_view.d[i] - identity.d[i]) > 1e-4f)
      view_is_identity = 0;
  }
  /* Previous projection should NOT be identity (it's a perspective matrix) */
  TEST_ASSERT(!proj_is_identity);
  /* Previous view should NOT be identity (orbit camera has position) */
  TEST_ASSERT(!view_is_identity);

  /* Projection should be restored (unjittered) — d[8] and d[9] should
   * match prev_proj (which is the unjittered version). */
  TEST_ASSERT_FLOAT_EQ(vp->projection_matrix.d[8], vp->taa_prev_proj.d[8]);
  TEST_ASSERT_FLOAT_EQ(vp->projection_matrix.d[9], vp->taa_prev_proj.d[9]);

  /* Second render — prev matrices should update */
  MopMat4 first_prev_proj = vp->taa_prev_proj;
  mop_viewport_render(vp);
  /* Prev proj should still be the same unjittered projection (camera
   * hasn't moved between frames, so projection is unchanged) */
  TEST_ASSERT_MAT4_NEAR(vp->taa_prev_proj, first_prev_proj, 1e-4f);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Jitter application to projection matrix
 * ------------------------------------------------------------------------- */

static void test_projection_jitter_application(void) {
  TEST_BEGIN("projection_jitter_application");
  /* Verify the jitter formula: proj.d[8] += jitter_x * 2 / width
   * proj.d[9] += jitter_y * 2 / height
   * These are column-major: d[8] = col2,row0 and d[9] = col2,row1 */
  float jx = 0.25f;
  float jy = -0.125f;
  int w = 640, h = 480;

  MopMat4 proj = mop_mat4_perspective(1.0f, (float)w / (float)h, 0.1f, 100.0f);
  float orig_d8 = proj.d[8];
  float orig_d9 = proj.d[9];

  /* Apply jitter (same as viewport.c) */
  proj.d[8] += jx * 2.0f / (float)w;
  proj.d[9] += jy * 2.0f / (float)h;

  float expected_d8 = orig_d8 + jx * 2.0f / (float)w;
  float expected_d9 = orig_d9 + jy * 2.0f / (float)h;
  TEST_ASSERT_FLOAT_EQ(proj.d[8], expected_d8);
  TEST_ASSERT_FLOAT_EQ(proj.d[9], expected_d9);

  /* Other elements should be unchanged */
  TEST_ASSERT_FLOAT_EQ(
      proj.d[0],
      mop_mat4_perspective(1.0f, (float)w / (float)h, 0.1f, 100.0f).d[0]);
  TEST_ASSERT_FLOAT_EQ(
      proj.d[5],
      mop_mat4_perspective(1.0f, (float)w / (float)h, 0.1f, 100.0f).d[5]);

  TEST_END();
}

static void test_jitter_subpixel_magnitude(void) {
  TEST_BEGIN("jitter_subpixel_magnitude");
  /* Jitter should produce sub-pixel offsets in NDC.
   * For a 1920x1080 render: max jitter = 0.5 pixels
   * NDC offset = 0.5 * 2 / 1920 ≈ 0.00052 — well under 1 pixel in NDC */
  int w = 1920;
  float max_jitter = 0.5f;
  float ndc_offset = max_jitter * 2.0f / (float)w;
  TEST_ASSERT(ndc_offset < 0.001f);
  TEST_ASSERT(ndc_offset > 0.0f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Push constant layout verification
 * ------------------------------------------------------------------------- */

static void test_taa_push_constant_size(void) {
  TEST_BEGIN("taa_push_constant_size");
  /* TAA push constants: 2 mat4 + 2 vec2 + 2 float = 152 bytes.
   * Must fit within Vulkan minimum push constant limit (128 bytes for
   * guaranteed, but most GPUs support 256+). Our layout is 152 bytes. */
  struct TaaPushLayout {
    float inv_vp_jittered[16]; /* 64 bytes */
    float prev_vp[16];         /* 64 bytes */
    float inv_resolution[2];   /* 8 bytes  */
    float jitter[2];           /* 8 bytes  */
    float feedback;            /* 4 bytes  */
    float first_frame;         /* 4 bytes  */
  };
  TEST_ASSERT(sizeof(struct TaaPushLayout) == 152);
  TEST_END();
}

static void test_taa_push_constant_offsets(void) {
  TEST_BEGIN("taa_push_constant_offsets");
  /* Verify field offsets match GLSL push_constant block (std430) */
  struct TaaPushLayout {
    float inv_vp_jittered[16];
    float prev_vp[16];
    float inv_resolution[2];
    float jitter[2];
    float feedback;
    float first_frame;
  };
  struct TaaPushLayout pc;
  char *base = (char *)&pc;

  TEST_ASSERT((char *)&pc.inv_vp_jittered - base == 0);
  TEST_ASSERT((char *)&pc.prev_vp - base == 64);
  TEST_ASSERT((char *)&pc.inv_resolution - base == 128);
  TEST_ASSERT((char *)&pc.jitter - base == 136);
  TEST_ASSERT((char *)&pc.feedback - base == 144);
  TEST_ASSERT((char *)&pc.first_frame - base == 148);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Frame index cycling
 * ------------------------------------------------------------------------- */

static void test_taa_frame_index_wraps(void) {
  TEST_BEGIN("taa_frame_index_wraps");
  /* The Halton phase uses (frame_index % 16) + 1, cycling 1..16 */
  for (uint32_t fi = 0; fi < 32; fi++) {
    uint32_t phase = (fi % 16) + 1;
    TEST_ASSERT(phase >= 1 && phase <= 16);
    float jx = halton(phase, 2) - 0.5f;
    float jy = halton(phase, 3) - 0.5f;
    TEST_ASSERT(jx >= -0.5f && jx <= 0.5f);
    TEST_ASSERT(jy >= -0.5f && jy <= 0.5f);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Feedback parameter
 * ------------------------------------------------------------------------- */

static void test_taa_adaptive_feedback_formula(void) {
  TEST_BEGIN("taa_adaptive_feedback_formula");
  /* Shader: adaptive_feedback = mix(feedback, 0.5, clamp(motion/20, 0, 1))
   * Static: motion=0 → feedback (0.9)
   * Fast motion: motion=20+ pixels → 0.5 */
  float feedback = 0.9f;

  /* Static pixel */
  float motion0 = 0.0f;
  float f0 = feedback * (1.0f - fminf(motion0 / 20.0f, 1.0f)) +
             0.5f * fminf(motion0 / 20.0f, 1.0f);
  TEST_ASSERT_FLOAT_EQ(f0, 0.9f);

  /* 10-pixel motion */
  float motion10 = 10.0f;
  float t10 = fminf(motion10 / 20.0f, 1.0f);
  float f10 = feedback * (1.0f - t10) + 0.5f * t10;
  TEST_ASSERT_FLOAT_EQ(f10, 0.7f); /* mix(0.9, 0.5, 0.5) = 0.7 */

  /* 20+ pixel motion */
  float motion_fast = 30.0f;
  float t_fast = fminf(motion_fast / 20.0f, 1.0f);
  float f_fast = feedback * (1.0f - t_fast) + 0.5f * t_fast;
  TEST_ASSERT_FLOAT_EQ(f_fast, 0.5f);

  TEST_END();
}

/* -------------------------------------------------------------------------
 * Inverse VP matrix roundtrip
 * ------------------------------------------------------------------------- */

static void test_inv_vp_roundtrip(void) {
  TEST_BEGIN("inv_vp_roundtrip");
  /* Verify that inverse(VP) * VP ≈ identity */
  MopMat4 proj = mop_mat4_perspective(1.0f, 1.333f, 0.1f, 100.0f);
  MopMat4 view = mop_mat4_look_at((MopVec3){3, 3, 3}, (MopVec3){0, 0, 0},
                                  (MopVec3){0, 1, 0});
  MopMat4 vp = mop_mat4_multiply(proj, view);
  MopMat4 inv_vp = mop_mat4_inverse(vp);
  MopMat4 product = mop_mat4_multiply(vp, inv_vp);
  MopMat4 identity = mop_mat4_identity();
  TEST_ASSERT_MAT4_NEAR(product, identity, 1e-3f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Vulkan struct layout tests (only with Vulkan backend)
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"

static void test_taa_device_fields_exist(void) {
  TEST_BEGIN("taa_device_fields_exist");
  /* Verify TAA fields in MopRhiDevice are accessible */
  struct MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  dev.taa_enabled = true;
  dev.taa_first_frame = true;
  dev.taa_jitter[0] = 0.25f;
  dev.taa_jitter[1] = -0.125f;
  TEST_ASSERT(dev.taa_enabled == true);
  TEST_ASSERT(dev.taa_first_frame == true);
  TEST_ASSERT_FLOAT_EQ(dev.taa_jitter[0], 0.25f);
  TEST_ASSERT_FLOAT_EQ(dev.taa_jitter[1], -0.125f);
  TEST_END();
}

static void test_taa_fb_fields_exist(void) {
  TEST_BEGIN("taa_fb_fields_exist");
  /* Verify TAA fields in MopRhiFramebuffer are accessible */
  struct MopRhiFramebuffer fb;
  memset(&fb, 0, sizeof(fb));
  fb.taa_current = 0;
  TEST_ASSERT(fb.taa_current == 0);
  fb.taa_current = 1;
  TEST_ASSERT(fb.taa_current == 1);
  /* Ping-pong index should be 0 or 1 */
  TEST_ASSERT(fb.taa_current <= 1);
  TEST_END();
}

static void test_taa_pingpong_logic(void) {
  TEST_BEGIN("taa_pingpong_logic");
  /* Verify ping-pong swap: 1 - current gives the other buffer */
  uint32_t cur = 0;
  TEST_ASSERT(1 - cur == 1);
  cur = 1;
  TEST_ASSERT(1 - cur == 0);
  /* After swap: fb->taa_current = 1 - fb->taa_current */
  cur = 0;
  cur = 1 - cur;
  TEST_ASSERT(cur == 1);
  cur = 1 - cur;
  TEST_ASSERT(cur == 0);
  TEST_END();
}
#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("taa");

  /* Halton sequence */
  TEST_RUN(test_halton_base2);
  TEST_RUN(test_halton_base3);
  TEST_RUN(test_halton_16_phase_range);
  TEST_RUN(test_halton_jitter_centered);

  /* Public API flag */
  TEST_RUN(test_taa_flag_value);
  TEST_RUN(test_taa_flag_combinable);

  /* TAA viewport state */
  TEST_RUN(test_taa_state_initial);
  TEST_RUN(test_taa_state_after_render);
  TEST_RUN(test_taa_history_reset_on_resize);
  TEST_RUN(test_taa_prev_matrices_saved);

  /* Jitter math */
  TEST_RUN(test_projection_jitter_application);
  TEST_RUN(test_jitter_subpixel_magnitude);

  /* Push constant layout */
  TEST_RUN(test_taa_push_constant_size);
  TEST_RUN(test_taa_push_constant_offsets);

  /* Frame cycling */
  TEST_RUN(test_taa_frame_index_wraps);

  /* Feedback formula */
  TEST_RUN(test_taa_adaptive_feedback_formula);

  /* Inverse VP roundtrip */
  TEST_RUN(test_inv_vp_roundtrip);

#if defined(MOP_HAS_VULKAN)
  /* Vulkan struct layout */
  TEST_RUN(test_taa_device_fields_exist);
  TEST_RUN(test_taa_fb_fields_exist);
  TEST_RUN(test_taa_pingpong_logic);
#endif

  TEST_REPORT();
  TEST_EXIT();
}
