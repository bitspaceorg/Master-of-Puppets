/* SPDX-License-Identifier: Apache-2.0 */
#include "metrics.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static inline double lum(uint8_t r, uint8_t g, uint8_t b) {
  return 0.299 * r + 0.587 * g + 0.114 * b;
}

/* ------------------------------------------------------------------ */
/*  1. RMSE  —  sqrt(mean((a-b)^2))  over RGB, ignore alpha           */
/* ------------------------------------------------------------------ */

double mop_metric_rmse(const uint8_t *a, const uint8_t *b, int w, int h) {
  int n = w * h;
  if (n == 0)
    return 0.0;

  double sum = 0.0;
  for (int i = 0; i < n; i++) {
    int idx = i * 4;
    double dr = (double)a[idx + 0] - (double)b[idx + 0];
    double dg = (double)a[idx + 1] - (double)b[idx + 1];
    double db = (double)a[idx + 2] - (double)b[idx + 2];
    sum += dr * dr + dg * dg + db * db;
  }
  return sqrt(sum / ((double)n * 3.0));
}

/* ------------------------------------------------------------------ */
/*  2. SSIM  —  Wang et al. 2004, Gaussian-weighted 11x11 window     */
/* ------------------------------------------------------------------ */

/* Pre-compute normalised 11x11 Gaussian kernel (sigma = 1.5) */
static void ssim_gaussian_kernel(double kernel[11][11]) {
  static const int WIN = 11;
  static const double SIGMA = 1.5;
  int half = WIN / 2;
  double sum = 0.0;
  for (int y = 0; y < WIN; y++) {
    for (int x = 0; x < WIN; x++) {
      double dx = (double)(x - half);
      double dy = (double)(y - half);
      double v = exp(-(dx * dx + dy * dy) / (2.0 * SIGMA * SIGMA));
      kernel[y][x] = v;
      sum += v;
    }
  }
  /* Normalise so weights sum to 1 */
  for (int y = 0; y < WIN; y++)
    for (int x = 0; x < WIN; x++)
      kernel[y][x] /= sum;
}

double mop_metric_ssim(const uint8_t *a, const uint8_t *b, int w, int h) {
  static const int WIN = 11;
  static const double C1 = (0.01 * 255.0) * (0.01 * 255.0); /* 6.5025  */
  static const double C2 = (0.03 * 255.0) * (0.03 * 255.0); /* 58.5225 */

  if (w < WIN || h < WIN)
    return 1.0;

  double kernel[11][11];
  ssim_gaussian_kernel(kernel);

  /* Pre-compute luminance planes */
  int n = w * h;
  double *la = (double *)malloc(sizeof(double) * (size_t)n);
  double *lb = (double *)malloc(sizeof(double) * (size_t)n);
  if (!la || !lb) {
    free(la);
    free(lb);
    return -1.0;
  }

  for (int i = 0; i < n; i++) {
    int idx = i * 4;
    la[i] = lum(a[idx], a[idx + 1], a[idx + 2]);
    lb[i] = lum(b[idx], b[idx + 1], b[idx + 2]);
  }

  double ssim_sum = 0.0;
  int win_count = 0;

  for (int y = 0; y <= h - WIN; y++) {
    for (int x = 0; x <= w - WIN; x++) {
      double mu_a = 0.0, mu_b = 0.0;
      double sum_a2 = 0.0, sum_b2 = 0.0;
      double sum_ab = 0.0;

      for (int wy = 0; wy < WIN; wy++) {
        int row = (y + wy) * w + x;
        for (int wx = 0; wx < WIN; wx++) {
          double g = kernel[wy][wx];
          double va = la[row + wx];
          double vb = lb[row + wx];
          mu_a += g * va;
          mu_b += g * vb;
          sum_a2 += g * va * va;
          sum_b2 += g * vb * vb;
          sum_ab += g * va * vb;
        }
      }

      double var_a = sum_a2 - mu_a * mu_a;
      double var_b = sum_b2 - mu_b * mu_b;
      double cov = sum_ab - mu_a * mu_b;

      double num = (2.0 * mu_a * mu_b + C1) * (2.0 * cov + C2);
      double den = (mu_a * mu_a + mu_b * mu_b + C1) * (var_a + var_b + C2);

      ssim_sum += num / den;
      win_count++;
    }
  }

  free(la);
  free(lb);
  return (win_count > 0) ? ssim_sum / win_count : 1.0;
}

/* ------------------------------------------------------------------ */
/*  3. PSNR  —  20*log10(255 / RMSE)                                  */
/* ------------------------------------------------------------------ */

double mop_metric_psnr(const uint8_t *a, const uint8_t *b, int w, int h) {
  double rmse = mop_metric_rmse(a, b, w, h);
  if (rmse == 0.0)
    return 100.0;
  return 20.0 * log10(255.0 / rmse);
}

/* ------------------------------------------------------------------ */
/*  4. Histogram Chi-squared                                          */
/* ------------------------------------------------------------------ */

double mop_metric_histogram_chi2(const uint8_t *a, const uint8_t *b, int w,
                                 int h) {
  int n = w * h;
  if (n == 0)
    return 0.0;

  double inv_n = 1.0 / (double)n;
  double total_chi2 = 0.0;

  /* Process R, G, B channels independently */
  for (int c = 0; c < 3; c++) {
    double ha[256];
    double hb[256];
    memset(ha, 0, sizeof(ha));
    memset(hb, 0, sizeof(hb));

    for (int i = 0; i < n; i++) {
      ha[a[i * 4 + c]] += 1.0;
      hb[b[i * 4 + c]] += 1.0;
    }

    /* Normalize to sum to 1 */
    for (int j = 0; j < 256; j++) {
      ha[j] *= inv_n;
      hb[j] *= inv_n;
    }

    /* Chi-squared distance */
    double chi2 = 0.0;
    for (int j = 0; j < 256; j++) {
      double denom = ha[j] + hb[j];
      if (denom > 0.0) {
        double diff = ha[j] - hb[j];
        chi2 += (diff * diff) / denom;
      }
    }
    total_chi2 += chi2;
  }

  return total_chi2 / 3.0;
}

/* ------------------------------------------------------------------ */
/*  5. Edge F1  —  Sobel + 95th percentile threshold + F1             */
/* ------------------------------------------------------------------ */

/* Compare function for qsort on doubles */
static int cmp_double(const void *pa, const void *pb) {
  double a = *(const double *)pa;
  double b = *(const double *)pb;
  if (a < b)
    return -1;
  if (a > b)
    return 1;
  return 0;
}

/*
 * Compute Sobel magnitude map.  Output is malloc'd, caller frees.
 * Border pixels get magnitude 0.
 */
static double *sobel_magnitude(const uint8_t *rgba, int w, int h) {
  int n = w * h;
  double *gray = (double *)malloc(sizeof(double) * (size_t)n);
  double *mag = (double *)calloc((size_t)n, sizeof(double));
  if (!gray || !mag) {
    free(gray);
    free(mag);
    return NULL;
  }

  for (int i = 0; i < n; i++) {
    int idx = i * 4;
    gray[i] = lum(rgba[idx], rgba[idx + 1], rgba[idx + 2]);
  }

  for (int y = 1; y < h - 1; y++) {
    for (int x = 1; x < w - 1; x++) {
      /* Sobel Gx kernel:  -1 0 +1
       *                   -2 0 +2
       *                   -1 0 +1  */
      double gx = -1.0 * gray[(y - 1) * w + (x - 1)] +
                  1.0 * gray[(y - 1) * w + (x + 1)] +
                  -2.0 * gray[(y)*w + (x - 1)] + 2.0 * gray[(y)*w + (x + 1)] +
                  -1.0 * gray[(y + 1) * w + (x - 1)] +
                  1.0 * gray[(y + 1) * w + (x + 1)];

      /* Sobel Gy kernel:  -1 -2 -1
       *                    0  0  0
       *                   +1 +2 +1  */
      double gy =
          -1.0 * gray[(y - 1) * w + (x - 1)] + -2.0 * gray[(y - 1) * w + (x)] +
          -1.0 * gray[(y - 1) * w + (x + 1)] +
          1.0 * gray[(y + 1) * w + (x - 1)] + 2.0 * gray[(y + 1) * w + (x)] +
          1.0 * gray[(y + 1) * w + (x + 1)];

      mag[y * w + x] = sqrt(gx * gx + gy * gy);
    }
  }

  free(gray);
  return mag;
}

/*
 * Find the value at the p-th percentile (0..1) of magnitude map.
 * Only considers interior pixels (border pixels are 0 and excluded).
 */
static double percentile(const double *mag, int w, int h, double p) {
  /* Collect interior magnitudes */
  int interior = (w - 2) * (h - 2);
  if (interior <= 0)
    return 0.0;

  double *buf = (double *)malloc(sizeof(double) * (size_t)interior);
  if (!buf)
    return 0.0;

  int k = 0;
  for (int y = 1; y < h - 1; y++)
    for (int x = 1; x < w - 1; x++)
      buf[k++] = mag[y * w + x];

  qsort(buf, (size_t)k, sizeof(double), cmp_double);

  int idx = (int)(p * (k - 1));
  if (idx < 0)
    idx = 0;
  if (idx >= k)
    idx = k - 1;
  double val = buf[idx];
  free(buf);
  return val;
}

double mop_metric_edge_f1(const uint8_t *a, const uint8_t *b, int w, int h) {
  if (w < 3 || h < 3)
    return 1.0;

  double *mag_a = sobel_magnitude(a, w, h);
  double *mag_b = sobel_magnitude(b, w, h);
  if (!mag_a || !mag_b) {
    free(mag_a);
    free(mag_b);
    return -1.0;
  }

  double thresh_a = percentile(mag_a, w, h, 0.95);
  double thresh_b = percentile(mag_b, w, h, 0.95);

  int n = w * h;
  int tp = 0, fp = 0, fn = 0;

  for (int i = 0; i < n; i++) {
    int ea = mag_a[i] >= thresh_a ? 1 : 0;
    int eb = mag_b[i] >= thresh_b ? 1 : 0;
    if (ea && eb)
      tp++;
    else if (eb && !ea)
      fp++;
    else if (ea && !eb)
      fn++;
  }

  free(mag_a);
  free(mag_b);

  int denom = 2 * tp + fp + fn;
  if (denom == 0)
    return 1.0; /* no edges at all — trivially matching */
  return (2.0 * tp) / (double)denom;
}

/* ------------------------------------------------------------------ */
/*  6. Depth RMSE  —  over valid (non-NaN) depth values               */
/* ------------------------------------------------------------------ */

double mop_metric_depth_rmse(const float *a, const float *b, int w, int h) {
  int n = w * h;
  double sum = 0.0;
  int valid = 0;

  for (int i = 0; i < n; i++) {
    if (isnan(a[i]) || isnan(b[i]) || isinf(a[i]) || isinf(b[i]))
      continue;
    double d = (double)a[i] - (double)b[i];
    sum += d * d;
    valid++;
  }

  if (valid == 0)
    return 0.0;
  return sqrt(sum / (double)valid);
}

/* ------------------------------------------------------------------ */
/*  7. Temporal flicker                                                */
/* ------------------------------------------------------------------ */

double mop_metric_temporal_flicker(const uint8_t *prev, const uint8_t *curr,
                                   const uint32_t *obj_ids, int w, int h,
                                   uint32_t animated_id, float threshold) {
  int n = w * h;
  if (n == 0)
    return 0.0;

  double thr = (double)threshold * 255.0;
  int count = 0;
  int checked = 0;

  for (int i = 0; i < n; i++) {
    /* Skip pixels belonging to the animated object */
    if (obj_ids != NULL && obj_ids[i] == animated_id)
      continue;

    int idx = i * 4;
    double lp = lum(prev[idx], prev[idx + 1], prev[idx + 2]);
    double lc = lum(curr[idx], curr[idx + 1], curr[idx + 2]);
    if (fabs(lp - lc) > thr)
      count++;
    checked++;
  }

  if (checked == 0)
    return 0.0;
  return 100.0 * (double)count / (double)checked;
}

/* ------------------------------------------------------------------ */
/*  8. Normal cosine error  —  mean angular error in degrees          */
/* ------------------------------------------------------------------ */

double mop_metric_normal_cosine_err(const float *normals_a,
                                    const float *normals_b, int w, int h) {
  static const double RAD_TO_DEG = 180.0 / 3.14159265358979323846;
  int n = w * h;
  double sum = 0.0;
  int valid = 0;

  for (int i = 0; i < n; i++) {
    int idx = i * 3;
    double ax = (double)normals_a[idx + 0];
    double ay = (double)normals_a[idx + 1];
    double az = (double)normals_a[idx + 2];
    double bx = (double)normals_b[idx + 0];
    double by = (double)normals_b[idx + 1];
    double bz = (double)normals_b[idx + 2];

    double len_a = sqrt(ax * ax + ay * ay + az * az);
    double len_b = sqrt(bx * bx + by * by + bz * bz);

    /* Skip if either normal has zero length */
    if (len_a < 1e-12 || len_b < 1e-12)
      continue;

    double dot = ax * bx + ay * by + az * bz;
    double cos_angle = dot / (len_a * len_b);

    /* Clamp to [-1, 1] to guard against floating-point overshoot */
    if (cos_angle > 1.0)
      cos_angle = 1.0;
    if (cos_angle < -1.0)
      cos_angle = -1.0;

    sum += acos(cos_angle) * RAD_TO_DEG;
    valid++;
  }

  if (valid == 0)
    return 0.0;
  return sum / (double)valid;
}

/* ------------------------------------------------------------------ */
/*  9. NaN / Inf scanning                                             */
/* ------------------------------------------------------------------ */

int mop_scan_nan_rgba(const uint8_t *rgba, int w, int h) {
  /*
   * RGBA8 is uint8_t [0..255] — NaN/Inf cannot exist in integer data.
   * Return 0 unconditionally.  The depth buffer scan (mop_scan_nan_depth)
   * handles float-based NaN/Inf detection for the depth attachment.
   */
  (void)rgba;
  (void)w;
  (void)h;
  return 0;
}

int mop_scan_nan_depth(const float *depth, int w, int h) {
  int n = w * h;
  int count = 0;

  for (int i = 0; i < n; i++) {
    if (isnan(depth[i]) || isinf(depth[i]))
      count++;
  }
  return count;
}
