/*
 * Master of Puppets — Conformance Framework
 * conformance.h — Public API for conformance runner
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CONFORMANCE_H
#define MOP_CONFORMANCE_H

#include <mop/mop.h>
#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Tier levels
 * ------------------------------------------------------------------------- */

typedef enum MopConfTier {
  MOP_CONF_TIER1 = 1, /* Quick: < 60s, geometric + depth + picking */
  MOP_CONF_TIER2 = 2, /* Standard: < 10 min, 3 paths + metrics */
  MOP_CONF_TIER3 = 3, /* Full: < 60 min, all 7 paths + oracle */
  MOP_CONF_TIER4 = 4, /* Exhaustive: < 4 hr, all configs × all paths */
} MopConfTier;

/* -------------------------------------------------------------------------
 * Verdict
 * ------------------------------------------------------------------------- */

typedef enum MopConfVerdict {
  MOP_CONF_PASS = 0,
  MOP_CONF_WARN = 1,
  MOP_CONF_FAIL = 2,
} MopConfVerdict;

/* -------------------------------------------------------------------------
 * Per-frame metrics
 * ------------------------------------------------------------------------- */

typedef struct MopConformanceMetrics {
  double rmse;
  double ssim;
  double psnr;
  double histogram_chi2;
  double edge_f1;
  double depth_rmse;
  double normal_cosine_err;
  uint32_t pick_mismatches;
  double temporal_ssim_min;
  double temporal_ssim_mean;
  double temporal_flicker;
} MopConformanceMetrics;

/* -------------------------------------------------------------------------
 * Camera path IDs
 * ------------------------------------------------------------------------- */

typedef enum MopCameraPathId {
  MOP_PATH_ORBIT = 0,
  MOP_PATH_ZOOM,
  MOP_PATH_FOV_SWEEP,
  MOP_PATH_JITTER,
  MOP_PATH_EXTREME_NEAR,
  MOP_PATH_HIERARCHY_FLY,
  MOP_PATH_TRANSPARENCY,
  MOP_PATH_COUNT,
} MopCameraPathId;

/* -------------------------------------------------------------------------
 * Lighting config IDs
 * ------------------------------------------------------------------------- */

typedef enum MopLightConfigId {
  MOP_LIGHT_CONFIG_SUNONLY = 0,
  MOP_LIGHT_CONFIG_POINTS,
  MOP_LIGHT_CONFIG_SPOTS,
  MOP_LIGHT_CONFIG_FULL,
  MOP_LIGHT_CONFIG_AMBIENT_ONLY,
  MOP_LIGHT_CONFIG_DARK,
  MOP_LIGHT_CONFIG_SINGLE_POINT,
  MOP_LIGHT_CONFIG_SINGLE_SPOT,
  MOP_LIGHT_CONFIG_COUNT,
} MopLightConfigId;

/* -------------------------------------------------------------------------
 * Camera state at a frame (conformance-local; distinct from the public
 * MopCameraState in <mop/query/camera_query.h> which also carries matrices)
 * ------------------------------------------------------------------------- */

typedef struct MopConfCameraState {
  MopVec3 eye;
  MopVec3 target;
  MopVec3 up;
  float fov_degrees;
  float near_plane;
  float far_plane;
} MopConfCameraState;

/* -------------------------------------------------------------------------
 * Runner configuration
 * ------------------------------------------------------------------------- */

typedef struct MopConfRunnerConfig {
  MopConfTier tier;
  MopBackendType backend;
  int width;
  int height;
  const char *golden_dir; /* path to golden baseline PNGs (NULL = skip) */
  const char *output_dir; /* path to write results (NULL = /tmp default) */
  bool reverse_z;         /* enable reversed-Z depth */
  bool verbose;           /* print per-frame info */
  bool collect_timing;    /* enable GPU timing collection */
} MopConfRunnerConfig;

/* -------------------------------------------------------------------------
 * Runner result
 * ------------------------------------------------------------------------- */

typedef struct MopConfRunnerResult {
  MopConfVerdict overall;
  uint32_t total_frames;
  uint32_t pass_frames;
  uint32_t warn_frames;
  uint32_t fail_frames;
  uint32_t nan_detected;
  uint32_t pick_invariant_failures;
  double mean_ssim;
  double min_ssim;
  double mean_rmse;
  double max_rmse;
  double rss_slope_kb_per_frame;
  double gpu_frame_ms_median;
  uint32_t validation_errors; /* Vulkan validation layer errors */
  uint32_t sync_hazards;      /* Vulkan synchronization hazards */
} MopConfRunnerResult;

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

/* Run the conformance suite. Returns overall verdict. */
MopConfRunnerResult mop_conformance_run(const MopConfRunnerConfig *config);

#endif /* MOP_CONFORMANCE_H */
