/*
 * Master of Puppets — Conformance Framework
 * report.c — JSON + text report generation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "report.h"

#include "stb_image_write.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * Helper: ensure directory exists
 * ------------------------------------------------------------------------- */

static void ensure_dir(const char *path) { mkdir(path, 0755); }

/* -------------------------------------------------------------------------
 * JSON report
 * ------------------------------------------------------------------------- */

int mop_conf_report_json(const char *output_dir,
                         const MopConfFrameResult *frames, uint32_t frame_count,
                         const MopConfRunnerResult *summary) {
  ensure_dir(output_dir);

  char path[1024];
  snprintf(path, sizeof(path), "%s/metrics.json", output_dir);
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;

  fprintf(f, "{\n");
  fprintf(f, "  \"verdict\": \"%s\",\n",
          summary->overall == MOP_CONF_PASS   ? "PASS"
          : summary->overall == MOP_CONF_WARN ? "WARN"
                                              : "FAIL");
  fprintf(f, "  \"total_frames\": %u,\n", summary->total_frames);
  fprintf(f, "  \"pass_frames\": %u,\n", summary->pass_frames);
  fprintf(f, "  \"warn_frames\": %u,\n", summary->warn_frames);
  fprintf(f, "  \"fail_frames\": %u,\n", summary->fail_frames);
  fprintf(f, "  \"mean_ssim\": %.6f,\n", summary->mean_ssim);
  fprintf(f, "  \"min_ssim\": %.6f,\n", summary->min_ssim);
  fprintf(f, "  \"mean_rmse\": %.4f,\n", summary->mean_rmse);
  fprintf(f, "  \"max_rmse\": %.4f,\n", summary->max_rmse);
  fprintf(f, "  \"nan_detected\": %u,\n", summary->nan_detected);
  fprintf(f, "  \"pick_invariant_failures\": %u,\n",
          summary->pick_invariant_failures);
  fprintf(f, "  \"rss_slope_kb_per_frame\": %.6f,\n",
          summary->rss_slope_kb_per_frame);
  fprintf(f, "  \"gpu_frame_ms_median\": %.3f,\n",
          summary->gpu_frame_ms_median);

  fprintf(f, "  \"frames\": [\n");
  for (uint32_t i = 0; i < frame_count; i++) {
    const MopConfFrameResult *fr = &frames[i];
    fprintf(f, "    {\n");
    fprintf(f, "      \"index\": %u,\n", fr->frame_index);
    fprintf(f, "      \"verdict\": \"%s\",\n",
            fr->verdict == MOP_CONF_PASS   ? "PASS"
            : fr->verdict == MOP_CONF_WARN ? "WARN"
                                           : "FAIL");
    fprintf(f, "      \"ssim\": %.6f,\n", fr->metrics.ssim);
    fprintf(f, "      \"rmse\": %.4f,\n", fr->metrics.rmse);
    fprintf(f, "      \"psnr\": %.2f,\n", fr->metrics.psnr);
    fprintf(f, "      \"histogram_chi2\": %.6f,\n", fr->metrics.histogram_chi2);
    fprintf(f, "      \"edge_f1\": %.6f,\n", fr->metrics.edge_f1);
    fprintf(f, "      \"depth_rmse\": %.6f,\n", fr->metrics.depth_rmse);
    fprintf(f, "      \"has_nan\": %s,\n", fr->has_nan ? "true" : "false");
    fprintf(f, "      \"has_inf\": %s\n", fr->has_inf ? "true" : "false");
    fprintf(f, "    }%s\n", (i + 1 < frame_count) ? "," : "");
  }
  fprintf(f, "  ]\n");
  fprintf(f, "}\n");

  fclose(f);
  return 0;
}

/* -------------------------------------------------------------------------
 * Summary text report
 * ------------------------------------------------------------------------- */

int mop_conf_report_summary(const char *output_dir,
                            const MopConfRunnerResult *result,
                            const MopConfGeomResult *geom,
                            const MopConfDepthResult *depth,
                            const MopConfShadowResult *shadow) {
  ensure_dir(output_dir);

  char path[1024];
  snprintf(path, sizeof(path), "%s/summary.txt", output_dir);
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;

  fprintf(f, "╔══════════════════════════════════════════════╗\n");
  fprintf(f, "║  MOP Conformance Report                     ║\n");
  fprintf(f, "╚══════════════════════════════════════════════╝\n\n");

  fprintf(f, "Overall: %s\n\n",
          result->overall == MOP_CONF_PASS   ? "PASS"
          : result->overall == MOP_CONF_WARN ? "WARN"
                                             : "FAIL");

  fprintf(f, "--- Frame Summary ---\n");
  fprintf(f, "  Total:  %u\n", result->total_frames);
  fprintf(f, "  Pass:   %u\n", result->pass_frames);
  fprintf(f, "  Warn:   %u\n", result->warn_frames);
  fprintf(f, "  Fail:   %u\n\n", result->fail_frames);

  fprintf(f, "--- Image Metrics ---\n");
  fprintf(f, "  Mean SSIM:  %.4f\n", result->mean_ssim);
  fprintf(f, "  Min SSIM:   %.4f\n", result->min_ssim);
  fprintf(f, "  Mean RMSE:  %.2f\n", result->mean_rmse);
  fprintf(f, "  Max RMSE:   %.2f\n\n", result->max_rmse);

  fprintf(f, "--- Stability ---\n");
  fprintf(f, "  NaN detected:       %u\n", result->nan_detected);
  fprintf(f, "  Pick invariant:     %u failures\n",
          result->pick_invariant_failures);
  fprintf(f, "  RSS slope:          %.4f KB/frame\n",
          result->rss_slope_kb_per_frame);
  fprintf(f, "  GPU median:         %.3f ms\n\n", result->gpu_frame_ms_median);

  if (geom) {
    fprintf(f, "--- Geometric Correctness ---\n");
    fprintf(f, "  G1 Projection:      %s\n",
            geom->g1_projection_accuracy ? "PASS" : "FAIL");
    fprintf(f, "  G2 FOV:             %s\n",
            geom->g2_fov_correctness ? "PASS" : "FAIL");
    fprintf(f, "  G3 Aspect:          %s\n",
            geom->g3_aspect_ratio ? "PASS" : "FAIL");
    fprintf(f, "  G4 Reprojection:    %s\n",
            geom->g4_reprojection ? "PASS" : "FAIL");
    fprintf(f, "  G5 Depth order:     %s\n",
            geom->g5_depth_ordering ? "PASS" : "FAIL");
    fprintf(f, "  G6 Hierarchy:       %s\n",
            geom->g6_hierarchy_occlusion ? "PASS" : "FAIL");
    fprintf(f, "  G7 TRS:             %s\n",
            geom->g7_trs_composition ? "PASS" : "FAIL");
    fprintf(f, "  G8 24-level:        %s\n",
            geom->g8_hierarchy_propagation ? "PASS" : "FAIL");
    fprintf(f, "  G9 Neg scale:       %s\n",
            geom->g9_negative_scale ? "PASS" : "FAIL");
    fprintf(f, "  Total:              %u / %u\n\n", geom->pass_count,
            geom->total_count);
  }

  if (depth) {
    fprintf(f, "--- Depth Precision ---\n");
    fprintf(f, "  D1 Near plane:      %s\n",
            depth->d1_near_plane ? "PASS" : "FAIL");
    fprintf(f, "  D2 Far plane:       %s\n",
            depth->d2_far_plane ? "PASS" : "FAIL");
    fprintf(f, "  D3 Monotonic:       %s\n",
            depth->d3_monotonic ? "PASS" : "FAIL");
    fprintf(f, "  D4 Precision:       %s\n",
            depth->d4_precision ? "PASS" : "FAIL");
    fprintf(f, "  D5 Z-fighting:      %s\n",
            depth->d5_z_fighting ? "PASS" : "FAIL");
    fprintf(f, "  D6 Far field:       %s\n\n",
            depth->d6_far_field ? "PASS" : "FAIL");
  }

  if (shadow) {
    fprintf(f, "--- Shadow Tests ---\n");
    fprintf(f, "  S1 Direction:       %s\n",
            shadow->s1_shadow_direction ? "PASS" : "FAIL");
    fprintf(f, "  S2 Boundary:        %s\n",
            shadow->s2_shadow_boundary ? "PASS" : "FAIL");
    fprintf(f, "  S3 Acne:            %s\n",
            shadow->s3_shadow_acne ? "PASS" : "FAIL");
    fprintf(f, "  S4 Peter panning:   %s\n",
            shadow->s4_peter_panning ? "PASS" : "FAIL");
    fprintf(f, "  S5 Cascade band:    %s\n",
            shadow->s5_cascade_banding ? "PASS" : "FAIL");
    fprintf(f, "  S6 Stability:       %s\n",
            shadow->s6_shadow_stability ? "PASS" : "FAIL");
    fprintf(f, "  Total:              %u / %u\n\n", shadow->pass_count,
            shadow->total_count);
  }

  fclose(f);
  return 0;
}

/* -------------------------------------------------------------------------
 * Diff image (absolute difference visualization, PNG output)
 * ------------------------------------------------------------------------- */

static uint8_t *rgba_to_rgb(const uint8_t *rgba, int w, int h) {
  uint8_t *rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
  if (!rgb)
    return NULL;
  for (int i = 0; i < w * h; i++) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
  }
  return rgb;
}

int mop_conf_report_diff_image(const char *output_dir, uint32_t frame_index,
                               const uint8_t *rendered,
                               const uint8_t *reference, int w, int h) {
  char failures_dir[512];
  snprintf(failures_dir, sizeof(failures_dir), "%s/failures", output_dir);
  ensure_dir(failures_dir);

  /* Build diff RGB buffer (amplified for visibility) */
  uint8_t *diff_rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
  if (!diff_rgb)
    return -1;

  for (int i = 0; i < w * h; i++) {
    int idx = i * 4;
    uint8_t dr =
        (uint8_t)(abs((int)rendered[idx + 0] - (int)reference[idx + 0]));
    uint8_t dg =
        (uint8_t)(abs((int)rendered[idx + 1] - (int)reference[idx + 1]));
    uint8_t db =
        (uint8_t)(abs((int)rendered[idx + 2] - (int)reference[idx + 2]));
    diff_rgb[i * 3 + 0] = (uint8_t)(dr < 128 ? dr * 2 : 255);
    diff_rgb[i * 3 + 1] = (uint8_t)(dg < 128 ? dg * 2 : 255);
    diff_rgb[i * 3 + 2] = (uint8_t)(db < 128 ? db * 2 : 255);
  }

  /* Write diff PNG */
  char path[1024];
  snprintf(path, sizeof(path), "%s/frame_%04u_diff.png", failures_dir,
           frame_index);
  stbi_write_png(path, w, h, 3, diff_rgb, w * 3);
  free(diff_rgb);

  /* Write rendered frame PNG */
  uint8_t *mop_rgb = rgba_to_rgb(rendered, w, h);
  if (mop_rgb) {
    snprintf(path, sizeof(path), "%s/frame_%04u_mop.png", failures_dir,
             frame_index);
    stbi_write_png(path, w, h, 3, mop_rgb, w * 3);
    free(mop_rgb);
  }

  /* Write reference frame PNG */
  uint8_t *ref_rgb = rgba_to_rgb(reference, w, h);
  if (ref_rgb) {
    snprintf(path, sizeof(path), "%s/frame_%04u_ref.png", failures_dir,
             frame_index);
    stbi_write_png(path, w, h, 3, ref_rgb, w * 3);
    free(ref_rgb);
  }

  return 0;
}

/* -------------------------------------------------------------------------
 * Helper: compare doubles for qsort (used by timings percentile)
 * ------------------------------------------------------------------------- */

static int cmp_double(const void *a, const void *b) {
  double da = *(const double *)a;
  double db = *(const double *)b;
  return (da > db) - (da < db);
}

/* -------------------------------------------------------------------------
 * Timings report (Gap 3)
 * ------------------------------------------------------------------------- */

int mop_conf_report_timings(const char *output_dir, const double *gpu_frame_ms,
                            const double *cpu_submit_ms, uint32_t frame_count) {
  if (!output_dir || frame_count == 0)
    return -1;

  ensure_dir(output_dir);

  /* Compute aggregates from GPU timings */
  double *sorted = (double *)malloc(frame_count * sizeof(double));
  if (!sorted)
    return -1;

  double gpu_sum = 0.0;
  for (uint32_t i = 0; i < frame_count; i++) {
    sorted[i] = gpu_frame_ms[i];
    gpu_sum += gpu_frame_ms[i];
  }
  qsort(sorted, frame_count, sizeof(double), cmp_double);

  double gpu_mean = gpu_sum / frame_count;
  double gpu_median = sorted[frame_count / 2];
  double gpu_p95 = sorted[(uint32_t)((frame_count - 1) * 0.95)];
  double gpu_p99 = sorted[(uint32_t)((frame_count - 1) * 0.99)];

  /* CPU aggregates */
  double cpu_sum = 0.0;
  double cpu_median = 0.0, cpu_mean = 0.0, cpu_p95 = 0.0, cpu_p99 = 0.0;
  if (cpu_submit_ms) {
    for (uint32_t i = 0; i < frame_count; i++) {
      sorted[i] = cpu_submit_ms[i];
      cpu_sum += cpu_submit_ms[i];
    }
    qsort(sorted, frame_count, sizeof(double), cmp_double);
    cpu_mean = cpu_sum / frame_count;
    cpu_median = sorted[frame_count / 2];
    cpu_p95 = sorted[(uint32_t)((frame_count - 1) * 0.95)];
    cpu_p99 = sorted[(uint32_t)((frame_count - 1) * 0.99)];
  }
  free(sorted);

  char path[1024];
  snprintf(path, sizeof(path), "%s/timings.json", output_dir);
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;

  fprintf(f, "{\n");
  fprintf(f, "  \"frame_count\": %u,\n", frame_count);
  fprintf(f, "  \"gpu_ms\": {\n");
  fprintf(f, "    \"mean\": %.4f,\n", gpu_mean);
  fprintf(f, "    \"median\": %.4f,\n", gpu_median);
  fprintf(f, "    \"p95\": %.4f,\n", gpu_p95);
  fprintf(f, "    \"p99\": %.4f,\n", gpu_p99);
  fprintf(f, "    \"per_frame\": [");
  for (uint32_t i = 0; i < frame_count; i++) {
    fprintf(f, "%.4f%s", gpu_frame_ms[i], (i + 1 < frame_count) ? "," : "");
  }
  fprintf(f, "]\n");
  fprintf(f, "  },\n");

  fprintf(f, "  \"cpu_ms\": {\n");
  fprintf(f, "    \"mean\": %.4f,\n", cpu_mean);
  fprintf(f, "    \"median\": %.4f,\n", cpu_median);
  fprintf(f, "    \"p95\": %.4f,\n", cpu_p95);
  fprintf(f, "    \"p99\": %.4f,\n", cpu_p99);
  fprintf(f, "    \"per_frame\": [");
  if (cpu_submit_ms) {
    for (uint32_t i = 0; i < frame_count; i++) {
      fprintf(f, "%.4f%s", cpu_submit_ms[i], (i + 1 < frame_count) ? "," : "");
    }
  }
  fprintf(f, "]\n");
  fprintf(f, "  }\n");
  fprintf(f, "}\n");

  fclose(f);
  return 0;
}

/* -------------------------------------------------------------------------
 * Memory report (Gap 4)
 * ------------------------------------------------------------------------- */

int mop_conf_report_memory(const char *output_dir, const double *rss_samples_kb,
                           uint32_t sample_count, double slope_kb_per_frame) {
  if (!output_dir || sample_count == 0)
    return -1;

  ensure_dir(output_dir);

  char path[1024];
  snprintf(path, sizeof(path), "%s/memory.json", output_dir);
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;

  bool passed = (slope_kb_per_frame < 1.0);

  fprintf(f, "{\n");
  fprintf(f, "  \"sample_count\": %u,\n", sample_count);
  fprintf(f, "  \"slope_kb_per_frame\": %.6f,\n", slope_kb_per_frame);
  fprintf(f, "  \"passed\": %s,\n", passed ? "true" : "false");
  fprintf(f, "  \"rss_samples_kb\": [");
  for (uint32_t i = 0; i < sample_count; i++) {
    fprintf(f, "%.2f%s", rss_samples_kb[i], (i + 1 < sample_count) ? "," : "");
  }
  fprintf(f, "]\n");
  fprintf(f, "}\n");

  fclose(f);
  return 0;
}

/* -------------------------------------------------------------------------
 * Validation log (Gap 5)
 * ------------------------------------------------------------------------- */

int mop_conf_report_validation_log(const char *output_dir,
                                   uint32_t validation_errors,
                                   uint32_t sync_hazards) {
  if (!output_dir)
    return -1;

  ensure_dir(output_dir);

  char path[1024];
  snprintf(path, sizeof(path), "%s/validation.log", output_dir);
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;

  fprintf(f, "MOP Conformance — Validation Layer Report\n");
  fprintf(f, "==========================================\n\n");
  fprintf(f, "Validation errors: %u\n", validation_errors);
  fprintf(f, "Sync hazards:      %u\n\n", sync_hazards);

  if (validation_errors == 0 && sync_hazards == 0) {
    fprintf(f, "Summary: CLEAN — no validation issues detected.\n");
  } else {
    fprintf(f, "Summary: %u issue(s) detected.\n",
            validation_errors + sync_hazards);
  }

  fclose(f);
  return 0;
}
