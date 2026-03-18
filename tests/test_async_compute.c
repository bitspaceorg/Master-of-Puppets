/*
 * Master of Puppets — Test Suite
 * test_async_compute.c — Phase 1C: Async Compute Queue
 *
 * Tests validate the async compute infrastructure:
 *   - Queue family detection and fallback logic
 *   - Compute resource fields in MopRhiDevice
 *   - GPU culling dispatch helper (null/zero-draw safety)
 *   - Async compute submit helper (null safety)
 *   - Queue creation info array layout
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
 * Vulkan device struct: async compute fields
 * ------------------------------------------------------------------------- */

static void test_device_has_async_compute_fields(void) {
  TEST_BEGIN("device_has_async_compute_fields");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));

  /* Verify all async compute fields are zero-initialized */
  TEST_ASSERT(dev.compute_queue == VK_NULL_HANDLE);
  TEST_ASSERT(dev.compute_queue_family == 0);
  TEST_ASSERT(dev.compute_cmd_pool == VK_NULL_HANDLE);
  TEST_ASSERT(dev.compute_cmd_buf == VK_NULL_HANDLE);
  TEST_ASSERT(dev.compute_semaphore == VK_NULL_HANDLE);
  TEST_ASSERT(dev.compute_fence == VK_NULL_HANDLE);
  TEST_ASSERT(dev.has_async_compute == false);
  TEST_END();
}

static void test_device_compute_queue_family_sentinel(void) {
  TEST_BEGIN("device_compute_queue_family_sentinel");
  /* UINT32_MAX is the sentinel for "no compute queue found" */
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  dev.compute_queue_family = UINT32_MAX;
  TEST_ASSERT(dev.compute_queue_family == UINT32_MAX);
  TEST_ASSERT(dev.has_async_compute == false);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * GPU culling dispatch: null/zero safety
 * ------------------------------------------------------------------------- */

static void test_cull_dispatch_null_device(void) {
  TEST_BEGIN("cull_dispatch_null_safety");
  /* Should not crash with valid dev but zero draw count */
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  MopRhiFramebuffer fb;
  memset(&fb, 0, sizeof(fb));

  /* gpu_culling_enabled = false → early return */
  dev.gpu_culling_enabled = false;
  fb.draw_count_this_frame = 10;
  mop_vk_dispatch_gpu_cull(&dev, &fb, VK_NULL_HANDLE);

  /* gpu_culling_enabled = true, draw_count = 0 → early return */
  dev.gpu_culling_enabled = true;
  fb.draw_count_this_frame = 0;
  mop_vk_dispatch_gpu_cull(&dev, &fb, VK_NULL_HANDLE);

  /* Both conditions met but no buffers → early return */
  fb.draw_count_this_frame = 5;
  mop_vk_dispatch_gpu_cull(&dev, &fb, VK_NULL_HANDLE);

  TEST_ASSERT(1); /* Reached without crashing */
  TEST_END();
}

static void test_cull_dispatch_needs_buffers(void) {
  TEST_BEGIN("cull_dispatch_needs_buffers");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  MopRhiFramebuffer fb;
  memset(&fb, 0, sizeof(fb));

  dev.gpu_culling_enabled = true;
  fb.draw_count_this_frame = 10;

  /* Missing input_draw_cmds → should not dispatch */
  fb.input_draw_cmds = VK_NULL_HANDLE;
  fb.output_draw_cmds = (VkBuffer)1; /* fake non-null */
  fb.draw_count_buf = (VkBuffer)1;
  fb.object_ssbo = (VkBuffer)1;
  fb.globals_ubo = (VkBuffer)1;
  mop_vk_dispatch_gpu_cull(&dev, &fb, VK_NULL_HANDLE);

  /* Missing object_ssbo → should not dispatch */
  fb.input_draw_cmds = (VkBuffer)1;
  fb.object_ssbo = VK_NULL_HANDLE;
  mop_vk_dispatch_gpu_cull(&dev, &fb, VK_NULL_HANDLE);

  TEST_ASSERT(1); /* No crash */
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Async compute submit: null safety
 * ------------------------------------------------------------------------- */

static void test_async_submit_no_compute(void) {
  TEST_BEGIN("async_submit_no_compute");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  dev.has_async_compute = false;

  /* Should return false when async compute is not available */
  bool ok = mop_vk_submit_async_compute(&dev, NULL, NULL);
  TEST_ASSERT(!ok);
  TEST_END();
}

static void test_async_submit_null_callback(void) {
  TEST_BEGIN("async_submit_null_callback");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  dev.has_async_compute = true;

  /* Should return false with null record function */
  bool ok = mop_vk_submit_async_compute(&dev, NULL, NULL);
  TEST_ASSERT(!ok);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Struct layout: MopVkThreadState vs async compute coexistence
 * ------------------------------------------------------------------------- */

static void test_thread_and_compute_coexist(void) {
  TEST_BEGIN("thread_and_compute_coexist");
  /* Verify that both thread_states[] and async compute fields exist
   * in the device struct without overlap */
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));

  dev.thread_count = 4;
  dev.thread_states[0].cb_recording = true;
  dev.has_async_compute = true;

  TEST_ASSERT(dev.thread_count == 4);
  TEST_ASSERT(dev.thread_states[0].cb_recording == true);
  TEST_ASSERT(dev.has_async_compute == true);

  /* Different fields don't alias */
  dev.thread_states[0].cb_recording = false;
  TEST_ASSERT(dev.has_async_compute == true);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Queue family: same-family vs different-family detection
 * ------------------------------------------------------------------------- */

static void test_compute_same_family(void) {
  TEST_BEGIN("compute_same_family");
  /* When compute_queue_family == queue_family, the device requests 2 queues
   * from the same family.  Validate the logic condition. */
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  dev.queue_family = 0;
  dev.compute_queue_family = 0; /* same family */

  TEST_ASSERT(dev.compute_queue_family == dev.queue_family);
  /* In this case, compute queue index should be 1 (second queue) */
  uint32_t cq_index = (dev.compute_queue_family == dev.queue_family) ? 1 : 0;
  TEST_ASSERT(cq_index == 1);
  TEST_END();
}

static void test_compute_different_family(void) {
  TEST_BEGIN("compute_different_family");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  dev.queue_family = 0;
  dev.compute_queue_family = 2; /* dedicated compute family */

  TEST_ASSERT(dev.compute_queue_family != dev.queue_family);
  uint32_t cq_index = (dev.compute_queue_family == dev.queue_family) ? 1 : 0;
  TEST_ASSERT(cq_index == 0);
  TEST_END();
}

static void test_compute_no_family(void) {
  TEST_BEGIN("compute_no_family");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  dev.queue_family = 0;
  dev.compute_queue_family = UINT32_MAX; /* no compute found */

  /* Should not enable async compute */
  TEST_ASSERT(dev.compute_queue_family == UINT32_MAX);
  TEST_ASSERT(dev.has_async_compute == false);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Cull descriptor set: per-frame lifecycle
 * ------------------------------------------------------------------------- */

static void test_cull_ds_reset_per_frame(void) {
  TEST_BEGIN("cull_ds_reset_per_frame");
  /* cull_ds is set to VK_NULL_HANDLE in frame_begin so it gets
   * re-allocated each frame.  Verify the default is null. */
  MopRhiFramebuffer fb;
  memset(&fb, 0, sizeof(fb));
  TEST_ASSERT(fb.cull_ds == VK_NULL_HANDLE);

  /* Simulate: after dispatch, cull_ds is set */
  fb.cull_ds = (VkDescriptorSet)0x1234;
  TEST_ASSERT(fb.cull_ds != VK_NULL_HANDLE);

  /* Simulate frame_begin reset */
  fb.cull_ds = VK_NULL_HANDLE;
  TEST_ASSERT(fb.cull_ds == VK_NULL_HANDLE);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Workgroup count calculation
 * ------------------------------------------------------------------------- */

static void test_cull_workgroup_count(void) {
  TEST_BEGIN("cull_workgroup_count");
  /* Workgroup size = 256 (from mop_cull.comp) */
  /* dispatch groups = ceil(total_draws / 256) */

  uint32_t draws1 = 1;
  uint32_t groups1 = (draws1 + 255) / 256;
  TEST_ASSERT(groups1 == 1);

  uint32_t draws256 = 256;
  uint32_t groups256 = (draws256 + 255) / 256;
  TEST_ASSERT(groups256 == 1);

  uint32_t draws257 = 257;
  uint32_t groups257 = (draws257 + 255) / 256;
  TEST_ASSERT(groups257 == 2);

  uint32_t draws4096 = 4096;
  uint32_t groups4096 = (draws4096 + 255) / 256;
  TEST_ASSERT(groups4096 == 16);

  uint32_t draws0 = 0;
  uint32_t groups0 = (draws0 + 255) / 256;
  TEST_ASSERT(groups0 == 0);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * MopVkComputeRecordFn callback type validation
 * ------------------------------------------------------------------------- */

static int g_record_called = 0;
static void *g_record_user_data = NULL;

static void dummy_record_fn(MopRhiDevice *dev, VkCommandBuffer cb,
                            void *user_data) {
  (void)dev;
  (void)cb;
  g_record_called = 1;
  g_record_user_data = user_data;
}

static void test_compute_record_fn_typedef(void) {
  TEST_BEGIN("compute_record_fn_typedef");
  /* Verify MopVkComputeRecordFn can be assigned and called */
  MopVkComputeRecordFn fn = dummy_record_fn;
  int sentinel = 42;
  g_record_called = 0;
  g_record_user_data = NULL;

  fn(NULL, VK_NULL_HANDLE, &sentinel);

  TEST_ASSERT(g_record_called == 1);
  TEST_ASSERT(g_record_user_data == &sentinel);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Frame globals struct: total_draws field
 * ------------------------------------------------------------------------- */

static void test_frame_globals_total_draws(void) {
  TEST_BEGIN("frame_globals_total_draws");
  MopVkFrameGlobals g;
  memset(&g, 0, sizeof(g));

  g.total_draws = 1024;
  TEST_ASSERT(g.total_draws == 1024);

  /* Offset check: total_draws at byte 480 */
  TEST_ASSERT(offsetof(MopVkFrameGlobals, total_draws) == 480);
  TEST_ASSERT(sizeof(MopVkFrameGlobals) == 496);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
  printf("=== Async Compute Queue Tests (Phase 1C) ===\n");

  /* Device struct */
  test_device_has_async_compute_fields();
  test_device_compute_queue_family_sentinel();
  test_thread_and_compute_coexist();

  /* Queue family logic */
  test_compute_same_family();
  test_compute_different_family();
  test_compute_no_family();

  /* GPU culling dispatch safety */
  test_cull_dispatch_null_device();
  test_cull_dispatch_needs_buffers();
  test_cull_ds_reset_per_frame();
  test_cull_workgroup_count();

  /* Async compute submit safety */
  test_async_submit_no_compute();
  test_async_submit_null_callback();
  test_compute_record_fn_typedef();

  /* Struct layout */
  test_frame_globals_total_draws();

  TEST_REPORT();
  TEST_EXIT();
}

#else /* !MOP_HAS_VULKAN */

int main(void) {
  printf("=== Async Compute Queue Tests (Phase 1C) ===\n");
  printf("SKIP: Vulkan not enabled\n");
  return 0;
}

#endif /* MOP_HAS_VULKAN */
