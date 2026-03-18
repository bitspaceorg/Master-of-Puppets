/*
 * Master of Puppets — Test Suite
 * test_volumetric.c — Phase 3C: Volumetric Fog
 *
 * Tests Henyey-Greenstein phase function, Beer-Lambert transmittance,
 * UBO layout, public API, and Vulkan struct fields.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include "test_harness.h"
#include <math.h>
#include <mop/mop.h>
#include <mop/render/postprocess.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------
 * Henyey-Greenstein phase function
 * P(cos_theta, g) = (1-g^2) / (4*pi * (1+g^2-2g*cos_theta)^1.5)
 * g=0 → isotropic (1/4pi)
 * g>0 → forward scattering (bright towards light)
 * g<0 → back scattering
 * ------------------------------------------------------------------------- */

static float hg_phase(float cos_theta, float g) {
  float g2 = g * g;
  float denom = 1.0f + g2 - 2.0f * g * cos_theta;
  if (denom < 0.0001f)
    denom = 0.0001f;
  return (1.0f - g2) / (4.0f * (float)M_PI * powf(denom, 1.5f));
}

static void test_hg_isotropic(void) {
  TEST_BEGIN("hg_isotropic");
  /* g=0: phase function = 1/(4*pi) for any angle */
  float expected = 1.0f / (4.0f * (float)M_PI);
  float p0 = hg_phase(1.0f, 0.0f);  /* forward */
  float p1 = hg_phase(-1.0f, 0.0f); /* backward */
  float p2 = hg_phase(0.0f, 0.0f);  /* perpendicular */
  TEST_ASSERT(fabsf(p0 - expected) < 0.001f);
  TEST_ASSERT(fabsf(p1 - expected) < 0.001f);
  TEST_ASSERT(fabsf(p2 - expected) < 0.001f);
  TEST_END();
}

static void test_hg_forward_scatter(void) {
  TEST_BEGIN("hg_forward_scatter");
  /* g>0: forward direction (cos=1) is brighter than backward (cos=-1) */
  float g = 0.6f;
  float forward = hg_phase(1.0f, g);
  float backward = hg_phase(-1.0f, g);
  TEST_ASSERT(forward > backward);
  TEST_ASSERT(forward > 1.0f / (4.0f * (float)M_PI)); /* brighter than iso */
  TEST_END();
}

static void test_hg_back_scatter(void) {
  TEST_BEGIN("hg_back_scatter");
  /* g<0: backward direction is brighter than forward */
  float g = -0.5f;
  float forward = hg_phase(1.0f, g);
  float backward = hg_phase(-1.0f, g);
  TEST_ASSERT(backward > forward);
  TEST_END();
}

static void test_hg_normalization(void) {
  TEST_BEGIN("hg_normalization");
  /* Phase function integrates to 1 over the sphere.
   * Approximate with trapezoidal integration over cos_theta ∈ [-1,1]. */
  float g = 0.3f;
  int N = 1000;
  float integral = 0.0f;
  for (int i = 0; i < N; i++) {
    float cos_theta = -1.0f + 2.0f * ((float)i + 0.5f) / (float)N;
    integral += hg_phase(cos_theta, g) * 2.0f * (float)M_PI * (2.0f / (float)N);
  }
  TEST_ASSERT(fabsf(integral - 1.0f) < 0.05f); /* within 5% */
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Beer-Lambert transmittance: T = exp(-density * distance)
 * ------------------------------------------------------------------------- */

static void test_beer_lambert(void) {
  TEST_BEGIN("beer_lambert");
  float density = 0.05f;
  float dist = 10.0f;
  float T = expf(-density * dist);
  /* 0.05 * 10 = 0.5, exp(-0.5) ≈ 0.6065 */
  TEST_ASSERT(fabsf(T - 0.6065f) < 0.01f);
  /* Zero distance = full transmittance */
  TEST_ASSERT(expf(-density * 0.0f) == 1.0f);
  /* Very large distance = nearly opaque */
  TEST_ASSERT(expf(-density * 1000.0f) < 0.001f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Volumetric UBO layout
 * inv_vp(64) + cam_pos(16) + fog_params(16) +
 * anisotropy(4) + num_lights(4) + num_steps(4) + reverse_z(4) = 112
 * ------------------------------------------------------------------------- */

static void test_volumetric_ubo_layout(void) {
  TEST_BEGIN("volumetric_ubo_layout");
  struct {
    float inv_vp[16];    /* mat4 — 64 bytes */
    float cam_pos[4];    /* vec4 — 16 bytes */
    float fog_params[4]; /* vec4 — 16 bytes */
    float anisotropy;    /* float — 4 bytes */
    int32_t num_lights;  /* int — 4 bytes */
    int32_t num_steps;   /* int — 4 bytes */
    int32_t reverse_z;   /* int — 4 bytes */
  } ubo;
  TEST_ASSERT(sizeof(ubo) == 112);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Media compositing blend equation
 * out = in_scattered * 1 + scene * transmittance
 * Verify the math works correctly
 * ------------------------------------------------------------------------- */

static void test_media_compositing(void) {
  TEST_BEGIN("media_compositing");
  /* Scene color = (0.8, 0.2, 0.1), in_scattered = (0.1, 0.1, 0.15),
   * transmittance = 0.7 */
  float scene_r = 0.8f, scatter_r = 0.1f, T = 0.7f;
  float out_r = scatter_r * 1.0f + scene_r * T;
  /* Expected: 0.1 + 0.8*0.7 = 0.66 */
  TEST_ASSERT(fabsf(out_r - 0.66f) < 0.001f);

  /* Zero fog: scatter=0, T=1 → output = scene unchanged */
  TEST_ASSERT(fabsf(0.0f + scene_r * 1.0f - scene_r) < 0.001f);

  /* Full fog: T=0 → output = scatter only (scene fully occluded) */
  TEST_ASSERT(fabsf(scatter_r + scene_r * 0.0f - scatter_r) < 0.001f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Public API: MOP_POST_VOLUMETRIC flag
 * ------------------------------------------------------------------------- */

static void test_volumetric_flag(void) {
  TEST_BEGIN("volumetric_flag");
  TEST_ASSERT(MOP_POST_VOLUMETRIC == (1 << 10));
  /* Doesn't collide with other flags */
  TEST_ASSERT((MOP_POST_VOLUMETRIC & MOP_POST_OIT) == 0);
  TEST_ASSERT((MOP_POST_VOLUMETRIC & MOP_POST_SSR) == 0);
  TEST_ASSERT((MOP_POST_VOLUMETRIC & MOP_POST_FOG) == 0);
  TEST_END();
}

static void test_volumetric_params_defaults(void) {
  TEST_BEGIN("volumetric_params_defaults");
  MopViewportDesc desc = {
      .width = 16, .height = 16, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  /* Default params should be reasonable */
  TEST_ASSERT(vp->volumetric_params.density > 0.0f);
  TEST_ASSERT(vp->volumetric_params.steps > 0);
  TEST_ASSERT(vp->volumetric_params.anisotropy >= -1.0f &&
              vp->volumetric_params.anisotropy <= 1.0f);

  mop_viewport_destroy(vp);
  TEST_END();
}

static void test_volumetric_set_params(void) {
  TEST_BEGIN("volumetric_set_params");
  MopViewportDesc desc = {
      .width = 16, .height = 16, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  MopVolumetricParams params = {
      .density = 0.05f,
      .color = {0.8f, 0.9f, 1.0f, 1.0f},
      .anisotropy = 0.5f,
      .steps = 48,
  };
  mop_viewport_set_volumetric(vp, &params);

  TEST_ASSERT(fabsf(vp->volumetric_params.density - 0.05f) < 0.001f);
  TEST_ASSERT(fabsf(vp->volumetric_params.anisotropy - 0.5f) < 0.001f);
  TEST_ASSERT(vp->volumetric_params.steps == 48);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Vulkan struct fields
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"

static void test_volumetric_device_fields(void) {
  TEST_BEGIN("volumetric_device_fields");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  TEST_ASSERT(dev.volumetric_pipeline == VK_NULL_HANDLE);
  TEST_ASSERT(dev.volumetric_render_pass == VK_NULL_HANDLE);
  TEST_ASSERT(dev.volumetric_pipeline_layout == VK_NULL_HANDLE);
  TEST_ASSERT(dev.volumetric_desc_layout == VK_NULL_HANDLE);
  TEST_ASSERT(dev.volumetric_frag == VK_NULL_HANDLE);
  TEST_ASSERT(dev.volumetric_enabled == false);
  TEST_END();
}

static void test_volumetric_framebuffer_fields(void) {
  TEST_BEGIN("volumetric_framebuffer_fields");
  MopRhiFramebuffer fb;
  memset(&fb, 0, sizeof(fb));
  TEST_ASSERT(fb.volumetric_framebuffer == VK_NULL_HANDLE);
  TEST_ASSERT(fb.volumetric_ubo == VK_NULL_HANDLE);
  TEST_ASSERT(fb.volumetric_ubo_mapped == NULL);
  TEST_END();
}
#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("volumetric");

  /* Phase function */
  test_hg_isotropic();
  test_hg_forward_scatter();
  test_hg_back_scatter();
  test_hg_normalization();

  /* Transmittance */
  test_beer_lambert();

  /* Layout */
  test_volumetric_ubo_layout();
  test_media_compositing();

  /* API */
  test_volumetric_flag();
  test_volumetric_params_defaults();
  test_volumetric_set_params();

#if defined(MOP_HAS_VULKAN)
  test_volumetric_device_fields();
  test_volumetric_framebuffer_fields();
#endif

  TEST_REPORT();
  TEST_EXIT();
}
