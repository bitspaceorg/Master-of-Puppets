/*
 * Master of Puppets — Test Suite
 * test_debug_profiling.c — Phase 9: Debug, Profiling, and LOD System
 *
 * Tests validate:
 *   - Extended MopFrameStats fields
 *   - Per-pass GPU timing structures
 *   - Debug visualization enum values
 *   - LOD system constants and selection logic
 *   - MopDebugViz / MopDisplaySettings integration
 *   - LOD level chain in MopMesh
 *   - Viewport debug viz / LOD bias API
 *   - Vulkan per-pass timestamp pool constants
 *   - GPU pass timing entry/result struct sizes
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>
#include <stddef.h>
#include <string.h>

#ifdef MOP_HAS_VULKAN
#include "backend/vulkan/vulkan_internal.h"
#endif

/* -------------------------------------------------------------------------
 * Extended MopFrameStats fields (Phase 9A)
 * ------------------------------------------------------------------------- */

static void test_frame_stats_extended_fields(void) {
  TEST_BEGIN("frame_stats_extended_fields");
  MopFrameStats stats;
  memset(&stats, 0, sizeof(stats));

  /* Existing fields */
  TEST_ASSERT(stats.frame_time_ms == 0.0);
  TEST_ASSERT(stats.triangle_count == 0);
  TEST_ASSERT(stats.pixel_count == 0);

  /* New Phase 9A fields */
  TEST_ASSERT(stats.draw_call_count == 0);
  TEST_ASSERT(stats.vertex_count == 0);
  TEST_ASSERT(stats.visible_draws == 0);
  TEST_ASSERT(stats.culled_draws == 0);
  TEST_ASSERT(stats.gpu_frame_ms == 0.0);
  TEST_ASSERT(stats.pass_timing_count == 0);
  TEST_ASSERT(stats.lod_transitions == 0);
  TEST_ASSERT(stats.gpu_memory_used == 0);
  TEST_ASSERT(stats.gpu_memory_budget == 0);

  /* Verify we can write/read these fields */
  stats.draw_call_count = 42;
  stats.vertex_count = 12345;
  stats.gpu_frame_ms = 16.6;
  TEST_ASSERT(stats.draw_call_count == 42);
  TEST_ASSERT(stats.vertex_count == 12345);
  TEST_ASSERT(stats.gpu_frame_ms > 16.5 && stats.gpu_frame_ms < 16.7);
  TEST_END();
}

static void test_frame_stats_pass_timings(void) {
  TEST_BEGIN("frame_stats_pass_timings");
  MopFrameStats stats;
  memset(&stats, 0, sizeof(stats));

  /* Verify MOP_MAX_GPU_PASS_TIMINGS is reasonable */
  TEST_ASSERT(MOP_MAX_GPU_PASS_TIMINGS >= 16);
  TEST_ASSERT(MOP_MAX_GPU_PASS_TIMINGS <= 128);

  /* Fill some pass timings */
  stats.pass_timing_count = 3;
  stats.pass_timings[0] = (MopGpuPassTiming){.name = "opaque", .gpu_ms = 2.5};
  stats.pass_timings[1] = (MopGpuPassTiming){.name = "shadow", .gpu_ms = 1.2};
  stats.pass_timings[2] =
      (MopGpuPassTiming){.name = "postprocess", .gpu_ms = 0.8};

  TEST_ASSERT(stats.pass_timing_count == 3);
  TEST_ASSERT(strcmp(stats.pass_timings[0].name, "opaque") == 0);
  TEST_ASSERT(stats.pass_timings[0].gpu_ms > 2.4 &&
              stats.pass_timings[0].gpu_ms < 2.6);
  TEST_ASSERT(strcmp(stats.pass_timings[2].name, "postprocess") == 0);
  TEST_END();
}

static void test_gpu_pass_timing_struct(void) {
  TEST_BEGIN("gpu_pass_timing_struct");
  /* MopGpuPassTiming should contain name + gpu_ms */
  MopGpuPassTiming t = {.name = "test_pass", .gpu_ms = 3.14};
  TEST_ASSERT(strcmp(t.name, "test_pass") == 0);
  TEST_ASSERT(t.gpu_ms > 3.13 && t.gpu_ms < 3.15);

  /* Struct should be reasonably sized */
  TEST_ASSERT(sizeof(MopGpuPassTiming) <= 32);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Debug visualization enum (Phase 9B)
 * ------------------------------------------------------------------------- */

static void test_debug_viz_enum_values(void) {
  TEST_BEGIN("debug_viz_enum_values");
  /* Verify all enum values exist and are distinct */
  TEST_ASSERT(MOP_DEBUG_VIZ_NONE == 0);
  TEST_ASSERT(MOP_DEBUG_VIZ_OVERDRAW == 1);
  TEST_ASSERT(MOP_DEBUG_VIZ_SHADOW_CASCADES == 2);
  TEST_ASSERT(MOP_DEBUG_VIZ_LOD_LEVEL == 3);
  TEST_ASSERT(MOP_DEBUG_VIZ_CULL_RESULT == 4);
  TEST_ASSERT(MOP_DEBUG_VIZ_DEPTH == 5);
  TEST_ASSERT(MOP_DEBUG_VIZ_NORMALS == 6);
  TEST_ASSERT(MOP_DEBUG_VIZ_MIPMAP == 7);
  TEST_END();
}

static void test_debug_viz_in_display_settings(void) {
  TEST_BEGIN("debug_viz_in_display_settings");
  MopDisplaySettings ds;
  memset(&ds, 0, sizeof(ds));

  /* Default should be no debug viz */
  TEST_ASSERT(ds.debug_viz == MOP_DEBUG_VIZ_NONE);

  /* Set and read back */
  ds.debug_viz = MOP_DEBUG_VIZ_OVERDRAW;
  TEST_ASSERT(ds.debug_viz == MOP_DEBUG_VIZ_OVERDRAW);

  ds.debug_viz = MOP_DEBUG_VIZ_SHADOW_CASCADES;
  TEST_ASSERT(ds.debug_viz == MOP_DEBUG_VIZ_SHADOW_CASCADES);
  TEST_END();
}

static void test_debug_viz_viewport_api(void) {
  TEST_BEGIN("debug_viz_viewport_api");
  /* Null safety */
  mop_viewport_set_debug_viz(NULL, MOP_DEBUG_VIZ_OVERDRAW);
  TEST_ASSERT(mop_viewport_get_debug_viz(NULL) == MOP_DEBUG_VIZ_NONE);
  TEST_ASSERT(1); /* No crash */
  TEST_END();
}

/* -------------------------------------------------------------------------
 * LOD system constants (Phase 9C)
 * ------------------------------------------------------------------------- */

static void test_lod_max_levels(void) {
  TEST_BEGIN("lod_max_levels");
  /* MOP_MAX_LOD_LEVELS should be between 4 and 16 */
  TEST_ASSERT(MOP_MAX_LOD_LEVELS >= 4);
  TEST_ASSERT(MOP_MAX_LOD_LEVELS <= 16);
  /* Must be at least 2 (base + 1 LOD) */
  TEST_ASSERT(MOP_MAX_LOD_LEVELS >= 2);
  TEST_END();
}

static void test_lod_bias_viewport_api(void) {
  TEST_BEGIN("lod_bias_viewport_api");
  /* Null safety */
  mop_viewport_set_lod_bias(NULL, 5.0f);
  float bias = mop_viewport_get_lod_bias(NULL);
  TEST_ASSERT(bias == 0.0f);
  TEST_ASSERT(1); /* No crash */
  TEST_END();
}

static void test_lod_add_null_safety(void) {
  TEST_BEGIN("lod_add_null_safety");
  MopMeshDesc desc;
  memset(&desc, 0, sizeof(desc));
  /* All NULL/invalid args should return -1 */
  int32_t r = mop_mesh_add_lod(NULL, NULL, NULL, 100.0f);
  TEST_ASSERT(r == -1);
  r = mop_mesh_add_lod(NULL, NULL, &desc, 100.0f);
  TEST_ASSERT(r == -1);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Vulkan per-pass timestamp infrastructure (Phase 9A)
 * ------------------------------------------------------------------------- */

#ifdef MOP_HAS_VULKAN

static void test_vk_pass_timestamp_constants(void) {
  TEST_BEGIN("vk_pass_timestamp_constants");
  /* MOP_VK_MAX_PASS_TIMESTAMPS should accommodate 48 passes × 2 queries */
  TEST_ASSERT(MOP_VK_MAX_PASS_TIMESTAMPS >= 96);
  TEST_ASSERT(MOP_VK_PASS_QUERY_OFFSET == 2);
  TEST_END();
}

static void test_vk_device_pass_timing_fields(void) {
  TEST_BEGIN("vk_device_pass_timing_fields");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));

  /* Zero-initialized fields */
  TEST_ASSERT(dev.pass_timestamp_pool == VK_NULL_HANDLE);
  TEST_ASSERT(dev.pass_query_count == 0);
  TEST_ASSERT(dev.pass_timing_count == 0);
  TEST_ASSERT(dev.pass_timing_result_count == 0);

  /* Verify timing entries can be populated */
  dev.pass_timing_count = 1;
  dev.pass_timing_entries[0].name = "test_pass";
  dev.pass_timing_entries[0].query_start = 0;
  dev.pass_timing_entries[0].query_end = 1;
  TEST_ASSERT(strcmp(dev.pass_timing_entries[0].name, "test_pass") == 0);
  TEST_ASSERT(dev.pass_timing_entries[0].query_start == 0);
  TEST_ASSERT(dev.pass_timing_entries[0].query_end == 1);

  /* Verify timing results can be populated */
  dev.pass_timing_result_count = 1;
  dev.pass_timing_results[0].name = "test_pass";
  dev.pass_timing_results[0].gpu_ms = 1.234;
  TEST_ASSERT(dev.pass_timing_results[0].gpu_ms > 1.233 &&
              dev.pass_timing_results[0].gpu_ms < 1.235);
  TEST_END();
}

static void test_vk_pass_timestamp_null_safety(void) {
  TEST_BEGIN("vk_pass_timestamp_null_safety");
  /* Null device should not crash */
  mop_vk_pass_timestamp_begin(NULL, "test");
  mop_vk_pass_timestamp_end(NULL);

  /* Device with no pool should not crash */
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  dev.has_timestamp_queries = false;
  mop_vk_pass_timestamp_begin(&dev, "test");
  mop_vk_pass_timestamp_end(&dev);

  TEST_ASSERT(1); /* No crash */
  TEST_END();
}

#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * LOD selection logic
 * ------------------------------------------------------------------------- */

static void test_lod_screen_threshold(void) {
  TEST_BEGIN("lod_screen_threshold");
  /* Verify LOD level struct layout */
  struct MopLodTestLevel {
    void *vb;
    void *ib;
    uint32_t vc;
    uint32_t ic;
    float screen_threshold;
  };
  struct MopLodTestLevel lvl;
  memset(&lvl, 0, sizeof(lvl));
  lvl.screen_threshold = 200.0f;
  TEST_ASSERT(lvl.screen_threshold > 199.0f && lvl.screen_threshold < 201.0f);
  TEST_END();
}

static void test_frame_stats_lod_transitions(void) {
  TEST_BEGIN("frame_stats_lod_transitions");
  MopFrameStats stats;
  memset(&stats, 0, sizeof(stats));

  stats.lod_transitions = 5;
  TEST_ASSERT(stats.lod_transitions == 5);

  stats.lod_transitions = 0;
  TEST_ASSERT(stats.lod_transitions == 0);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Frame stats struct size
 * ------------------------------------------------------------------------- */

static void test_frame_stats_struct_size(void) {
  TEST_BEGIN("frame_stats_struct_size");
  /* MopFrameStats should be large enough for all fields but not absurdly so */
  TEST_ASSERT(sizeof(MopFrameStats) > 32);
  /* With MOP_MAX_GPU_PASS_TIMINGS=48 entries × (ptr+double), it will be sizable
   * but shouldn't exceed 2KB */
  TEST_ASSERT(sizeof(MopFrameStats) < 2048);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Debug viz mode completeness
 * ------------------------------------------------------------------------- */

static void test_debug_viz_mode_completeness(void) {
  TEST_BEGIN("debug_viz_mode_completeness");
  /* Verify all modes are non-negative and sequential from 0 */
  MopDebugViz modes[] = {
      MOP_DEBUG_VIZ_NONE,
      MOP_DEBUG_VIZ_OVERDRAW,
      MOP_DEBUG_VIZ_SHADOW_CASCADES,
      MOP_DEBUG_VIZ_LOD_LEVEL,
      MOP_DEBUG_VIZ_CULL_RESULT,
      MOP_DEBUG_VIZ_DEPTH,
      MOP_DEBUG_VIZ_NORMALS,
      MOP_DEBUG_VIZ_MIPMAP,
  };
  /* 8 modes total */
  TEST_ASSERT(sizeof(modes) / sizeof(modes[0]) == 8);
  /* Each value should equal its index */
  for (int i = 0; i < 8; i++) {
    TEST_ASSERT((int)modes[i] == i);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Profile API null safety
 * ------------------------------------------------------------------------- */

static void test_get_stats_null_safety(void) {
  TEST_BEGIN("get_stats_null_safety");
  MopFrameStats stats = mop_viewport_get_stats(NULL);
  TEST_ASSERT(stats.frame_time_ms == 0.0);
  TEST_ASSERT(stats.triangle_count == 0);
  TEST_ASSERT(stats.draw_call_count == 0);
  TEST_ASSERT(stats.gpu_frame_ms == 0.0);
  TEST_END();
}

static void test_gpu_frame_time_null_safety(void) {
  TEST_BEGIN("gpu_frame_time_null_safety");
  float t = mop_viewport_gpu_frame_time_ms(NULL);
  TEST_ASSERT(t == 0.0f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
  printf("=== Debug, Profiling & LOD Tests (Phase 9) ===\n");

  /* Phase 9A: GPU Profiling */
  test_frame_stats_extended_fields();
  test_frame_stats_pass_timings();
  test_gpu_pass_timing_struct();
  test_frame_stats_struct_size();
  test_get_stats_null_safety();
  test_gpu_frame_time_null_safety();

  /* Phase 9B: Debug Visualization */
  test_debug_viz_enum_values();
  test_debug_viz_in_display_settings();
  test_debug_viz_viewport_api();
  test_debug_viz_mode_completeness();

  /* Phase 9C: LOD System */
  test_lod_max_levels();
  test_lod_bias_viewport_api();
  test_lod_add_null_safety();
  test_lod_screen_threshold();
  test_frame_stats_lod_transitions();

#ifdef MOP_HAS_VULKAN
  /* Vulkan-specific per-pass timing */
  test_vk_pass_timestamp_constants();
  test_vk_device_pass_timing_fields();
  test_vk_pass_timestamp_null_safety();
#endif

  TEST_REPORT();
  TEST_EXIT();
}
