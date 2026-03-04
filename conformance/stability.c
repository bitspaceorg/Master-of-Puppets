/*
 * Master of Puppets — Conformance Framework
 * stability.c — Long-duration stability tests
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "stability.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <stdio.h>
#endif

/* -------------------------------------------------------------------------
 * Platform-specific RSS measurement
 * ------------------------------------------------------------------------- */

static double get_rss_kb(void) {
#if defined(__APPLE__)
  struct mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  kern_return_t kr = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                               (task_info_t)&info, &count);
  if (kr == KERN_SUCCESS)
    return (double)info.resident_size / 1024.0;
  return 0.0;
#elif defined(__linux__)
  FILE *f = fopen("/proc/self/status", "r");
  if (!f)
    return 0.0;
  char line[256];
  double rss = 0.0;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      long kb = 0;
      sscanf(line + 6, "%ld", &kb);
      rss = (double)kb;
      break;
    }
  }
  fclose(f);
  return rss;
#else
  return 0.0;
#endif
}

/* -------------------------------------------------------------------------
 * FNV-1a hash
 * ------------------------------------------------------------------------- */

uint64_t mop_conf_hash_rgba(const uint8_t *rgba, int w, int h) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  size_t n = (size_t)w * (size_t)h * 4;
  for (size_t i = 0; i < n; i++) {
    hash ^= (uint64_t)rgba[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

/* -------------------------------------------------------------------------
 * Memory leak detection
 * ------------------------------------------------------------------------- */

MopConfMemoryResult mop_conf_test_memory(MopViewport *viewport,
                                         uint32_t total_frames) {
  MopConfMemoryResult r;
  memset(&r, 0, sizeof(r));

  uint32_t sample_interval = total_frames / 10;
  if (sample_interval == 0)
    sample_interval = 1;

  r.rss_samples_kb[0] = get_rss_kb();
  r.sample_count = 1;

  for (uint32_t frame = 1; frame <= total_frames; frame++) {
    /* Render a frame */
    mop_viewport_render(viewport);

    if (frame % sample_interval == 0 && r.sample_count < 11) {
      r.rss_samples_kb[r.sample_count] = get_rss_kb();
      r.sample_count++;
    }
  }

  /* Linear regression: y = mx + b */
  if (r.sample_count >= 2) {
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    for (uint32_t i = 0; i < r.sample_count; i++) {
      double x = (double)(i * sample_interval);
      double y = r.rss_samples_kb[i];
      sum_x += x;
      sum_y += y;
      sum_xy += x * y;
      sum_x2 += x * x;
    }
    double n = (double)r.sample_count;
    double denom = n * sum_x2 - sum_x * sum_x;
    if (fabs(denom) > 1e-12) {
      r.slope_kb_per_frame = (n * sum_xy - sum_x * sum_y) / denom;
    }
  }

  r.passed = (r.slope_kb_per_frame < 1.0);
  return r;
}

/* -------------------------------------------------------------------------
 * Determinism test
 * ------------------------------------------------------------------------- */

MopConfDriftResult mop_conf_test_determinism(MopViewport *viewport,
                                             uint32_t intervening_frames) {
  MopConfDriftResult r;
  memset(&r, 0, sizeof(r));

  /* Render frame 0 and hash */
  mop_viewport_render(viewport);
  int w = 0, h = 0;
  const uint8_t *pixels = mop_viewport_read_color(viewport, &w, &h);
  if (pixels && w > 0 && h > 0) {
    r.hash_before = mop_conf_hash_rgba(pixels, w, h);
  }

  /* Render N intervening frames (animation would advance here) */
  for (uint32_t i = 0; i < intervening_frames; i++) {
    mop_viewport_render(viewport);
  }

  /* Re-render frame 0 (same state) and hash */
  mop_viewport_render(viewport);
  pixels = mop_viewport_read_color(viewport, &w, &h);
  if (pixels && w > 0 && h > 0) {
    r.hash_after = mop_conf_hash_rgba(pixels, w, h);
  }

  r.deterministic = (r.hash_before == r.hash_after);
  return r;
}

/* -------------------------------------------------------------------------
 * Matrix accumulation drift test
 * ------------------------------------------------------------------------- */

double mop_conf_test_matrix_drift(uint32_t iterations) {
  /* Compose N incremental rotation matrices */
  float angle_step = (2.0f * (float)M_PI) / (float)iterations;

  /* Accumulated: apply rotation step N times */
  MopMat4 accumulated = mop_mat4_identity();
  MopMat4 step = mop_mat4_rotate_y(angle_step);
  for (uint32_t i = 0; i < iterations; i++) {
    accumulated = mop_mat4_multiply(accumulated, step);
  }

  /* Direct: single rotation by total angle */
  MopMat4 direct = mop_mat4_rotate_y(angle_step * (float)iterations);

  /* Frobenius norm of difference */
  double sum = 0.0;
  for (int i = 0; i < 16; i++) {
    double diff = (double)(accumulated.d[i] - direct.d[i]);
    sum += diff * diff;
  }

  return sqrt(sum);
}
