/*
 * Master of Puppets — Render Graph
 * render_graph.c — Pass registration, dependency analysis, and execution
 *
 * Dependency analysis:
 *   Two passes conflict (must not run in parallel) if one writes a
 *   resource that the other reads or writes.  The graph builds execution
 *   batches of non-conflicting passes using a greedy left-to-right scan
 *   that preserves registration order within each batch.
 *
 * Parallel execution:
 *   Passes within the same batch are submitted to a MopThreadPool.
 *   The main thread waits for the batch to complete before starting the
 *   next batch.  This gives implicit barrier semantics between batches.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "render_graph.h"
#include "thread_pool.h"
#include <mop/util/log.h>

#include <string.h>

/* -------------------------------------------------------------------------
 * Resource conflict detection
 * ------------------------------------------------------------------------- */

/* Per-resource state: bitmask of which resources are written in the
 * current batch.  Read-read is safe; read-write or write-write conflicts. */

static bool passes_conflict(const MopRgPass *a, const MopRgPass *b) {
  /* Check if a writes something b reads or writes, or vice versa */
  for (uint32_t aw = 0; aw < a->write_count; aw++) {
    MopRgResourceId r = a->writes[aw];
    for (uint32_t br = 0; br < b->read_count; br++) {
      if (b->reads[br] == r)
        return true;
    }
    for (uint32_t bw = 0; bw < b->write_count; bw++) {
      if (b->writes[bw] == r)
        return true;
    }
  }
  for (uint32_t bw = 0; bw < b->write_count; bw++) {
    MopRgResourceId r = b->writes[bw];
    for (uint32_t ar = 0; ar < a->read_count; ar++) {
      if (a->reads[ar] == r)
        return true;
    }
  }
  return false;
}

/* -------------------------------------------------------------------------
 * Graph compilation: build execution batches
 *
 * Greedy algorithm:
 *   1. For each pass in registration order:
 *      a. Try to add it to the current batch (check conflicts with all
 *         passes already in the batch).
 *      b. If it conflicts (or is marked SEQUENTIAL), close the current
 *         batch and start a new one with this pass.
 *   2. Close the final batch.
 *
 * This preserves rendering order (important for Vulkan render passes
 * that share attachments) while identifying parallelism opportunities.
 * ------------------------------------------------------------------------- */

uint32_t mop_rg_compile(MopRenderGraph *rg) {
  if (!rg || rg->pass_count == 0) {
    if (rg)
      rg->compiled = true;
    return 0;
  }

  rg->batch_count = 0;
  memset(rg->batches, 0, sizeof(rg->batches));

  /* Start first batch */
  MopRgBatch *batch = &rg->batches[0];
  rg->batch_count = 1;

  for (uint32_t i = 0; i < rg->pass_count; i++) {
    const MopRgPass *pass = &rg->passes[i];

    /* Check if this pass can join the current batch */
    bool can_join = true;

    /* Sequential flag forces a new batch */
    if (pass->flags & MOP_RG_FLAG_SEQUENTIAL)
      can_join = false;

    /* Passes with no resource declarations can't be safely parallelized
     * (we don't know their dependencies) */
    if (pass->read_count == 0 && pass->write_count == 0)
      can_join = false;

    /* Check conflicts with all passes already in the batch */
    if (can_join) {
      for (uint32_t j = 0; j < batch->count; j++) {
        if (passes_conflict(pass, &rg->passes[batch->pass_indices[j]])) {
          can_join = false;
          break;
        }
      }
    }

    /* Batch is full */
    if (can_join && batch->count >= MOP_RG_MAX_BATCH_SIZE)
      can_join = false;

    if (!can_join && batch->count > 0) {
      /* Start a new batch */
      rg->batch_count++;
      if (rg->batch_count > MOP_RG_MAX_PASSES) {
        MOP_WARN("render graph: too many batches, truncating");
        rg->batch_count = MOP_RG_MAX_PASSES;
        break;
      }
      batch = &rg->batches[rg->batch_count - 1];
    }

    batch->pass_indices[batch->count++] = i;

    /* If this pass has no resource declarations, close the batch —
     * we can't verify that the next pass is safe to parallelize with it. */
    if (pass->read_count == 0 && pass->write_count == 0) {
      rg->batch_count++;
      if (rg->batch_count > MOP_RG_MAX_PASSES) {
        rg->batch_count = MOP_RG_MAX_PASSES;
        break;
      }
      batch = &rg->batches[rg->batch_count - 1];
    }
  }

  rg->compiled = true;
  return rg->batch_count;
}

/* -------------------------------------------------------------------------
 * Pass management
 * ------------------------------------------------------------------------- */

void mop_rg_clear(MopRenderGraph *rg) {
  rg->pass_count = 0;
  rg->batch_count = 0;
  rg->compiled = false;
}

uint32_t mop_rg_add_pass(MopRenderGraph *rg, const MopRgPass *pass) {
  if (rg->pass_count >= MOP_RG_MAX_PASSES) {
    MOP_WARN("render graph full (%d passes), dropping '%s'", MOP_RG_MAX_PASSES,
             pass->name);
    return UINT32_MAX;
  }
  uint32_t idx = rg->pass_count++;
  rg->passes[idx] = *pass;
  rg->compiled = false; /* invalidate compiled schedule */
  return idx;
}

/* -------------------------------------------------------------------------
 * Sequential execution (fallback)
 * ------------------------------------------------------------------------- */

void mop_rg_execute(MopRenderGraph *rg, struct MopViewport *vp) {
  for (uint32_t i = 0; i < rg->pass_count; i++) {
    if (rg->passes[i].execute)
      rg->passes[i].execute(vp, rg->passes[i].user_data);
  }
}

/* -------------------------------------------------------------------------
 * Multi-threaded execution
 * ------------------------------------------------------------------------- */

/* Per-task argument for thread pool dispatch */
typedef struct MopRgTaskArg {
  MopRgExecuteFn execute;
  struct MopViewport *vp;
  void *user_data;
} MopRgTaskArg;

static void rg_task_fn(void *arg) {
  MopRgTaskArg *task = (MopRgTaskArg *)arg;
  task->execute(task->vp, task->user_data);
}

void mop_rg_execute_mt(MopRenderGraph *rg, struct MopViewport *vp,
                       struct MopThreadPool *pool) {
  if (!rg || rg->pass_count == 0)
    return;

  /* Fallback to sequential if no pool or not compiled */
  if (!pool || !rg->compiled) {
    mop_rg_execute(rg, vp);
    return;
  }

  /* Stack-allocated task args (max passes per batch) */
  MopRgTaskArg task_args[MOP_RG_MAX_BATCH_SIZE];

  for (uint32_t b = 0; b < rg->batch_count; b++) {
    const MopRgBatch *batch = &rg->batches[b];

    if (batch->count == 1) {
      /* Single-pass batch: execute directly on main thread */
      const MopRgPass *pass = &rg->passes[batch->pass_indices[0]];
      if (pass->execute)
        pass->execute(vp, pass->user_data);
      continue;
    }

    /* Multi-pass batch: dispatch to thread pool */
    uint32_t submitted = 0;
    for (uint32_t p = 0; p < batch->count; p++) {
      const MopRgPass *pass = &rg->passes[batch->pass_indices[p]];
      if (!pass->execute)
        continue;

      task_args[submitted] = (MopRgTaskArg){
          .execute = pass->execute,
          .vp = vp,
          .user_data = pass->user_data,
      };
      mop_threadpool_submit(pool, rg_task_fn, &task_args[submitted]);
      submitted++;
    }

    /* Wait for all passes in this batch to complete before next batch */
    if (submitted > 0)
      mop_threadpool_wait(pool);
  }
}

/* -------------------------------------------------------------------------
 * Query API
 * ------------------------------------------------------------------------- */

uint32_t mop_rg_batch_count(const MopRenderGraph *rg) {
  return rg && rg->compiled ? rg->batch_count : 0;
}

uint32_t mop_rg_batch_size(const MopRenderGraph *rg, uint32_t batch_idx) {
  if (!rg || !rg->compiled || batch_idx >= rg->batch_count)
    return 0;
  return rg->batches[batch_idx].count;
}
