/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MOP_CONFORMANCE_METRICS_H
#define MOP_CONFORMANCE_METRICS_H
#include <stdint.h>

/* All metrics operate on raw RGBA8 buffers (uint8_t *, w*h*4 bytes) */
double mop_metric_rmse(const uint8_t *a, const uint8_t *b, int w, int h);
double mop_metric_ssim(const uint8_t *a, const uint8_t *b, int w, int h);
double mop_metric_psnr(const uint8_t *a, const uint8_t *b, int w, int h);
double mop_metric_histogram_chi2(const uint8_t *a, const uint8_t *b, int w,
                                 int h);
double mop_metric_edge_f1(const uint8_t *a, const uint8_t *b, int w, int h);

/* Depth buffer metrics (float *, w*h floats) */
double mop_metric_depth_rmse(const float *a, const float *b, int w, int h);

/* Temporal stability: flicker = percentage of pixels changing > threshold
 * between frames. When obj_ids != NULL, skip pixels whose object_id ==
 * animated_id (expected to change). */
double mop_metric_temporal_flicker(const uint8_t *prev, const uint8_t *curr,
                                   const uint32_t *obj_ids, int w, int h,
                                   uint32_t animated_id, float threshold);

/* Normal buffer metrics (float *, w*h*3 floats — nx, ny, nz per pixel) */
double mop_metric_normal_cosine_err(const float *normals_a,
                                    const float *normals_b, int w, int h);

/* NaN/Inf scanning */
int mop_scan_nan_rgba(const uint8_t *rgba, int w,
                      int h); /* returns count of NaN pixels */
int mop_scan_nan_depth(const float *depth, int w,
                       int h); /* returns count of NaN/Inf */

#endif
