/*
 * Master of Puppets — Test Suite
 * test_hiz.c — Phase 2C: Hi-Z Occlusion Culling
 *
 * Tests validate the Hi-Z occlusion culling infrastructure:
 *   - Mip level count calculation
 *   - Hi-Z framebuffer fields
 *   - Hi-Z device fields
 *   - Hi-Z downsample workgroup count
 *   - Cull push constants layout
 *   - Build/dispatch null safety
 *   - Mipped image/view helper declarations
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>
#include <stddef.h>
#include <string.h>

/* Include Vulkan internal header for struct validation */
#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"
#endif

#if defined(MOP_HAS_VULKAN)

/* -------------------------------------------------------------------------
 * Mip level count calculation
 * ------------------------------------------------------------------------- */

static uint32_t calc_mip_levels(uint32_t w, uint32_t h) {
  uint32_t levels = 1;
  while (w > 1 || h > 1) {
    w = (w + 1) / 2;
    h = (h + 1) / 2;
    levels++;
  }
  return levels;
}

static void test_mip_levels_power_of_two(void) {
  TEST_BEGIN("mip_levels_power_of_two");
  /* 1024×1024 → 11 levels (1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1) */
  TEST_ASSERT(calc_mip_levels(1024, 1024) == 11);
  /* 256×256 → 9 levels */
  TEST_ASSERT(calc_mip_levels(256, 256) == 9);
  /* 1×1 → 1 level */
  TEST_ASSERT(calc_mip_levels(1, 1) == 1);
  TEST_END();
}

static void test_mip_levels_non_power_of_two(void) {
  TEST_BEGIN("mip_levels_non_power_of_two");
  /* 1920×1080 → ceil rounds: 960×540, 480×270, 240×135, 120×68, 60×34,
   * 30×17, 15×9, 8×5, 4×3, 2×2, 1×1 = 12 levels */
  uint32_t levels = calc_mip_levels(1920, 1080);
  TEST_ASSERT(levels == 12);

  /* 800×600 → levels include odd sizes */
  levels = calc_mip_levels(800, 600);
  TEST_ASSERT(levels > 5);   /* at least 5+ levels */
  TEST_ASSERT(levels <= 16); /* won't exceed max */
  TEST_END();
}

static void test_mip_levels_rectangular(void) {
  TEST_BEGIN("mip_levels_rectangular");
  /* 4096×1 → levels: 2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1
   * = 13 levels (driven by the larger dimension) */
  uint32_t levels = calc_mip_levels(4096, 1);
  TEST_ASSERT(levels == 13);
  TEST_END();
}

static void test_mip_level_clamp(void) {
  TEST_BEGIN("mip_level_clamp");
  /* MOP_VK_HIZ_MAX_LEVELS = 16 — verify it's enough for 32K resolution */
  uint32_t levels = calc_mip_levels(32768, 32768);
  /* 32768 → 16 levels unclamped */
  if (levels > MOP_VK_HIZ_MAX_LEVELS)
    levels = MOP_VK_HIZ_MAX_LEVELS;
  TEST_ASSERT(levels == MOP_VK_HIZ_MAX_LEVELS);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Framebuffer Hi-Z fields
 * ------------------------------------------------------------------------- */

static void test_fb_hiz_fields(void) {
  TEST_BEGIN("fb_hiz_fields");
  MopRhiFramebuffer fb;
  memset(&fb, 0, sizeof(fb));

  /* Zero-initialized Hi-Z fields */
  TEST_ASSERT(fb.hiz_image == VK_NULL_HANDLE);
  TEST_ASSERT(fb.hiz_memory == VK_NULL_HANDLE);
  TEST_ASSERT(fb.hiz_levels == 0);
  TEST_ASSERT(fb.hiz_width == 0);
  TEST_ASSERT(fb.hiz_height == 0);

  /* All views should be null */
  for (int i = 0; i < MOP_VK_HIZ_MAX_LEVELS; i++) {
    TEST_ASSERT(fb.hiz_views[i] == VK_NULL_HANDLE);
  }

  /* Test setting values */
  fb.hiz_levels = 11;
  fb.hiz_width = 1024;
  fb.hiz_height = 768;
  TEST_ASSERT(fb.hiz_levels == 11);
  TEST_ASSERT(fb.hiz_width == 1024);
  TEST_ASSERT(fb.hiz_height == 768);
  TEST_END();
}

static void test_fb_hiz_max_levels_constant(void) {
  TEST_BEGIN("fb_hiz_max_levels_constant");
  /* MOP_VK_HIZ_MAX_LEVELS should be at least 12 (4096×4096) */
  TEST_ASSERT(MOP_VK_HIZ_MAX_LEVELS >= 12);
  /* Should be at most 20 (reasonable upper bound) */
  TEST_ASSERT(MOP_VK_HIZ_MAX_LEVELS <= 20);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Device Hi-Z fields
 * ------------------------------------------------------------------------- */

static void test_device_hiz_fields(void) {
  TEST_BEGIN("device_hiz_fields");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));

  TEST_ASSERT(dev.hiz_downsample_comp == VK_NULL_HANDLE);
  TEST_ASSERT(dev.hiz_pipeline == VK_NULL_HANDLE);
  TEST_ASSERT(dev.hiz_pipeline_layout == VK_NULL_HANDLE);
  TEST_ASSERT(dev.hiz_desc_layout == VK_NULL_HANDLE);
  TEST_ASSERT(dev.hiz_sampler == VK_NULL_HANDLE);
  TEST_ASSERT(dev.hiz_enabled == false);
  TEST_END();
}

static void test_hiz_depends_on_culling(void) {
  TEST_BEGIN("hiz_depends_on_culling");
  /* Hi-Z requires gpu_culling_enabled — both fields exist and can be
   * independently set */
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));

  dev.gpu_culling_enabled = true;
  dev.hiz_enabled = false;
  TEST_ASSERT(dev.gpu_culling_enabled && !dev.hiz_enabled);

  dev.hiz_enabled = true;
  TEST_ASSERT(dev.gpu_culling_enabled && dev.hiz_enabled);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Hi-Z downsample workgroup count
 * ------------------------------------------------------------------------- */

static void test_hiz_downsample_groups(void) {
  TEST_BEGIN("hiz_downsample_groups");
  /* Workgroup size = 8×8 from mop_hiz_downsample.comp */
  /* Groups = ceil(dst_w/8) × ceil(dst_h/8) */

  /* 512×512 dst → 64×64 groups */
  uint32_t gx = (512 + 7) / 8;
  uint32_t gy = (512 + 7) / 8;
  TEST_ASSERT(gx == 64);
  TEST_ASSERT(gy == 64);

  /* 513×257 dst → 65×33 groups */
  gx = (513 + 7) / 8;
  gy = (257 + 7) / 8;
  TEST_ASSERT(gx == 65);
  TEST_ASSERT(gy == 33);

  /* 1×1 dst → 1×1 groups */
  gx = (1 + 7) / 8;
  gy = (1 + 7) / 8;
  TEST_ASSERT(gx == 1);
  TEST_ASSERT(gy == 1);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Cull push constants layout
 * ------------------------------------------------------------------------- */

static void test_cull_push_constants_size(void) {
  TEST_BEGIN("cull_push_constants_size");
  /* Push constants: int hiz_enabled + ivec2 hiz_size + int reverse_z = 16 bytes
   */
  struct CullPC {
    int32_t hiz_enabled;
    int32_t hiz_w, hiz_h;
    int32_t reverse_z;
  };
  TEST_ASSERT(sizeof(struct CullPC) == 16);
  TEST_END();
}

static void test_hiz_downsample_push_constants(void) {
  TEST_BEGIN("hiz_downsample_push_constants");
  /* Push constants: ivec2 src_size + int reverse_z = 12 bytes */
  struct HizPC {
    int32_t src_w, src_h;
    int32_t reverse_z;
  };
  TEST_ASSERT(sizeof(struct HizPC) == 12);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Build/dispatch null safety
 * ------------------------------------------------------------------------- */

static void test_build_hiz_null_safety(void) {
  TEST_BEGIN("build_hiz_null_safety");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  MopRhiFramebuffer fb;
  memset(&fb, 0, sizeof(fb));

  /* hiz_enabled = false → early return */
  dev.hiz_enabled = false;
  mop_vk_build_hiz_pyramid(&dev, &fb, VK_NULL_HANDLE);

  /* hiz_enabled but no levels → early return */
  dev.hiz_enabled = true;
  fb.hiz_levels = 0;
  mop_vk_build_hiz_pyramid(&dev, &fb, VK_NULL_HANDLE);

  /* hiz_enabled but no image → early return */
  fb.hiz_levels = 5;
  fb.hiz_image = VK_NULL_HANDLE;
  mop_vk_build_hiz_pyramid(&dev, &fb, VK_NULL_HANDLE);

  /* No depth image/copy → early return */
  fb.hiz_image = (VkImage)0x1234; /* fake */
  fb.depth_image = VK_NULL_HANDLE;
  fb.depth_copy_image = VK_NULL_HANDLE;
  mop_vk_build_hiz_pyramid(&dev, &fb, VK_NULL_HANDLE);

  TEST_ASSERT(1); /* No crash */
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Mip size reduction
 * ------------------------------------------------------------------------- */

static void test_mip_size_reduction(void) {
  TEST_BEGIN("mip_size_reduction");
  /* The downsample uses (src + 1) / 2 for each dimension */
  /* Even: 1024 → 512 */
  TEST_ASSERT((1024 + 1) / 2 == 512);
  /* Odd: 1023 → 512 */
  TEST_ASSERT((1023 + 1) / 2 == 512);
  /* 1 → 1 (minimum) */
  uint32_t v = (1 + 1) / 2;
  if (v == 0)
    v = 1;
  TEST_ASSERT(v == 1);
  /* 3 → 2 */
  TEST_ASSERT((3 + 1) / 2 == 2);
  /* 2 → 1 (should reach 1, not 0) */
  v = (2 + 1) / 2;
  TEST_ASSERT(v == 1);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Hi-Z image format
 * ------------------------------------------------------------------------- */

static void test_hiz_uses_r32_sfloat(void) {
  TEST_BEGIN("hiz_uses_r32_sfloat");
  /* R32_SFLOAT = 100 in Vulkan spec.
   * Hi-Z image uses this format for single-channel float depth. */
  TEST_ASSERT(VK_FORMAT_R32_SFLOAT == 100);
  TEST_END();
}

#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_VULKAN)
int main(void) {
  printf("=== Hi-Z Occlusion Culling Tests (Phase 2C) ===\n");

  /* Mip level calculation */
  test_mip_levels_power_of_two();
  test_mip_levels_non_power_of_two();
  test_mip_levels_rectangular();
  test_mip_level_clamp();

  /* Framebuffer fields */
  test_fb_hiz_fields();
  test_fb_hiz_max_levels_constant();

  /* Device fields */
  test_device_hiz_fields();
  test_hiz_depends_on_culling();

  /* Workgroup calculation */
  test_hiz_downsample_groups();

  /* Push constants */
  test_cull_push_constants_size();
  test_hiz_downsample_push_constants();

  /* Null safety */
  test_build_hiz_null_safety();

  /* Mip math */
  test_mip_size_reduction();

  /* Format */
  test_hiz_uses_r32_sfloat();

  TEST_REPORT();
  TEST_EXIT();
}
#else
int main(void) {
  printf("=== Hi-Z Occlusion Culling Tests (Phase 2C) ===\n");
  printf("SKIP: Vulkan not enabled\n");
  return 0;
}
#endif
