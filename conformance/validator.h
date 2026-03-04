/*
 * Master of Puppets — Conformance Framework
 * validator.h — Per-frame validation logic
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CONFORMANCE_VALIDATOR_H
#define MOP_CONFORMANCE_VALIDATOR_H

#include "conformance.h"
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Validation thresholds
 * ------------------------------------------------------------------------- */

typedef struct MopConfThresholds {
  double ssim_pass;
  double ssim_warn;
  double rmse_pass;
  double rmse_warn;
  double psnr_pass;
  double psnr_warn;
  double hist_chi2_pass;
  double hist_chi2_warn;
  double edge_f1_pass;
  double edge_f1_warn;
  double depth_rmse_pass;
  double depth_rmse_warn;
  double temporal_ssim_pass;
  double temporal_ssim_warn;
  double temporal_flicker_pass;
  double temporal_flicker_warn;
} MopConfThresholds;

/* Standard thresholds from conformance spec */
MopConfThresholds mop_conf_thresholds_standard(void);

/* Per-mode threshold variants (Section 11.3) */
MopConfThresholds mop_conf_thresholds_material_preview(void);
MopConfThresholds mop_conf_thresholds_wireframe(void);

/* -------------------------------------------------------------------------
 * Frame validation result
 * ------------------------------------------------------------------------- */

typedef struct MopConfFrameResult {
  uint32_t frame_index;
  MopConfVerdict verdict;
  MopConformanceMetrics metrics;
  bool has_nan;
  bool has_inf;
  uint32_t pick_invariant_fails;   /* P2-P7 failures */
  uint32_t pick_oracle_mismatches; /* P1 failures */
} MopConfFrameResult;

/* -------------------------------------------------------------------------
 * Geometric correctness tests (G1-G9)
 * ------------------------------------------------------------------------- */

typedef struct MopConfGeomResult {
  bool g1_projection_accuracy;
  bool g2_fov_correctness;
  bool g3_aspect_ratio;
  bool g4_reprojection;
  bool g5_depth_ordering;
  bool g6_hierarchy_occlusion;
  bool g7_trs_composition;
  bool g8_hierarchy_propagation;
  bool g9_negative_scale;
  uint32_t pass_count;
  uint32_t total_count;
} MopConfGeomResult;

/* -------------------------------------------------------------------------
 * Depth precision tests (D1-D6)
 * ------------------------------------------------------------------------- */

typedef struct MopConfDepthResult {
  bool d1_near_plane;
  bool d2_far_plane;
  bool d3_monotonic;
  bool d4_precision;
  bool d5_z_fighting;
  bool d6_far_field;
  double min_resolvable_eps[4]; /* at distances 1, 10, 100, 1000 */
} MopConfDepthResult;

/* -------------------------------------------------------------------------
 * Validation API
 * ------------------------------------------------------------------------- */

/* Validate a single frame against reference (golden baseline) */
MopConfFrameResult mop_conf_validate_frame(const uint8_t *rendered_rgba,
                                           const uint8_t *reference_rgba,
                                           const float *rendered_depth,
                                           const float *reference_depth, int w,
                                           int h,
                                           const MopConfThresholds *thresholds);

/* Classify metrics into PASS/WARN/FAIL */
MopConfVerdict mop_conf_classify_metrics(const MopConformanceMetrics *m,
                                         const MopConfThresholds *t);

/* Scan buffer for NaN/Inf */
bool mop_conf_scan_nan_inf(const float *buffer, int count);

/* Run geometric correctness tests against a viewport */
MopConfGeomResult mop_conf_test_geometry(MopViewport *viewport);

/* Run depth precision tests */
MopConfDepthResult mop_conf_test_depth(MopViewport *viewport);

/* Picking validation rules P2-P7 (invariants) */
uint32_t mop_conf_validate_picking_invariants(MopViewport *viewport, int w,
                                              int h);

/* -------------------------------------------------------------------------
 * Shadow validation tests (S1-S6)
 * ------------------------------------------------------------------------- */

typedef struct MopConfShadowResult {
  bool s1_shadow_direction;
  bool s2_shadow_boundary;
  bool s3_shadow_acne;
  bool s4_peter_panning;
  bool s5_cascade_banding;
  bool s6_shadow_stability;
  uint32_t pass_count;
  uint32_t total_count;
} MopConfShadowResult;

MopConfShadowResult mop_conf_test_shadows(MopViewport *viewport);

/* Picking oracle comparison (P1) — compares MOP picks against golden data.
 * Returns mismatch count. Skips when golden_dir is NULL. */
uint32_t mop_conf_validate_picking_oracle(MopViewport *viewport,
                                          const char *golden_dir,
                                          const char *path_name,
                                          uint32_t frame_index, int w, int h);

#endif /* MOP_CONFORMANCE_VALIDATOR_H */
