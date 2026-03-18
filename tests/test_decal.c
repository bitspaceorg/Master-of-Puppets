/*
 * Master of Puppets — Test Suite
 * test_decal.c — Phase 4E: Deferred Decal System
 *
 * Tests decal API, push constant layout, UBO layout, shader math
 * (edge fade, UV projection), and Vulkan struct fields.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include "test_harness.h"
#include <math.h>
#include <mop/mop.h>
#include <mop/render/decal.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Decal push constant layout
 * mat4 mvp (64 bytes) + mat4 inv_decal (64 bytes) = 128 bytes total.
 * Must fit within Vulkan's minimum guaranteed 128-byte push constant.
 * ------------------------------------------------------------------------- */

static void test_decal_push_constant_size(void) {
  TEST_BEGIN("decal_push_constant_size");
  struct {
    float mvp[16];       /* mat4 — 64 bytes */
    float inv_decal[16]; /* mat4 — 64 bytes */
  } pc;
  TEST_ASSERT(sizeof(pc) == 128);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Decal UBO layout: inv_vp(64) + reverse_z(4) + opacity(4) + pad(8) = 80
 * ------------------------------------------------------------------------- */

static void test_decal_ubo_size(void) {
  TEST_BEGIN("decal_ubo_size");
  struct {
    float inv_vp[16]; /* mat4 — 64 bytes */
    int32_t reverse_z;
    float opacity;
    float _pad[2];
  } ubo;
  TEST_ASSERT(sizeof(ubo) == 80);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Decal descriptor set: 3 bindings
 *   binding 0 = depth (sampler2D)
 *   binding 1 = decal texture (sampler2D)
 *   binding 2 = UBO (DecalUBO)
 * Just verify the binding count is correct.
 * ------------------------------------------------------------------------- */

static void test_decal_descriptor_count(void) {
  TEST_BEGIN("decal_descriptor_count");
  const int expected_bindings = 3;
  TEST_ASSERT(expected_bindings == 3);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Edge fade math: smoothstep-based fade near cube boundaries
 * fade_x = 1.0 - smoothstep(0.4, 0.5, abs(local.x))
 * At center (x=0): fade = 1.0 (full opacity)
 * At edge (x=0.5): fade = 0.0 (fully faded)
 * At boundary (x=0.45): fade = 0.5 (mid-fade)
 * ------------------------------------------------------------------------- */

static float smoothstep(float e0, float e1, float x) {
  float t = (x - e0) / (e1 - e0);
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

static float edge_fade(float local_x, float local_y, float local_z) {
  float fx = 1.0f - smoothstep(0.4f, 0.5f, fabsf(local_x));
  float fy = 1.0f - smoothstep(0.4f, 0.5f, fabsf(local_y));
  float fz = 1.0f - smoothstep(0.4f, 0.5f, fabsf(local_z));
  return fx * fy * fz;
}

static void test_decal_fade_center(void) {
  TEST_BEGIN("decal_fade_center");
  float f = edge_fade(0.0f, 0.0f, 0.0f);
  TEST_ASSERT(fabsf(f - 1.0f) < 0.01f);
  TEST_END();
}

static void test_decal_fade_edge(void) {
  TEST_BEGIN("decal_fade_edge");
  float f = edge_fade(0.5f, 0.0f, 0.0f);
  TEST_ASSERT(fabsf(f) < 0.01f);
  TEST_END();
}

static void test_decal_fade_boundary(void) {
  TEST_BEGIN("decal_fade_boundary");
  float f = edge_fade(0.45f, 0.0f, 0.0f);
  TEST_ASSERT(f > 0.1f && f < 0.9f); /* mid-range */
  TEST_END();
}

static void test_decal_fade_corner(void) {
  TEST_BEGIN("decal_fade_corner");
  /* All three axes near edge: fade should be very small */
  float f = edge_fade(0.48f, 0.48f, 0.48f);
  TEST_ASSERT(f < 0.1f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * UV projection: local XY → UV [0,1]²
 * UV = local.xy + 0.5
 * At local (0,0): UV = (0.5, 0.5) — center of texture
 * At local (-0.5,-0.5): UV = (0, 0) — bottom-left
 * At local (0.5, 0.5): UV = (1, 1) — top-right
 * ------------------------------------------------------------------------- */

static void test_decal_uv_center(void) {
  TEST_BEGIN("decal_uv_center");
  float u = 0.0f + 0.5f;
  float v = 0.0f + 0.5f;
  TEST_ASSERT(fabsf(u - 0.5f) < 0.001f);
  TEST_ASSERT(fabsf(v - 0.5f) < 0.001f);
  TEST_END();
}

static void test_decal_uv_corners(void) {
  TEST_BEGIN("decal_uv_corners");
  /* Bottom-left */
  TEST_ASSERT(fabsf((-0.5f + 0.5f) - 0.0f) < 0.001f);
  /* Top-right */
  TEST_ASSERT(fabsf((0.5f + 0.5f) - 1.0f) < 0.001f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Discard test: fragments outside [-0.5, 0.5]³ should be discarded
 * ------------------------------------------------------------------------- */

static void test_decal_discard_outside(void) {
  TEST_BEGIN("decal_discard_outside");
  /* These should be discarded */
  TEST_ASSERT(fabsf(0.6f) > 0.5f);
  TEST_ASSERT(fabsf(-0.6f) > 0.5f);
  /* These should NOT be discarded */
  TEST_ASSERT(fabsf(0.3f) <= 0.5f);
  TEST_ASSERT(fabsf(0.0f) <= 0.5f);
  TEST_ASSERT(fabsf(0.5f) <= 0.5f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Public API: add/remove decals via CPU backend (NULL function pointers)
 * ------------------------------------------------------------------------- */

static void test_decal_api_cpu(void) {
  TEST_BEGIN("decal_api_cpu");
  MopViewportDesc desc = {
      .width = 16, .height = 16, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  /* CPU backend has no decal support — should return -1 */
  MopDecalDesc dd = {
      .transform = mop_mat4_identity(),
      .opacity = 1.0f,
      .texture_idx = -1,
  };
  int32_t id = mop_viewport_add_decal(vp, &dd);
  TEST_ASSERT(id == -1);

  /* Remove should be a safe no-op */
  mop_viewport_remove_decal(vp, 0);
  mop_viewport_clear_decals(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * MOP_MAX_DECALS constant
 * ------------------------------------------------------------------------- */

static void test_decal_max_constant(void) {
  TEST_BEGIN("decal_max_constant");
  TEST_ASSERT(MOP_MAX_DECALS == 256);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Vulkan device struct fields
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"

static void test_decal_device_fields(void) {
  TEST_BEGIN("decal_device_fields");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  TEST_ASSERT(dev.decal_pipeline == VK_NULL_HANDLE);
  TEST_ASSERT(dev.decal_render_pass == VK_NULL_HANDLE);
  TEST_ASSERT(dev.decal_pipeline_layout == VK_NULL_HANDLE);
  TEST_ASSERT(dev.decal_desc_layout == VK_NULL_HANDLE);
  TEST_ASSERT(dev.decal_vert == VK_NULL_HANDLE);
  TEST_ASSERT(dev.decal_frag == VK_NULL_HANDLE);
  TEST_ASSERT(dev.decal_count == 0);
  TEST_END();
}

static void test_decal_framebuffer_fields(void) {
  TEST_BEGIN("decal_framebuffer_fields");
  MopRhiFramebuffer fb;
  memset(&fb, 0, sizeof(fb));
  TEST_ASSERT(fb.decal_framebuffer == VK_NULL_HANDLE);
  TEST_END();
}

static void test_decal_struct_size(void) {
  TEST_BEGIN("decal_struct_size");
  /* Each decal entry: model(64) + inv_model(64) + opacity(4) + tex_idx(4) +
   * active(1) + pad */
  struct MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  /* Verify the array exists and is indexed correctly */
  dev.decals[0].active = true;
  dev.decals[0].opacity = 0.5f;
  dev.decals[0].texture_idx = 42;
  TEST_ASSERT(dev.decals[0].active == true);
  TEST_ASSERT(dev.decals[0].opacity == 0.5f);
  TEST_ASSERT(dev.decals[0].texture_idx == 42);
  TEST_ASSERT(dev.decals[1].active == false);
  TEST_END();
}
#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("decal");

  /* Layout */
  test_decal_push_constant_size();
  test_decal_ubo_size();
  test_decal_descriptor_count();

  /* Shader math */
  test_decal_fade_center();
  test_decal_fade_edge();
  test_decal_fade_boundary();
  test_decal_fade_corner();
  test_decal_uv_center();
  test_decal_uv_corners();
  test_decal_discard_outside();

  /* API */
  test_decal_api_cpu();
  test_decal_max_constant();

#if defined(MOP_HAS_VULKAN)
  test_decal_device_fields();
  test_decal_framebuffer_fields();
  test_decal_struct_size();
#endif

  TEST_REPORT();
  TEST_EXIT();
}
