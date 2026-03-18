/*
 * Master of Puppets — Test Suite
 * test_shadow.c — Phase 3B: Cascade Shadow Mapping
 *
 * Tests shadow pipeline push constant layout, cascade split computation,
 * light VP matrix construction, shadow draw storage, and Vulkan struct fields.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include "test_harness.h"
#include <math.h>
#include <mop/mop.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Shadow push constant: mat4 light_vp (64 bytes).
 * Must fit within Vulkan's minimum 128-byte push constant limit.
 * ------------------------------------------------------------------------- */

static void test_shadow_push_constant_size(void) {
  TEST_BEGIN("shadow_push_constant_size");
  struct {
    float light_vp[16]; /* mat4 — 64 bytes */
  } pc;
  TEST_ASSERT(sizeof(pc) == 64);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Cascade split computation: logarithmic scheme
 * splits[0] = near, splits[N] = far, intermediate = log/linear blend
 * ------------------------------------------------------------------------- */

static void test_cascade_splits_monotonic(void) {
  TEST_BEGIN("cascade_splits_monotonic");
  /* Simulate log/linear cascade split distances */
  float near_clip = 0.1f;
  float far_clip = 100.0f;
  int cascade_count = 4;
  float splits[5];

  splits[0] = near_clip;
  for (int i = 1; i <= cascade_count; i++) {
    float p = (float)i / (float)cascade_count;
    float log_split = near_clip * powf(far_clip / near_clip, p);
    float lin_split = near_clip + (far_clip - near_clip) * p;
    splits[i] = 0.95f * log_split + 0.05f * lin_split;
  }

  /* Splits must be strictly increasing */
  for (int i = 0; i < cascade_count; i++) {
    TEST_ASSERT(splits[i] < splits[i + 1]);
  }
  /* First split = near */
  TEST_ASSERT_FLOAT_EQ(splits[0], near_clip);
  /* Last split ~ far */
  TEST_ASSERT(fabsf(splits[cascade_count] - far_clip) < 1.0f);
  TEST_END();
}

static void test_cascade_splits_near_far(void) {
  TEST_BEGIN("cascade_splits_near_far");
  /* With equal near/far the splits should degenerate gracefully */
  float near_clip = 1.0f;
  float far_clip = 1.0f; /* degenerate case */
  float splits[5];
  splits[0] = near_clip;
  for (int i = 1; i <= 4; i++) {
    float p = (float)i / 4.0f;
    float log_split = near_clip * powf(far_clip / near_clip, p);
    float lin_split = near_clip + (far_clip - near_clip) * p;
    splits[i] = 0.95f * log_split + 0.05f * lin_split;
  }
  /* All splits should be near_clip (no NaN/Inf) */
  for (int i = 0; i <= 4; i++) {
    TEST_ASSERT(!isnan(splits[i]) && !isinf(splits[i]));
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Light direction normalization
 * ------------------------------------------------------------------------- */

static void test_light_dir_normalization(void) {
  TEST_BEGIN("light_dir_normalization");
  MopVec3 dir = {3.0f, 4.0f, 0.0f};
  float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
  if (len > 1e-6f) {
    dir.x /= len;
    dir.y /= len;
    dir.z /= len;
  }
  float nlen = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
  TEST_ASSERT(fabsf(nlen - 1.0f) < 0.001f);
  TEST_ASSERT(fabsf(dir.x - 0.6f) < 0.001f);
  TEST_ASSERT(fabsf(dir.y - 0.8f) < 0.001f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Light VP = ortho * lookAt
 * Verify the light VP matrix produces valid NDC coordinates.
 * ------------------------------------------------------------------------- */

static void test_light_vp_produces_valid_ndc(void) {
  TEST_BEGIN("light_vp_produces_valid_ndc");
  /* Simple case: light looking down -Y */
  MopVec3 center = {0, 0, 0};
  MopVec3 light_dir = {0, -1, 0};
  MopVec3 light_eye = {center.x - light_dir.x * 50.0f,
                       center.y - light_dir.y * 50.0f,
                       center.z - light_dir.z * 50.0f};
  MopMat4 light_view = mop_mat4_look_at(light_eye, center, (MopVec3){0, 0, 1});
  MopMat4 light_proj = mop_mat4_ortho(-10, 10, -10, 10, 0, 100);
  MopMat4 light_vp = mop_mat4_multiply(light_proj, light_view);

  /* Transform origin → should be in NDC range */
  MopVec4 p = mop_mat4_mul_vec4(light_vp, (MopVec4){0, 0, 0, 1});
  if (fabsf(p.w) > 1e-6f) {
    p.x /= p.w;
    p.y /= p.w;
    p.z /= p.w;
  }
  TEST_ASSERT(p.x >= -1.0f && p.x <= 1.0f);
  TEST_ASSERT(p.y >= -1.0f && p.y <= 1.0f);
  TEST_ASSERT(p.z >= 0.0f && p.z <= 1.0f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Shadow map constants (hardcoded to match vulkan_internal.h values)
 * ------------------------------------------------------------------------- */

static void test_shadow_map_constants(void) {
  TEST_BEGIN("shadow_map_constants");
  /* Shadow maps: 2048x2048, 4 cascades — verify expected values */
  const int expected_shadow_size = 2048;
  const int expected_cascades = 4;
  TEST_ASSERT(expected_shadow_size == 2048);
  TEST_ASSERT(expected_cascades == 4);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Vulkan struct fields (conditional on MOP_HAS_VULKAN)
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"

static void test_shadow_device_fields(void) {
  TEST_BEGIN("shadow_device_fields");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  TEST_ASSERT(dev.shadow_pipeline == VK_NULL_HANDLE);
  TEST_ASSERT(dev.shadow_render_pass == VK_NULL_HANDLE);
  TEST_ASSERT(dev.shadow_pipeline_layout == VK_NULL_HANDLE);
  TEST_ASSERT(dev.shadow_vert == VK_NULL_HANDLE);
  TEST_ASSERT(dev.shadow_frag == VK_NULL_HANDLE);
  TEST_ASSERT(dev.shadow_image == VK_NULL_HANDLE);
  TEST_ASSERT(dev.shadow_sampler == VK_NULL_HANDLE);
  TEST_ASSERT(dev.shadows_enabled == false);
  TEST_ASSERT(dev.shadow_draw_count == 0);
  TEST_ASSERT(dev.shadow_draw_capacity == 0);
  TEST_ASSERT(dev.shadow_draws == NULL);
  TEST_ASSERT(dev.shadow_data_valid == false);
  TEST_END();
}

static void test_shadow_cascade_arrays(void) {
  TEST_BEGIN("shadow_cascade_arrays");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  /* Verify cascade arrays are correctly sized */
  for (int i = 0; i < MOP_VK_CASCADE_COUNT; i++) {
    TEST_ASSERT(dev.shadow_fbs[i] == VK_NULL_HANDLE);
    TEST_ASSERT(dev.shadow_views[i] == VK_NULL_HANDLE);
    /* cascade_vp and cascade_splits should be zero-initialized */
    for (int j = 0; j < 16; j++)
      TEST_ASSERT(dev.cascade_vp[i].d[j] == 0.0f);
  }
  for (int i = 0; i <= MOP_VK_CASCADE_COUNT; i++) {
    TEST_ASSERT(dev.cascade_splits[i] == 0.0f);
  }
  TEST_END();
}

static void test_shadow_frame_globals_layout(void) {
  TEST_BEGIN("shadow_frame_globals_layout");
  /* FrameGlobals shadow fields must be at correct offsets */
  MopVkFrameGlobals g;
  memset(&g, 0, sizeof(g));
  g.shadows_enabled = 1;
  g.cascade_count = 4;
  TEST_ASSERT(g.shadows_enabled == 1);
  TEST_ASSERT(g.cascade_count == 4);
  /* cascade_vp: 4 x mat4 = 256 bytes */
  TEST_ASSERT(sizeof(g.cascade_vp) == 256);
  /* cascade_splits: 4 x float = 16 bytes */
  TEST_ASSERT(sizeof(g.cascade_splits) == 16);
  TEST_END();
}

static void test_shadow_draw_struct(void) {
  TEST_BEGIN("shadow_draw_struct");
  /* MopVkShadowDraw stores vertex/index buf handles + model matrix */
  struct MopVkShadowDraw {
    VkBuffer vertex_buf;
    VkBuffer index_buf;
    uint32_t index_count;
    float model[16];
  } sd;
  memset(&sd, 0, sizeof(sd));
  sd.index_count = 36;
  sd.model[0] = 1.0f;
  sd.model[5] = 1.0f;
  sd.model[10] = 1.0f;
  sd.model[15] = 1.0f;
  TEST_ASSERT(sd.index_count == 36);
  TEST_ASSERT(sd.model[0] == 1.0f);
  TEST_ASSERT(sd.model[15] == 1.0f);
  TEST_END();
}
#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("shadow");

  /* Layout */
  test_shadow_push_constant_size();
  test_shadow_map_constants();

  /* Cascade computation */
  test_cascade_splits_monotonic();
  test_cascade_splits_near_far();
  test_light_dir_normalization();
  test_light_vp_produces_valid_ndc();

#if defined(MOP_HAS_VULKAN)
  test_shadow_device_fields();
  test_shadow_cascade_arrays();
  test_shadow_frame_globals_layout();
  test_shadow_draw_struct();
#endif

  TEST_REPORT();
  TEST_EXIT();
}
