/*
 * Master of Puppets — Conformance Framework
 * report.h — JSON + text report generation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CONFORMANCE_REPORT_H
#define MOP_CONFORMANCE_REPORT_H

#include "conformance.h"
#include "validator.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Report generation
 * ------------------------------------------------------------------------- */

/* Write metrics.json — machine-readable, all metrics per frame */
int mop_conf_report_json(const char *output_dir,
                         const MopConfFrameResult *frames, uint32_t frame_count,
                         const MopConfRunnerResult *summary);

/* Write summary.txt — human-readable PASS/FAIL with details */
int mop_conf_report_summary(const char *output_dir,
                            const MopConfRunnerResult *result,
                            const MopConfGeomResult *geom,
                            const MopConfDepthResult *depth,
                            const MopConfShadowResult *shadow);

/* Write a diff image for a failed frame (rendered, reference, diff as PNG) */
int mop_conf_report_diff_image(const char *output_dir, uint32_t frame_index,
                               const uint8_t *rendered,
                               const uint8_t *reference, int w, int h);

/* Write timings.json — per-frame GPU/CPU timing with aggregates */
int mop_conf_report_timings(const char *output_dir, const double *gpu_frame_ms,
                            const double *cpu_submit_ms, uint32_t frame_count);

/* Write memory.json — RSS samples with slope and pass/fail */
int mop_conf_report_memory(const char *output_dir, const double *rss_samples_kb,
                           uint32_t sample_count, double slope_kb_per_frame);

/* Write validation.log — validation error/hazard counts */
int mop_conf_report_validation_log(const char *output_dir,
                                   uint32_t validation_errors,
                                   uint32_t sync_hazards);

#endif /* MOP_CONFORMANCE_REPORT_H */
