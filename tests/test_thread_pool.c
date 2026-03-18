/*
 * Master of Puppets — Test Suite
 * test_thread_pool.c — Phase 1B: Multi-Threaded Command Recording
 *
 * Tests the generic thread pool, render graph dependency analysis and
 * batched execution, and Vulkan per-thread resource structures.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/render_graph.h"
#include "core/thread_pool.h"
#include "test_harness.h"
#include <mop/mop.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Thread pool basic tests
 * ------------------------------------------------------------------------- */

static void test_threadpool_create_destroy(void) {
  TEST_BEGIN("threadpool_create_destroy");
  MopThreadPool *pool = mop_threadpool_create(4);
  TEST_ASSERT(pool != NULL);
  TEST_ASSERT(mop_threadpool_num_threads(pool) == 4);
  mop_threadpool_destroy(pool);
  TEST_END();
}

static void test_threadpool_single_thread(void) {
  TEST_BEGIN("threadpool_single_thread");
  MopThreadPool *pool = mop_threadpool_create(1);
  TEST_ASSERT(pool != NULL);
  TEST_ASSERT(mop_threadpool_num_threads(pool) == 1);
  mop_threadpool_destroy(pool);
  TEST_END();
}

/* Shared counter for task execution tests */
static volatile int g_counter = 0;

static void increment_task(void *arg) {
  (void)arg;
  __atomic_fetch_add(&g_counter, 1, __ATOMIC_RELAXED);
}

static void test_threadpool_submit_wait(void) {
  TEST_BEGIN("threadpool_submit_wait");
  MopThreadPool *pool = mop_threadpool_create(4);
  TEST_ASSERT(pool != NULL);

  g_counter = 0;
  int N = 1000;
  for (int i = 0; i < N; i++) {
    bool ok = mop_threadpool_submit(pool, increment_task, NULL);
    TEST_ASSERT(ok);
  }
  mop_threadpool_wait(pool);
  TEST_ASSERT(g_counter == N);

  mop_threadpool_destroy(pool);
  TEST_END();
}

/* Test that work is actually distributed across threads */
static volatile int g_thread_ids[256];
static volatile int g_tid_count = 0;

#include <pthread.h>
static void record_thread_id(void *arg) {
  (void)arg;
  /* Busy-spin briefly to ensure tasks overlap across workers */
  volatile int spin = 0;
  for (int i = 0; i < 10000; i++)
    spin++;
  (void)spin;
  /* Record this thread's ID (approximate via pthread_self) */
  int idx = __atomic_fetch_add(&g_tid_count, 1, __ATOMIC_RELAXED);
  if (idx < 256)
    g_thread_ids[idx] = (int)(uintptr_t)pthread_self();
}

static void test_threadpool_parallelism(void) {
  TEST_BEGIN("threadpool_parallelism");
  MopThreadPool *pool = mop_threadpool_create(4);
  TEST_ASSERT(pool != NULL);

  g_tid_count = 0;
  memset((void *)g_thread_ids, 0, sizeof(g_thread_ids));

  for (int i = 0; i < 200; i++)
    mop_threadpool_submit(pool, record_thread_id, NULL);
  mop_threadpool_wait(pool);

  /* At least 2 different thread IDs should be seen
   * (4 workers + tasks with busy-spin ensure overlap) */
  int unique = 1;
  for (int i = 1; i < g_tid_count && i < 256; i++) {
    bool found = false;
    for (int j = 0; j < i; j++) {
      if (g_thread_ids[i] == g_thread_ids[j]) {
        found = true;
        break;
      }
    }
    if (!found)
      unique++;
  }
  TEST_ASSERT(unique >= 2);

  mop_threadpool_destroy(pool);
  TEST_END();
}

static void test_threadpool_wait_empty(void) {
  TEST_BEGIN("threadpool_wait_empty");
  MopThreadPool *pool = mop_threadpool_create(2);
  TEST_ASSERT(pool != NULL);
  /* Wait on empty pool should return immediately */
  mop_threadpool_wait(pool);
  mop_threadpool_destroy(pool);
  TEST_END();
}

static void test_threadpool_multiple_waits(void) {
  TEST_BEGIN("threadpool_multiple_waits");
  MopThreadPool *pool = mop_threadpool_create(2);
  TEST_ASSERT(pool != NULL);

  g_counter = 0;
  for (int i = 0; i < 50; i++)
    mop_threadpool_submit(pool, increment_task, NULL);
  mop_threadpool_wait(pool);
  TEST_ASSERT(g_counter == 50);

  /* Submit more work after first wait */
  for (int i = 0; i < 50; i++)
    mop_threadpool_submit(pool, increment_task, NULL);
  mop_threadpool_wait(pool);
  TEST_ASSERT(g_counter == 100);

  mop_threadpool_destroy(pool);
  TEST_END();
}

static void test_threadpool_null_safety(void) {
  TEST_BEGIN("threadpool_null_safety");
  /* NULL pool operations should not crash */
  TEST_ASSERT(mop_threadpool_num_threads(NULL) == 0);
  mop_threadpool_wait(NULL);
  mop_threadpool_destroy(NULL);
  TEST_ASSERT(!mop_threadpool_submit(NULL, increment_task, NULL));
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Render graph dependency analysis tests
 * ------------------------------------------------------------------------- */

/* Dummy pass execute functions */
static void pass_nop(struct MopViewport *vp, void *ud) {
  (void)vp;
  (void)ud;
}

static void test_rg_compile_empty(void) {
  TEST_BEGIN("rg_compile_empty");
  MopRenderGraph rg;
  mop_rg_clear(&rg);
  uint32_t batches = mop_rg_compile(&rg);
  TEST_ASSERT(batches == 0);
  TEST_ASSERT(mop_rg_batch_count(&rg) == 0);
  TEST_END();
}

static void test_rg_compile_single_pass(void) {
  TEST_BEGIN("rg_compile_single_pass");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  MopRgPass p = {.name = "single",
                 .execute = pass_nop,
                 .writes = {MOP_RG_RES_COLOR_HDR},
                 .write_count = 1};
  mop_rg_add_pass(&rg, &p);

  uint32_t batches = mop_rg_compile(&rg);
  TEST_ASSERT(batches == 1);
  TEST_ASSERT(mop_rg_batch_size(&rg, 0) == 1);
  TEST_END();
}

static void test_rg_compile_independent_passes(void) {
  TEST_BEGIN("rg_compile_independent_passes");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  /* Two passes writing to different resources — can run in parallel */
  MopRgPass p1 = {.name = "shadow",
                  .execute = pass_nop,
                  .writes = {MOP_RG_RES_SHADOW_MAP},
                  .write_count = 1};
  MopRgPass p2 = {.name = "ssao",
                  .execute = pass_nop,
                  .writes = {MOP_RG_RES_SSAO},
                  .write_count = 1};
  mop_rg_add_pass(&rg, &p1);
  mop_rg_add_pass(&rg, &p2);

  uint32_t batches = mop_rg_compile(&rg);
  TEST_ASSERT(batches == 1); /* both in same batch */
  TEST_ASSERT(mop_rg_batch_size(&rg, 0) == 2);
  TEST_END();
}

static void test_rg_compile_dependent_passes(void) {
  TEST_BEGIN("rg_compile_dependent_passes");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  /* Pass 1 writes color, Pass 2 reads color — must be in different batches */
  MopRgPass p1 = {.name = "opaque",
                  .execute = pass_nop,
                  .writes = {MOP_RG_RES_COLOR_HDR},
                  .write_count = 1};
  MopRgPass p2 = {.name = "tonemap",
                  .execute = pass_nop,
                  .reads = {MOP_RG_RES_COLOR_HDR},
                  .read_count = 1,
                  .writes = {MOP_RG_RES_LDR_COLOR},
                  .write_count = 1};
  mop_rg_add_pass(&rg, &p1);
  mop_rg_add_pass(&rg, &p2);

  uint32_t batches = mop_rg_compile(&rg);
  TEST_ASSERT(batches == 2);
  TEST_ASSERT(mop_rg_batch_size(&rg, 0) == 1);
  TEST_ASSERT(mop_rg_batch_size(&rg, 1) == 1);
  TEST_END();
}

static void test_rg_compile_write_write_conflict(void) {
  TEST_BEGIN("rg_compile_write_write_conflict");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  /* Two passes both writing the same resource — conflict */
  MopRgPass p1 = {.name = "pass_a",
                  .execute = pass_nop,
                  .writes = {MOP_RG_RES_COLOR_HDR},
                  .write_count = 1};
  MopRgPass p2 = {.name = "pass_b",
                  .execute = pass_nop,
                  .writes = {MOP_RG_RES_COLOR_HDR},
                  .write_count = 1};
  mop_rg_add_pass(&rg, &p1);
  mop_rg_add_pass(&rg, &p2);

  uint32_t batches = mop_rg_compile(&rg);
  TEST_ASSERT(batches == 2);
  TEST_END();
}

static void test_rg_compile_no_resource_decl(void) {
  TEST_BEGIN("rg_compile_no_resource_decl");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  /* Pass with no resource declarations goes into its own batch
   * (can't be safely parallelized without dependency info) */
  MopRgPass p1 = {.name = "undeclared",
                  .execute = pass_nop,
                  .read_count = 0,
                  .write_count = 0};
  MopRgPass p2 = {.name = "shadow",
                  .execute = pass_nop,
                  .writes = {MOP_RG_RES_SHADOW_MAP},
                  .write_count = 1};
  mop_rg_add_pass(&rg, &p1);
  mop_rg_add_pass(&rg, &p2);

  uint32_t batches = mop_rg_compile(&rg);
  TEST_ASSERT(batches == 2);
  TEST_END();
}

static void test_rg_compile_sequential_flag(void) {
  TEST_BEGIN("rg_compile_sequential_flag");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  /* Sequential flag forces a new batch even for independent passes */
  MopRgPass p1 = {.name = "shadow",
                  .execute = pass_nop,
                  .writes = {MOP_RG_RES_SHADOW_MAP},
                  .write_count = 1};
  MopRgPass p2 = {.name = "ssao_seq",
                  .execute = pass_nop,
                  .writes = {MOP_RG_RES_SSAO},
                  .write_count = 1,
                  .flags = MOP_RG_FLAG_SEQUENTIAL};
  mop_rg_add_pass(&rg, &p1);
  mop_rg_add_pass(&rg, &p2);

  uint32_t batches = mop_rg_compile(&rg);
  TEST_ASSERT(batches == 2);
  TEST_END();
}

static void test_rg_compile_mixed_pipeline(void) {
  TEST_BEGIN("rg_compile_mixed_pipeline");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  /* Realistic pipeline:
   * 1. opaque: writes COLOR, DEPTH, PICK
   * 2. shadow: writes SHADOW_MAP (independent of opaque results)
   * 3. tonemap: reads COLOR, writes LDR
   * 4. ssao: reads DEPTH, writes SSAO
   * 5. overlay: reads LDR, writes OVERLAY_BUF
   *
   * Expected batching:
   *   Batch 0: [opaque] — opaque has no resource decls? Actually it does.
   *   Wait, let me think...
   *   opaque writes COLOR+DEPTH+PICK
   *   shadow writes SHADOW_MAP → no conflict with opaque? Actually opaque
   *   could be reading shadow. Let me make them conflict-free for this test.
   */
  MopRgPass opaque = {
      .name = "opaque",
      .execute = pass_nop,
      .writes = {MOP_RG_RES_COLOR_HDR, MOP_RG_RES_DEPTH, MOP_RG_RES_PICK},
      .write_count = 3};
  MopRgPass tonemap = {.name = "tonemap",
                       .execute = pass_nop,
                       .reads = {MOP_RG_RES_COLOR_HDR},
                       .read_count = 1,
                       .writes = {MOP_RG_RES_LDR_COLOR},
                       .write_count = 1};
  MopRgPass ssao = {.name = "ssao",
                    .execute = pass_nop,
                    .reads = {MOP_RG_RES_DEPTH},
                    .read_count = 1,
                    .writes = {MOP_RG_RES_SSAO},
                    .write_count = 1};
  MopRgPass overlay = {.name = "overlay",
                       .execute = pass_nop,
                       .reads = {MOP_RG_RES_LDR_COLOR},
                       .read_count = 1,
                       .writes = {MOP_RG_RES_OVERLAY_BUF},
                       .write_count = 1};

  mop_rg_add_pass(&rg, &opaque);
  mop_rg_add_pass(&rg, &tonemap);
  mop_rg_add_pass(&rg, &ssao);
  mop_rg_add_pass(&rg, &overlay);

  uint32_t batches = mop_rg_compile(&rg);

  /* opaque writes COLOR+DEPTH → tonemap reads COLOR (conflict) → new batch
   * tonemap writes LDR, ssao reads DEPTH (DEPTH written by opaque, but
   * ssao doesn't conflict with tonemap since tonemap only writes LDR and
   * reads COLOR) → ssao can join batch with tonemap
   * overlay reads LDR (written by tonemap in same batch) → conflict → new batch
   *
   * Batch 0: [opaque]
   * Batch 1: [tonemap, ssao]  (no conflict: different read/write sets)
   * Batch 2: [overlay]
   */
  TEST_ASSERT(batches == 3);
  TEST_ASSERT(mop_rg_batch_size(&rg, 1) == 2); /* tonemap + ssao parallel */
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Render graph MT execution test
 * ------------------------------------------------------------------------- */

static volatile int g_pass_results[4];

static void pass_write_0(struct MopViewport *vp, void *ud) {
  (void)vp;
  (void)ud;
  __atomic_store_n(&g_pass_results[0], 1, __ATOMIC_RELEASE);
}
static void pass_write_1(struct MopViewport *vp, void *ud) {
  (void)vp;
  (void)ud;
  __atomic_store_n(&g_pass_results[1], 1, __ATOMIC_RELEASE);
}
static void pass_write_2(struct MopViewport *vp, void *ud) {
  (void)vp;
  (void)ud;
  __atomic_store_n(&g_pass_results[2], 1, __ATOMIC_RELEASE);
}
static void pass_write_3(struct MopViewport *vp, void *ud) {
  (void)vp;
  (void)ud;
  __atomic_store_n(&g_pass_results[3], 1, __ATOMIC_RELEASE);
}

static void test_rg_execute_mt(void) {
  TEST_BEGIN("rg_execute_mt");

  MopThreadPool *pool = mop_threadpool_create(2);
  TEST_ASSERT(pool != NULL);

  MopRenderGraph rg;
  mop_rg_clear(&rg);

  memset((void *)g_pass_results, 0, sizeof(g_pass_results));

  /* 4 independent passes writing to different resources */
  MopRgPass p0 = {.name = "shadow",
                  .execute = pass_write_0,
                  .writes = {MOP_RG_RES_SHADOW_MAP},
                  .write_count = 1};
  MopRgPass p1 = {.name = "ssao",
                  .execute = pass_write_1,
                  .writes = {MOP_RG_RES_SSAO},
                  .write_count = 1};
  MopRgPass p2 = {.name = "bloom",
                  .execute = pass_write_2,
                  .writes = {MOP_RG_RES_BLOOM},
                  .write_count = 1};
  MopRgPass p3 = {.name = "overlay",
                  .execute = pass_write_3,
                  .writes = {MOP_RG_RES_OVERLAY_BUF},
                  .write_count = 1};

  mop_rg_add_pass(&rg, &p0);
  mop_rg_add_pass(&rg, &p1);
  mop_rg_add_pass(&rg, &p2);
  mop_rg_add_pass(&rg, &p3);

  mop_rg_compile(&rg);
  mop_rg_execute_mt(&rg, NULL, pool);

  /* All passes should have executed */
  TEST_ASSERT(g_pass_results[0] == 1);
  TEST_ASSERT(g_pass_results[1] == 1);
  TEST_ASSERT(g_pass_results[2] == 1);
  TEST_ASSERT(g_pass_results[3] == 1);

  mop_threadpool_destroy(pool);
  TEST_END();
}

static void test_rg_execute_mt_fallback(void) {
  TEST_BEGIN("rg_execute_mt_fallback");

  /* NULL pool → should fall back to sequential execution */
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  memset((void *)g_pass_results, 0, sizeof(g_pass_results));

  MopRgPass p0 = {.name = "p0",
                  .execute = pass_write_0,
                  .writes = {MOP_RG_RES_COLOR_HDR},
                  .write_count = 1};
  mop_rg_add_pass(&rg, &p0);

  /* Not compiled → should fall back to sequential */
  mop_rg_execute_mt(&rg, NULL, NULL);
  TEST_ASSERT(g_pass_results[0] == 1);
  TEST_END();
}

static void test_rg_clear_invalidates(void) {
  TEST_BEGIN("rg_clear_invalidates");
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  MopRgPass p = {.name = "test",
                 .execute = pass_nop,
                 .writes = {MOP_RG_RES_COLOR_HDR},
                 .write_count = 1};
  mop_rg_add_pass(&rg, &p);
  mop_rg_compile(&rg);
  TEST_ASSERT(mop_rg_batch_count(&rg) == 1);

  /* Clear should invalidate the compiled schedule */
  mop_rg_clear(&rg);
  TEST_ASSERT(mop_rg_batch_count(&rg) == 0);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Vulkan per-thread struct layout tests
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"

static void test_vk_thread_state_fields(void) {
  TEST_BEGIN("vk_thread_state_fields");
  MopVkThreadState ts;
  memset(&ts, 0, sizeof(ts));
  TEST_ASSERT(ts.cmd_pool == VK_NULL_HANDLE);
  TEST_ASSERT(ts.secondary_cb == VK_NULL_HANDLE);
  TEST_ASSERT(ts.desc_pool == VK_NULL_HANDLE);
  TEST_ASSERT(ts.cb_recording == false);
  TEST_END();
}

static void test_vk_device_thread_count(void) {
  TEST_BEGIN("vk_device_thread_count");
  MopRhiDevice dev;
  memset(&dev, 0, sizeof(dev));
  TEST_ASSERT(dev.thread_count == 0);
  /* Max threads constant should be reasonable */
  TEST_ASSERT(MOP_VK_MAX_WORKER_THREADS >= 4);
  TEST_ASSERT(MOP_VK_MAX_WORKER_THREADS <= 64);
  TEST_END();
}
#endif /* MOP_HAS_VULKAN */

/* -------------------------------------------------------------------------
 * Render graph struct size test
 * ------------------------------------------------------------------------- */

static void test_rg_struct_sizes(void) {
  TEST_BEGIN("rg_struct_sizes");
  /* MopRgBatch should fit in a cache line or two */
  TEST_ASSERT(sizeof(MopRgBatch) <= 128);
  /* MopRenderGraph should be reasonable for stack allocation */
  TEST_ASSERT(sizeof(MopRenderGraph) < 65536); /* < 64 KB */
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("thread_pool");

  /* Thread pool */
  test_threadpool_create_destroy();
  test_threadpool_single_thread();
  test_threadpool_submit_wait();
  test_threadpool_parallelism();
  test_threadpool_wait_empty();
  test_threadpool_multiple_waits();
  test_threadpool_null_safety();

  /* Render graph compilation */
  test_rg_compile_empty();
  test_rg_compile_single_pass();
  test_rg_compile_independent_passes();
  test_rg_compile_dependent_passes();
  test_rg_compile_write_write_conflict();
  test_rg_compile_no_resource_decl();
  test_rg_compile_sequential_flag();
  test_rg_compile_mixed_pipeline();

  /* Render graph MT execution */
  test_rg_execute_mt();
  test_rg_execute_mt_fallback();
  test_rg_clear_invalidates();

  /* Struct sizes */
  test_rg_struct_sizes();

#if defined(MOP_HAS_VULKAN)
  test_vk_thread_state_fields();
  test_vk_device_thread_count();
#endif

  TEST_REPORT();
  TEST_EXIT();
}
