/*
 * Master of Puppets — Conformance Framework
 * perf_baseline.h — Performance regression detection
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CONF_PERF_BASELINE_H
#define MOP_CONF_PERF_BASELINE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct MopConfPerfBaseline {
  char gpu_name[128];
  char driver_version[64];
  double frame_ms_1m;  /* median frame time at 1M tris */
  double frame_ms_5m;  /* median frame time at 5M tris */
  double frame_ms_10m; /* median frame time at 10M tris */
} MopConfPerfBaseline;

/* Load baseline from JSON file. Returns false if file not found. */
bool mop_conf_perf_load_baseline(const char *path, MopConfPerfBaseline *out);

/* Save baseline to JSON file. */
bool mop_conf_perf_save_baseline(const char *path,
                                 const MopConfPerfBaseline *baseline);

/* Compare current timings against baseline. Returns true if within
 * regression threshold (e.g. threshold_ratio = 0.15 for 15%). */
bool mop_conf_perf_check_regression(const MopConfPerfBaseline *baseline,
                                    double current_1m, double current_5m,
                                    double current_10m, double threshold_ratio);

#endif /* MOP_CONF_PERF_BASELINE_H */
