/*
 * Master of Puppets — Test Suite
 * test_scene_threading.c — Concurrent scene mutation + render stress test
 *
 * Spawns a render thread that runs alongside worker threads that
 * concurrently add/remove meshes, mutate transforms, and change materials.
 * Validates:
 *
 *   - mop_viewport_render runs to completion without crashing
 *   - MopMesh* handles stay valid across pool growth (one thread holds
 *     handles while others force reallocs)
 *   - Concurrent mutation + render produces no use-after-free / torn reads
 *   - Add/remove counts balance at the end
 *
 * Run under tsan for full race detection:
 *   make SANITIZE=tsan test
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MUTATOR_THREADS 4
#define MUTATIONS_PER_THREAD 200

static const MopVertex TRI_V[3] = {
    {.position = {0, 1, 0}, .normal = {0, 0, 1}, .color = {1, 0, 0, 1}},
    {.position = {-1, -1, 0}, .normal = {0, 0, 1}, .color = {0, 1, 0, 1}},
    {.position = {1, -1, 0}, .normal = {0, 0, 1}, .color = {0, 0, 1, 1}},
};
static const uint32_t TRI_I[3] = {0, 1, 2};

typedef struct {
  MopViewport *vp;
  int thread_id;
  _Atomic int adds;
  _Atomic int removes;
  _Atomic int mutations;
} WorkerCtx;

typedef struct {
  MopViewport *vp;
  _Atomic int frames_done;
  _Atomic int stop;
  _Atomic int render_errors;
} RenderCtx;

static _Atomic int g_total_adds = 0;
static _Atomic int g_total_removes = 0;

static void *worker_main(void *arg) {
  WorkerCtx *w = (WorkerCtx *)arg;
  enum { OWN_CAP = 32 };
  MopMesh *own[OWN_CAP] = {0};
  uint32_t own_count = 0;
  unsigned seed = (unsigned)w->thread_id * 2654435761u;

  for (int i = 0; i < MUTATIONS_PER_THREAD; i++) {
    unsigned r = seed = seed * 1103515245u + 12345u;
    int op = (int)((r >> 16) & 0x3);

    if (op == 0 || own_count == 0) {
      MopMesh *m = mop_viewport_add_mesh(
          w->vp,
          &(MopMeshDesc){.vertices = TRI_V,
                         .vertex_count = 3,
                         .indices = TRI_I,
                         .index_count = 3,
                         .object_id =
                             (uint32_t)(w->thread_id * 1000 +
                                        atomic_fetch_add(&g_total_adds, 1))});
      if (m && own_count < OWN_CAP) {
        own[own_count++] = m;
        atomic_fetch_add(&w->adds, 1);
      }
    } else if (op == 1 && own_count > 0) {
      uint32_t idx = ((r >> 8) & 0xFFFF) % own_count;
      MopMesh *m = own[idx];
      mop_viewport_remove_mesh(w->vp, m);
      own[idx] = own[--own_count];
      atomic_fetch_add(&w->removes, 1);
      atomic_fetch_add(&g_total_removes, 1);
    } else if (own_count > 0) {
      uint32_t idx = ((r >> 8) & 0xFFFF) % own_count;
      MopMesh *m = own[idx];
      float angle = (float)i * 0.01f;
      mop_mesh_set_rotation(m, (MopVec3){0, angle, 0});
      mop_mesh_set_position(
          m, (MopVec3){(float)((r & 0xFF) - 128) / 128.0f, 0, 0});
      MopMaterial mat = mop_material_default();
      mat.metallic = (float)((r >> 4) & 0xFF) / 255.0f;
      mat.roughness = (float)((r >> 12) & 0xFF) / 255.0f;
      mop_mesh_set_material(m, &mat);
      atomic_fetch_add(&w->mutations, 1);
    }
    if ((i & 0xF) == 0)
      sched_yield();
  }

  for (uint32_t k = 0; k < own_count; k++) {
    mop_viewport_remove_mesh(w->vp, own[k]);
    atomic_fetch_add(&w->removes, 1);
    atomic_fetch_add(&g_total_removes, 1);
  }
  return NULL;
}

static void *render_main(void *arg) {
  RenderCtx *r = (RenderCtx *)arg;
  while (!atomic_load(&r->stop)) {
    MopRenderResult rr = mop_viewport_render(r->vp);
    if (rr != MOP_RENDER_OK)
      atomic_fetch_add(&r->render_errors, 1);
    atomic_fetch_add(&r->frames_done, 1);
    usleep(500);
  }
  return NULL;
}

int main(void) {
  printf("\n────────────────────────────────────────\n");
  printf("  scene_threading\n");
  printf("────────────────────────────────────────\n");

  /* ---- Test 1: handle stability under pool growth ---- */
  TEST_BEGIN("handle_stability_under_growth") {
    MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
        .width = 320,
        .height = 240,
        .backend = MOP_BACKEND_CPU,
        .ssaa_factor = 1,
    });
    TEST_ASSERT(vp != NULL);

    MopMesh *held[8];
    for (int i = 0; i < 8; i++) {
      held[i] = mop_viewport_add_mesh(
          vp, &(MopMeshDesc){.vertices = TRI_V,
                             .vertex_count = 3,
                             .indices = TRI_I,
                             .index_count = 3,
                             .object_id = (uint32_t)(9000 + i)});
      TEST_ASSERT(held[i] != NULL);
    }

    /* Force many grows: add 1024 more meshes. */
    MopMesh *churn[1024];
    for (int i = 0; i < 1024; i++) {
      churn[i] = mop_viewport_add_mesh(
          vp, &(MopMeshDesc){.vertices = TRI_V,
                             .vertex_count = 3,
                             .indices = TRI_I,
                             .index_count = 3,
                             .object_id = (uint32_t)(10000 + i)});
      TEST_ASSERT(churn[i] != NULL);
    }

    /* Held handles must still be live. */
    for (int i = 0; i < 8; i++) {
      mop_mesh_set_rotation(held[i], (MopVec3){0, (float)i, 0});
      MopVec3 r = mop_mesh_get_rotation(held[i]);
      TEST_ASSERT(fabsf(r.y - (float)i) < 1e-4f);
    }
    for (int i = 0; i < 1024; i++)
      mop_viewport_remove_mesh(vp, churn[i]);
    for (int i = 0; i < 8; i++)
      mop_mesh_set_position(held[i], (MopVec3){(float)i, 0, 0});

    mop_viewport_destroy(vp);
  }
  TEST_END();

  /* ---- Test 2: concurrent add/remove/mutate + render ---- */
  TEST_BEGIN("concurrent_add_remove_mutate") {
    MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
        .width = 320,
        .height = 240,
        .backend = MOP_BACKEND_CPU,
        .ssaa_factor = 1,
    });
    TEST_ASSERT(vp != NULL);
    mop_viewport_set_camera(vp, (MopVec3){3, 3, 5}, (MopVec3){0, 0, 0},
                            (MopVec3){0, 1, 0}, 45.0f, 0.1f, 100.0f);
    mop_viewport_set_chrome(vp, false);

    atomic_store(&g_total_adds, 0);
    atomic_store(&g_total_removes, 0);

    WorkerCtx workers[MUTATOR_THREADS];
    pthread_t worker_threads[MUTATOR_THREADS];
    for (int i = 0; i < MUTATOR_THREADS; i++) {
      workers[i] = (WorkerCtx){.vp = vp, .thread_id = i};
      pthread_create(&worker_threads[i], NULL, worker_main, &workers[i]);
    }

    RenderCtx rctx = {.vp = vp};
    pthread_t render_thread;
    pthread_create(&render_thread, NULL, render_main, &rctx);

    for (int i = 0; i < MUTATOR_THREADS; i++)
      pthread_join(worker_threads[i], NULL);

    atomic_store(&rctx.stop, 1);
    pthread_join(render_thread, NULL);

    int total_adds = atomic_load(&g_total_adds);
    int total_removes = atomic_load(&g_total_removes);
    int frames = atomic_load(&rctx.frames_done);
    int errors = atomic_load(&rctx.render_errors);

    printf("       %d adds, %d removes, %d frames rendered, %d errors\n",
           total_adds, total_removes, frames, errors);

    TEST_ASSERT(total_adds == total_removes);
    TEST_ASSERT(frames > 0);
    TEST_ASSERT(errors == 0);
    TEST_ASSERT(mop_viewport_mesh_count(vp) == 0);

    mop_viewport_destroy(vp);
  }
  TEST_END();

  /* ---- Test 3: undo/redo survives pool churn ----
   * Push undo entries for meshes that then get removed. Undo / redo must
   * not crash on the stale mesh_index entries — they should skip safely. */
  TEST_BEGIN("undo_redo_after_mesh_removal") {
    MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
        .width = 320,
        .height = 240,
        .backend = MOP_BACKEND_CPU,
        .ssaa_factor = 1,
    });
    TEST_ASSERT(vp != NULL);

    MopMesh *a = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = TRI_V,
                                                          .vertex_count = 3,
                                                          .indices = TRI_I,
                                                          .index_count = 3,
                                                          .object_id = 5001});
    MopMesh *b = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = TRI_V,
                                                          .vertex_count = 3,
                                                          .indices = TRI_I,
                                                          .index_count = 3,
                                                          .object_id = 5002});
    TEST_ASSERT(a != NULL && b != NULL);

    mop_mesh_set_position(a, (MopVec3){1, 0, 0});
    mop_mesh_set_position(b, (MopVec3){2, 0, 0});

    mop_viewport_push_undo(vp, a);
    mop_viewport_push_undo(vp, b);

    /* Remove a; the undo entry for a now refers to a recycled slot. */
    mop_viewport_remove_mesh(vp, a);

    /* Undo/redo must not crash. The entry for 'b' replays correctly;
     * the entry for 'a' either no-ops or hits a recycled slot that's
     * been reused — the safe paths in undo.c gate on mesh active state. */
    mop_viewport_undo(vp);
    mop_viewport_undo(vp);
    mop_viewport_redo(vp);
    mop_viewport_redo(vp);

    mop_viewport_destroy(vp);
  }
  TEST_END();

  TEST_REPORT();
  TEST_EXIT();
}
