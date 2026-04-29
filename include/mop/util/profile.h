/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * profile.h — Frame profiling statistics
 *
 * After each call to mop_viewport_render, timing data is stored in the
 * viewport.  Retrieve it with mop_viewport_get_stats to display or log
 * performance information.
 *
 * Per-pass GPU timing is available when the backend supports timestamp
 * queries.  Use mop_viewport_get_gpu_pass_timings() to retrieve per-pass
 * timing data for the most recent frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_UTIL_PROFILE_H
#define MOP_UTIL_PROFILE_H

#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Per-pass GPU timing (Phase 9A)
 *
 * Each render graph pass is timed via GPU timestamp queries.
 * Results are available after frame completion (1-frame latency).
 * ------------------------------------------------------------------------- */

#define MOP_MAX_GPU_PASS_TIMINGS 48

typedef struct MopGpuPassTiming {
  const char *name; /* pass name (pointer into render graph — valid until
                       next frame) */
  double gpu_ms;    /* GPU time in milliseconds */
} MopGpuPassTiming;

/* -------------------------------------------------------------------------
 * Frame statistics
 *
 * All time values are in milliseconds.
 * triangle_count : total triangles submitted (index_count / 3 per mesh)
 * pixel_count    : total framebuffer pixels (width * height)
 * ------------------------------------------------------------------------- */

typedef struct MopFrameStats {
  /* CPU timing */
  double frame_time_ms;
  double clear_ms;
  double transform_ms;
  double rasterize_ms;

  /* Geometry counters */
  uint32_t triangle_count;
  uint32_t pixel_count;
  uint32_t draw_call_count; /* total draw calls submitted */
  uint32_t vertex_count;    /* total vertices submitted */

  /* GPU culling stats (Phase 2B/2C) */
  uint32_t visible_draws; /* draws that passed GPU culling */
  uint32_t culled_draws;  /* draws removed by GPU culling */

  /* GPU timing (0.0 if not available) */
  double gpu_frame_ms; /* total GPU frame time */

  /* Per-pass GPU timing (populated by backend) */
  MopGpuPassTiming pass_timings[MOP_MAX_GPU_PASS_TIMINGS];
  uint32_t pass_timing_count;

  /* LOD statistics */
  uint32_t lod_transitions; /* meshes that changed LOD this frame */

  /* Memory usage (bytes, 0 if not available) */
  uint64_t gpu_memory_used;
  uint64_t gpu_memory_budget;
} MopFrameStats;

/* Retrieve the statistics from the most recent mop_viewport_render call.
 * Returns a zeroed struct if viewport is NULL. */
MopFrameStats mop_viewport_get_stats(const MopViewport *viewport);

/* Return the GPU frame time in milliseconds for the last completed frame.
 * Returns 0.0 if GPU timing is not available.
 * Note: declaration matches viewport.h (non-const viewport pointer). */

#ifdef __cplusplus
}
#endif

#endif /* MOP_UTIL_PROFILE_H */
