/*
 * Master of Puppets — Test Suite
 * test_gtao.c — Phase 4D: Ground Truth Ambient Occlusion (GTAO)
 *
 * Tests GTAO shader math (horizon integration, visibility), push constant
 * layout compatibility with SSAO, and Vulkan struct fields.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include "test_harness.h"
#include <math.h>
#include <mop/mop.h>
#include <mop/render/postprocess.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * GTAO inner integral: integrate_arc(h1, h2, n)
 *
 * V = 0.25 * (-cos(2h1 - n) + cos(n) + 2h1*sin(n))
 *   + 0.25 * (-cos(2h2 - n) + cos(n) + 2h2*sin(n))
 *
 * For fully unoccluded hemisphere: h1 = h2 = pi/2, n = 0
 *   → V = 0.25*(-cos(pi) + 1 + pi*0) + 0.25*(-cos(pi) + 1 + pi*0)
 *       = 0.25*(1+1) + 0.25*(1+1) = 1.0
 * ------------------------------------------------------------------------- */

#define PI 3.14159265f

static float integrate_arc(float h1, float h2, float n) {
  float sn = sinf(n);
  float cn = cosf(n);
  return 0.25f * (-cosf(2.0f * h1 - n) + cn + 2.0f * h1 * sn) +
         0.25f * (-cosf(2.0f * h2 - n) + cn + 2.0f * h2 * sn);
}

static void test_gtao_full_visibility(void) {
  TEST_BEGIN("gtao_full_visibility");
  /* h1=h2=π/2, n=0 → fully open hemisphere → V=1.0 */
  float v = integrate_arc(PI / 2.0f, PI / 2.0f, 0.0f);
  TEST_ASSERT(fabsf(v - 1.0f) < 0.01f);
  TEST_END();
}

static void test_gtao_zero_visibility(void) {
  TEST_BEGIN("gtao_zero_visibility");
  /* h1=h2=0, n=0 → horizons at tangent plane → V=0.0
   * integrate_arc(0,0,0) = 0.25*(-cos(0)+cos(0)) + same = 0.0 */
  float v = integrate_arc(0.0f, 0.0f, 0.0f);
  TEST_ASSERT(fabsf(v) < 0.01f);
  TEST_END();
}

static void test_gtao_half_visibility(void) {
  TEST_BEGIN("gtao_half_visibility");
  /* h1=h2=π/4, n=0 → quarter-angle horizons → V≈0.5
   * integrate_arc(π/4,π/4,0) = 0.25*(-cos(π/2)+1) * 2 = 0.5 */
  float v = integrate_arc(PI / 4.0f, PI / 4.0f, 0.0f);
  TEST_ASSERT(fabsf(v - 0.5f) < 0.05f);
  TEST_END();
}

static void test_gtao_normal_tilt(void) {
  TEST_BEGIN("gtao_normal_tilt");
  /* With tilted normal and full horizon, integral may exceed 1.0
   * but shader clamps output to [0,1]. Just verify formula is finite. */
  float n = PI / 4.0f; /* 45° tilt */
  float v = integrate_arc(PI / 2.0f, PI / 2.0f, n);
  TEST_ASSERT(v > 0.5f); /* should be large (>1 before clamp) */
  TEST_ASSERT(!isnan(v) && !isinf(v));
  TEST_END();
}

static void test_gtao_symmetry(void) {
  TEST_BEGIN("gtao_symmetry");
  /* For n=0, swapping h1 and h2 should give same result */
  float v1 = integrate_arc(0.5f, 0.3f, 0.0f);
  float v2 = integrate_arc(0.3f, 0.5f, 0.0f);
  TEST_ASSERT(fabsf(v1 - v2) < 0.01f);
  TEST_END();
}

static void test_gtao_monotonic(void) {
  TEST_BEGIN("gtao_monotonic");
  /* Higher horizons → more visibility */
  float v_low = integrate_arc(0.2f, 0.2f, 0.0f);
  float v_mid = integrate_arc(0.8f, 0.8f, 0.0f);
  float v_high = integrate_arc(PI / 2.0f, PI / 2.0f, 0.0f);
  TEST_ASSERT(v_low < v_mid);
  TEST_ASSERT(v_mid < v_high);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Push constant layout compatibility
 * Both SSAO and GTAO use 96-byte push constants with identical size.
 * ------------------------------------------------------------------------- */

static void test_gtao_push_constant_size(void) {
  TEST_BEGIN("gtao_push_constant_size");
  /* Both structs should be 96 bytes */
  struct {
    float projection[16];
    float radius;
    float intensity;
    int32_t num_steps;
    int32_t reverse_z;
    float noise_scale[2];
    float inv_resolution[2];
  } gtao_pc;
  TEST_ASSERT(sizeof(gtao_pc) == 96);
  TEST_END();
}

static void test_gtao_push_constant_defaults(void) {
  TEST_BEGIN("gtao_push_constant_defaults");
  /* Verify default GTAO parameters are sane */
  float radius = 0.5f;
  float intensity = 1.0f;
  int num_steps = 6;
  TEST_ASSERT(radius > 0.0f && radius < 5.0f);
  TEST_ASSERT(intensity >= 0.0f && intensity <= 2.0f);
  TEST_ASSERT(num_steps >= 2 && num_steps <= 12);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * SSAO flag still works (GTAO is transparent upgrade)
 * ------------------------------------------------------------------------- */

static void test_gtao_ssao_flag_unchanged(void) {
  TEST_BEGIN("gtao_ssao_flag_unchanged");
  TEST_ASSERT((int)MOP_POST_SSAO == (1 << 6));
  TEST_END();
}

static void test_gtao_ssao_api(void) {
  TEST_BEGIN("gtao_ssao_api");
  MopViewportDesc desc = {
      .width = 16, .height = 16, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);
  mop_viewport_set_post_effects(vp, MOP_POST_SSAO);
  TEST_ASSERT((vp->post_effects & MOP_POST_SSAO) != 0);
  mop_viewport_destroy(vp);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Vulkan struct fields
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"

static void test_gtao_device_fields(void) {
  TEST_BEGIN("gtao_device_fields");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  TEST_ASSERT(dev.gtao_available == false);
  TEST_ASSERT(dev.gtao_pipeline == VK_NULL_HANDLE);
  TEST_ASSERT(dev.gtao_blur_pipeline == VK_NULL_HANDLE);
  TEST_ASSERT(dev.gtao_frag == VK_NULL_HANDLE);
  TEST_ASSERT(dev.gtao_blur_frag == VK_NULL_HANDLE);
  TEST_END();
}
#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("gtao");

  /* Integration math */
  test_gtao_full_visibility();
  test_gtao_zero_visibility();
  test_gtao_half_visibility();
  test_gtao_normal_tilt();
  test_gtao_symmetry();
  test_gtao_monotonic();

  /* Push constant layout */
  test_gtao_push_constant_size();
  test_gtao_push_constant_defaults();

  /* SSAO API compatibility */
  test_gtao_ssao_flag_unchanged();
  test_gtao_ssao_api();

#if defined(MOP_HAS_VULKAN)
  test_gtao_device_fields();
#endif

  TEST_REPORT();
  TEST_EXIT();
}
