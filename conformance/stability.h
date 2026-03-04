/*
 * Master of Puppets — Conformance Framework
 * stability.h — Long-duration stability tests
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CONFORMANCE_STABILITY_H
#define MOP_CONFORMANCE_STABILITY_H

#include "conformance.h"
#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Memory leak detection result
 * ------------------------------------------------------------------------- */

typedef struct MopConfMemoryResult {
  double rss_samples_kb[11]; /* samples at frame 0, 1000, ..., 10000 */
  uint32_t sample_count;
  double slope_kb_per_frame; /* linear regression slope */
  bool passed;               /* slope < 1 KB/frame */
} MopConfMemoryResult;

/* -------------------------------------------------------------------------
 * Temporal drift detection result
 * ------------------------------------------------------------------------- */

typedef struct MopConfDriftResult {
  uint64_t hash_before;
  uint64_t hash_after;
  bool deterministic;               /* hashes match */
  double matrix_accumulation_error; /* Frobenius norm */
  bool matrix_passed;               /* error < 1e-3 */
} MopConfDriftResult;

/* -------------------------------------------------------------------------
 * Stability test API
 * ------------------------------------------------------------------------- */

/* Run memory leak detection over N frames */
MopConfMemoryResult mop_conf_test_memory(MopViewport *viewport,
                                         uint32_t total_frames);

/* Run determinism test: render frame 0 → N frames → render frame 0 again */
MopConfDriftResult mop_conf_test_determinism(MopViewport *viewport,
                                             uint32_t intervening_frames);

/* Run matrix accumulation drift test */
double mop_conf_test_matrix_drift(uint32_t iterations);

/* FNV-1a hash of RGBA buffer */
uint64_t mop_conf_hash_rgba(const uint8_t *rgba, int w, int h);

#endif /* MOP_CONFORMANCE_STABILITY_H */
