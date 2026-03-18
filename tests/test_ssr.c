/*
 * Master of Puppets — Test Suite
 * test_ssr.c — Phase 4B: Screen-Space Reflections
 *
 * Tests SSR flag, public API, push constant layout, shader math,
 * and Vulkan struct layout.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <math.h>
#include <mop/mop.h>
#include <mop/render/postprocess.h>
#include <string.h>

/* Access internal viewport struct for SSR state verification */
#include "core/viewport_internal.h"

/* -------------------------------------------------------------------------
 * MOP_POST_SSR flag
 * ------------------------------------------------------------------------- */

static void test_ssr_flag_value(void) {
  TEST_BEGIN("ssr_flag_value");
  TEST_ASSERT((int)MOP_POST_SSR == (1 << 8));
  TEST_END();
}

static void test_ssr_flag_combinable(void) {
  TEST_BEGIN("ssr_flag_combinable");
  uint32_t all = MOP_POST_TONEMAP | MOP_POST_BLOOM | MOP_POST_SSAO |
                 MOP_POST_TAA | MOP_POST_SSR;
  /* All bits distinct — no overlaps */
  TEST_ASSERT((MOP_POST_SSR & MOP_POST_TAA) == 0);
  TEST_ASSERT((MOP_POST_SSR & MOP_POST_SSAO) == 0);
  TEST_ASSERT((MOP_POST_SSR & MOP_POST_BLOOM) == 0);
  /* Combined still has SSR */
  TEST_ASSERT((all & MOP_POST_SSR) != 0);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

static void test_ssr_default_intensity(void) {
  TEST_BEGIN("ssr_default_intensity");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);
  TEST_ASSERT_FLOAT_EQ(vp->ssr_intensity, 0.5f);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_ssr_set_intensity(void) {
  TEST_BEGIN("ssr_set_intensity");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);
  mop_viewport_set_ssr(vp, 0.8f);
  TEST_ASSERT_FLOAT_EQ(vp->ssr_intensity, 0.8f);
  /* Clamp negative to 0 */
  mop_viewport_set_ssr(vp, -1.0f);
  TEST_ASSERT_FLOAT_EQ(vp->ssr_intensity, 0.0f);
  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_ssr_set_null_viewport(void) {
  TEST_BEGIN("ssr_set_null_viewport");
  /* Should not crash */
  mop_viewport_set_ssr(NULL, 0.5f);
  TEST_END();
}

static void test_ssr_post_effect_propagation(void) {
  TEST_BEGIN("ssr_post_effect_propagation");
  MopViewportDesc desc = {
      .width = 64, .height = 64, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);
  /* Enable SSR via post effects */
  mop_viewport_set_post_effects(vp, MOP_POST_SSR | MOP_POST_TONEMAP);
  TEST_ASSERT((vp->post_effects & MOP_POST_SSR) != 0);
  /* Disable SSR */
  mop_viewport_set_post_effects(vp, MOP_POST_TONEMAP);
  TEST_ASSERT((vp->post_effects & MOP_POST_SSR) == 0);
  mop_viewport_destroy(vp);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * SSR push constant layout
 * ------------------------------------------------------------------------- */

static void test_ssr_push_constant_size(void) {
  TEST_BEGIN("ssr_push_constant_size");
  /* Push constant block: 2×mat4 + vec2 + int + 4×float + pad = 156 bytes */
  struct SsrPush {
    float projection[16];
    float inv_projection[16];
    float inv_resolution[2];
    int32_t reverse_z;
    float max_distance;
    float thickness;
    float intensity;
    float _pad;
  };
  TEST_ASSERT(sizeof(struct SsrPush) == 156);
  TEST_END();
}

static void test_ssr_push_constant_offsets(void) {
  TEST_BEGIN("ssr_push_constant_offsets");
  struct SsrPush {
    float projection[16];     /* offset   0 */
    float inv_projection[16]; /* offset  64 */
    float inv_resolution[2];  /* offset 128 */
    int32_t reverse_z;        /* offset 136 */
    float max_distance;       /* offset 140 */
    float thickness;          /* offset 144 */
    float intensity;          /* offset 148 */
    float _pad;               /* offset 152 */
  };

#define OFFSET_OF(s, m) ((size_t)&(((s *)0)->m))
  TEST_ASSERT(OFFSET_OF(struct SsrPush, projection) == 0);
  TEST_ASSERT(OFFSET_OF(struct SsrPush, inv_projection) == 64);
  TEST_ASSERT(OFFSET_OF(struct SsrPush, inv_resolution) == 128);
  TEST_ASSERT(OFFSET_OF(struct SsrPush, reverse_z) == 136);
  TEST_ASSERT(OFFSET_OF(struct SsrPush, max_distance) == 140);
  TEST_ASSERT(OFFSET_OF(struct SsrPush, thickness) == 144);
  TEST_ASSERT(OFFSET_OF(struct SsrPush, intensity) == 148);
#undef OFFSET_OF
  TEST_END();
}

/* -------------------------------------------------------------------------
 * SSR shader math helpers (test view_pos_from_depth reconstruction)
 * ------------------------------------------------------------------------- */

/* Reconstruct a view-space position from depth + UV using inverse projection.
 * This mirrors the GLSL view_pos_from_depth function in mop_ssr.frag. */
static void view_pos_from_depth(const float inv_proj[16], float u, float v,
                                float depth, int reverse_z, float out[3]) {
  float z_ndc = reverse_z ? (1.0f - depth) * 2.0f - 1.0f : depth * 2.0f - 1.0f;
  float clip[4] = {u * 2.0f - 1.0f, v * 2.0f - 1.0f, z_ndc, 1.0f};
  /* inv_proj * clip (column-major multiply) */
  float view[4] = {0};
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      view[r] += inv_proj[c * 4 + r] * clip[c];
    }
  }
  out[0] = view[0] / view[3];
  out[1] = view[1] / view[3];
  out[2] = view[2] / view[3];
}

static void test_ssr_depth_reconstruction_center(void) {
  TEST_BEGIN("ssr_depth_reconstruction_center");
  /* Create a simple perspective projection and its inverse */
  MopMat4 proj =
      mop_mat4_perspective(60.0f * 3.14159f / 180.0f, 1.0f, 0.1f, 100.0f);
  MopMat4 inv = mop_mat4_inverse(proj);

  /* A point at center screen (0.5, 0.5) with a specific depth should
   * reconstruct to (0, 0, -z_view) roughly */
  float pos[3];
  /* Forward-Z: NDC z = (A*z_view + B) / (-z_view) where A,B from proj */
  /* For a point at z_view = -5 (5 units in front of camera) */
  float z_view = -5.0f;
  float ndc_z = (proj.d[10] * z_view + proj.d[14]) / (-z_view);
  float raw_depth = ndc_z * 0.5f + 0.5f;
  view_pos_from_depth(inv.d, 0.5f, 0.5f, raw_depth, 0, pos);
  /* Center pixel should reconstruct to roughly (0, 0, -5) */
  TEST_ASSERT(fabsf(pos[0]) < 0.01f);
  TEST_ASSERT(fabsf(pos[1]) < 0.01f);
  TEST_ASSERT(fabsf(pos[2] - z_view) < 0.01f);
  TEST_END();
}

static void test_ssr_depth_reconstruction_reverse_z(void) {
  TEST_BEGIN("ssr_depth_reconstruction_reverse_z");
  /* Reverse-Z: near=1.0, far=0.0 in NDC */
  MopMat4 proj =
      mop_mat4_perspective(60.0f * 3.14159f / 180.0f, 1.0f, 0.1f, 100.0f);
  MopMat4 inv = mop_mat4_inverse(proj);
  float z_view = -5.0f;
  float ndc_z = (proj.d[10] * z_view + proj.d[14]) / (-z_view);
  /* In reverse-Z, raw_depth = 1 - (ndc_z * 0.5 + 0.5) */
  float raw_depth = 1.0f - (ndc_z * 0.5f + 0.5f);
  float pos[3];
  view_pos_from_depth(inv.d, 0.5f, 0.5f, raw_depth, 1, pos);
  TEST_ASSERT(fabsf(pos[0]) < 0.01f);
  TEST_ASSERT(fabsf(pos[1]) < 0.01f);
  TEST_ASSERT(fabsf(pos[2] - z_view) < 0.01f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Screen edge fade function
 * ------------------------------------------------------------------------- */

static float screen_edge_fade(float u, float v) {
  /* smoothstep(0, 0.1, x) * (1 - smoothstep(0.9, 1.0, x)) for each axis */
  float fade_x, fade_y;
  /* smoothstep */
  {
    float t = (u - 0.0f) / (0.1f - 0.0f);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float s1 = t * t * (3.0f - 2.0f * t);
    t = (u - 0.9f) / (1.0f - 0.9f);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float s2 = t * t * (3.0f - 2.0f * t);
    fade_x = s1 * (1.0f - s2);
  }
  {
    float t = (v - 0.0f) / (0.1f - 0.0f);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float s1 = t * t * (3.0f - 2.0f * t);
    t = (v - 0.9f) / (1.0f - 0.9f);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float s2 = t * t * (3.0f - 2.0f * t);
    fade_y = s1 * (1.0f - s2);
  }
  return fade_x * fade_y;
}

static void test_ssr_edge_fade_center(void) {
  TEST_BEGIN("ssr_edge_fade_center");
  /* Center of screen: no fade */
  float f = screen_edge_fade(0.5f, 0.5f);
  TEST_ASSERT(fabsf(f - 1.0f) < 0.001f);
  TEST_END();
}

static void test_ssr_edge_fade_border(void) {
  TEST_BEGIN("ssr_edge_fade_border");
  /* At screen edge (0,0): fully faded */
  float f = screen_edge_fade(0.0f, 0.0f);
  TEST_ASSERT(fabsf(f) < 0.001f);
  /* Slightly inside edge: partially faded */
  float f2 = screen_edge_fade(0.05f, 0.5f);
  TEST_ASSERT(f2 > 0.0f && f2 < 1.0f);
  /* Near right edge */
  float f3 = screen_edge_fade(0.95f, 0.5f);
  TEST_ASSERT(f3 > 0.0f && f3 < 1.0f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Vulkan struct layout (conditional)
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"

static void test_ssr_device_fields(void) {
  TEST_BEGIN("ssr_device_fields");
  /* Verify SSR fields exist and have expected default values after zeroinit */
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  TEST_ASSERT(dev.ssr_enabled == false);
  TEST_ASSERT(dev.ssr_intensity == 0.0f);
  TEST_ASSERT(dev.ssr_pipeline == VK_NULL_HANDLE);
  TEST_ASSERT(dev.ssr_render_pass == VK_NULL_HANDLE);
  TEST_END();
}

static void test_ssr_framebuffer_fields(void) {
  TEST_BEGIN("ssr_framebuffer_fields");
  MopRhiFramebuffer fb;
  memset(&fb, 0, sizeof(fb));
  TEST_ASSERT(fb.ssr_image == VK_NULL_HANDLE);
  TEST_ASSERT(fb.ssr_view == VK_NULL_HANDLE);
  TEST_ASSERT(fb.ssr_framebuffer == VK_NULL_HANDLE);
  TEST_ASSERT(fb.ssr_memory == VK_NULL_HANDLE);
  TEST_END();
}
#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * Test runner
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("ssr");

  /* Flag tests */
  test_ssr_flag_value();
  test_ssr_flag_combinable();

  /* Public API tests */
  test_ssr_default_intensity();
  test_ssr_set_intensity();
  test_ssr_set_null_viewport();
  test_ssr_post_effect_propagation();

  /* Push constant layout tests */
  test_ssr_push_constant_size();
  test_ssr_push_constant_offsets();

  /* Shader math tests */
  test_ssr_depth_reconstruction_center();
  test_ssr_depth_reconstruction_reverse_z();
  test_ssr_edge_fade_center();
  test_ssr_edge_fade_border();

#if defined(MOP_HAS_VULKAN)
  /* Vulkan struct tests */
  test_ssr_device_fields();
  test_ssr_framebuffer_fields();
#endif

  TEST_REPORT();
  TEST_EXIT();
}
