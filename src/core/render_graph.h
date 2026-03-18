/*
 * Master of Puppets — Render Graph
 * render_graph.h — DAG-based frame pass management
 *
 * Each frame, the viewport builds a render graph of named passes.
 * Passes declare resource reads/writes.  The graph analyzes dependencies
 * and groups independent passes into parallel batches for multi-threaded
 * command recording.  Falls back to linear execution when no thread pool
 * is available or when all passes are sequential.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_RENDER_GRAPH_H
#define MOP_RENDER_GRAPH_H

#include <stdbool.h>
#include <stdint.h>

struct MopViewport;
struct MopThreadPool;

/* -------------------------------------------------------------------------
 * Virtual resource IDs for dependency tracking.
 * Passes declare which resources they read/write.
 * Used for automatic dependency analysis and barrier insertion.
 * ------------------------------------------------------------------------- */

typedef enum MopRgResourceId {
  MOP_RG_RES_COLOR_HDR = 0, /* Main HDR color attachment */
  MOP_RG_RES_DEPTH,         /* Main depth attachment */
  MOP_RG_RES_PICK,          /* Object-ID attachment */
  MOP_RG_RES_SHADOW_MAP,    /* Shadow depth map */
  MOP_RG_RES_LDR_COLOR,     /* Post-tonemap LDR output */
  MOP_RG_RES_SSAO,          /* SSAO buffer */
  MOP_RG_RES_BLOOM,         /* Bloom mip chain */
  MOP_RG_RES_OVERLAY_BUF,   /* Overlay SSBO / command buffer */
  MOP_RG_RES_COUNT
} MopRgResourceId;

/* -------------------------------------------------------------------------
 * Pass flags
 * ------------------------------------------------------------------------- */

#define MOP_RG_FLAG_NONE 0
#define MOP_RG_FLAG_SEQUENTIAL (1u << 0) /* Force sequential execution */

/* -------------------------------------------------------------------------
 * Pass node
 * ------------------------------------------------------------------------- */

#define MOP_RG_MAX_PASS_RESOURCES 8
#define MOP_RG_MAX_PASSES 48

typedef void (*MopRgExecuteFn)(struct MopViewport *vp, void *user_data);

typedef struct MopRgPass {
  const char *name;
  MopRgExecuteFn execute;
  void *user_data;

  /* Resource declarations (reads before writes for barrier ordering) */
  MopRgResourceId reads[MOP_RG_MAX_PASS_RESOURCES];
  uint32_t read_count;
  MopRgResourceId writes[MOP_RG_MAX_PASS_RESOURCES];
  uint32_t write_count;

  /* Pass flags (MOP_RG_FLAG_*) */
  uint32_t flags;
} MopRgPass;

/* -------------------------------------------------------------------------
 * Execution batch — group of passes that can run concurrently.
 * Within a batch, passes have no resource conflicts.
 * Batches execute sequentially; passes within a batch execute in parallel.
 * ------------------------------------------------------------------------- */

#define MOP_RG_MAX_BATCH_SIZE 16

typedef struct MopRgBatch {
  uint32_t pass_indices[MOP_RG_MAX_BATCH_SIZE];
  uint32_t count;
} MopRgBatch;

/* -------------------------------------------------------------------------
 * Render graph — per-frame pass sequence
 * ------------------------------------------------------------------------- */

typedef struct MopRenderGraph {
  MopRgPass passes[MOP_RG_MAX_PASSES];
  uint32_t pass_count;

  /* Compiled execution schedule (batches of independent passes).
   * Built by mop_rg_compile(), consumed by mop_rg_execute_mt(). */
  MopRgBatch batches[MOP_RG_MAX_PASSES]; /* worst case: 1 pass per batch */
  uint32_t batch_count;
  bool compiled;
} MopRenderGraph;

/* Clear the graph for a new frame */
void mop_rg_clear(MopRenderGraph *rg);

/* Add a pass. Returns pass index, or UINT32_MAX if full. */
uint32_t mop_rg_add_pass(MopRenderGraph *rg, const MopRgPass *pass);

/* Compile the graph: analyze resource dependencies and build execution
 * batches.  Must be called after all passes are added and before execute.
 * Returns the number of batches. */
uint32_t mop_rg_compile(MopRenderGraph *rg);

/* Execute all passes in registration order (single-threaded fallback) */
void mop_rg_execute(MopRenderGraph *rg, struct MopViewport *vp);

/* Execute passes using the compiled batch schedule.
 * Passes within the same batch run in parallel on the thread pool.
 * Falls back to sequential execution if pool is NULL or graph is
 * not compiled.  */
void mop_rg_execute_mt(MopRenderGraph *rg, struct MopViewport *vp,
                       struct MopThreadPool *pool);

/* Query the compiled schedule */
uint32_t mop_rg_batch_count(const MopRenderGraph *rg);
uint32_t mop_rg_batch_size(const MopRenderGraph *rg, uint32_t batch_idx);

#endif /* MOP_RENDER_GRAPH_H */
