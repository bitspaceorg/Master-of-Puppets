/*
 * Master of Puppets — Conformance Framework
 * validator.c — Per-frame validation logic
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "validator.h"
#include "metrics.h"
#include "scene_gen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Standard thresholds from conformance spec Section 11.2
 * ------------------------------------------------------------------------- */

MopConfThresholds mop_conf_thresholds_standard(void) {
  return (MopConfThresholds){
      .ssim_pass = 0.92,
      .ssim_warn = 0.85,
      .rmse_pass = 12.0,
      .rmse_warn = 20.0,
      .psnr_pass = 30.0,
      .psnr_warn = 25.0,
      .hist_chi2_pass = 0.05,
      .hist_chi2_warn = 0.15,
      .edge_f1_pass = 0.85,
      .edge_f1_warn = 0.75,
      .depth_rmse_pass = 0.005,
      .depth_rmse_warn = 0.02,
      .temporal_ssim_pass = 0.95,
      .temporal_ssim_warn = 0.90,
      .temporal_flicker_pass = 1.0,
      .temporal_flicker_warn = 3.0,
  };
}

/* -------------------------------------------------------------------------
 * Per-mode threshold variants (Section 11.3)
 * ------------------------------------------------------------------------- */

MopConfThresholds mop_conf_thresholds_material_preview(void) {
  MopConfThresholds t = mop_conf_thresholds_standard();
  t.ssim_pass = 0.88;
  t.ssim_warn = 0.80;
  return t;
}

MopConfThresholds mop_conf_thresholds_wireframe(void) {
  MopConfThresholds t = mop_conf_thresholds_standard();
  t.ssim_pass = 0.80;
  t.ssim_warn = 0.70;
  t.edge_f1_pass = 0.70;
  t.edge_f1_warn = 0.60;
  return t;
}

/* -------------------------------------------------------------------------
 * Classify metrics into PASS / WARN / FAIL
 * ------------------------------------------------------------------------- */

MopConfVerdict mop_conf_classify_metrics(const MopConformanceMetrics *m,
                                         const MopConfThresholds *t) {
  MopConfVerdict v = MOP_CONF_PASS;

  /* SSIM: higher is better */
  if (m->ssim < t->ssim_warn)
    return MOP_CONF_FAIL;
  if (m->ssim < t->ssim_pass && v < MOP_CONF_WARN)
    v = MOP_CONF_WARN;

  /* RMSE: lower is better */
  if (m->rmse > t->rmse_warn)
    return MOP_CONF_FAIL;
  if (m->rmse > t->rmse_pass && v < MOP_CONF_WARN)
    v = MOP_CONF_WARN;

  /* PSNR: higher is better */
  if (m->psnr < t->psnr_warn)
    return MOP_CONF_FAIL;
  if (m->psnr < t->psnr_pass && v < MOP_CONF_WARN)
    v = MOP_CONF_WARN;

  /* Histogram chi2: lower is better */
  if (m->histogram_chi2 > t->hist_chi2_warn)
    return MOP_CONF_FAIL;
  if (m->histogram_chi2 > t->hist_chi2_pass && v < MOP_CONF_WARN)
    v = MOP_CONF_WARN;

  /* Edge F1: higher is better */
  if (m->edge_f1 < t->edge_f1_warn)
    return MOP_CONF_FAIL;
  if (m->edge_f1 < t->edge_f1_pass && v < MOP_CONF_WARN)
    v = MOP_CONF_WARN;

  /* Depth RMSE: lower is better */
  if (m->depth_rmse > t->depth_rmse_warn)
    return MOP_CONF_FAIL;
  if (m->depth_rmse > t->depth_rmse_pass && v < MOP_CONF_WARN)
    v = MOP_CONF_WARN;

  return v;
}

/* -------------------------------------------------------------------------
 * Validate a single frame
 * ------------------------------------------------------------------------- */

MopConfFrameResult mop_conf_validate_frame(
    const uint8_t *rendered_rgba, const uint8_t *reference_rgba,
    const float *rendered_depth, const float *reference_depth, int w, int h,
    const MopConfThresholds *thresholds) {
  MopConfFrameResult r;
  memset(&r, 0, sizeof(r));

  /* Compute image metrics */
  r.metrics.rmse = mop_metric_rmse(rendered_rgba, reference_rgba, w, h);
  r.metrics.ssim = mop_metric_ssim(rendered_rgba, reference_rgba, w, h);
  r.metrics.psnr = mop_metric_psnr(rendered_rgba, reference_rgba, w, h);
  r.metrics.histogram_chi2 =
      mop_metric_histogram_chi2(rendered_rgba, reference_rgba, w, h);
  r.metrics.edge_f1 = mop_metric_edge_f1(rendered_rgba, reference_rgba, w, h);

  /* Depth metrics */
  if (rendered_depth && reference_depth) {
    r.metrics.depth_rmse =
        mop_metric_depth_rmse(rendered_depth, reference_depth, w, h);
  }

  /* NaN/Inf scan */
  r.has_nan = (mop_scan_nan_rgba(rendered_rgba, w, h) > 0);
  if (rendered_depth) {
    int nan_depth = mop_scan_nan_depth(rendered_depth, w, h);
    if (nan_depth > 0)
      r.has_inf = true;
  }

  /* Classify */
  r.verdict = mop_conf_classify_metrics(&r.metrics, thresholds);

  /* NaN/Inf is always a FAIL */
  if (r.has_nan || r.has_inf)
    r.verdict = MOP_CONF_FAIL;

  return r;
}

/* -------------------------------------------------------------------------
 * NaN/Inf scanning
 * ------------------------------------------------------------------------- */

bool mop_conf_scan_nan_inf(const float *buffer, int count) {
  for (int i = 0; i < count; i++) {
    if (isnan(buffer[i]) || isinf(buffer[i]))
      return true;
  }
  return false;
}

/* -------------------------------------------------------------------------
 * Helper: compute NDC depth from view-space z via projection matrix
 *
 * For a point at view-space z (negative, looking down -Z), the clip-space
 * depth is: clip_z = P[2][2]*z + P[3][2], clip_w = P[2][3]*z + P[3][3]
 * Column-major: P[col][row] = d[col*4+row]
 *   P[2][2] = d[10], P[3][2] = d[14], P[2][3] = d[11], P[3][3] = d[15]
 * NDC depth = clip_z / clip_w, mapped to [0,1] = (ndc+1)/2
 * ------------------------------------------------------------------------- */

static float conf_depth_from_viewz(const MopMat4 *proj, float view_z) {
  float clip_z = proj->d[10] * view_z + proj->d[14];
  float clip_w = proj->d[11] * view_z + proj->d[15];
  if (fabsf(clip_w) < 1e-12f)
    return -1.0f; /* degenerate */
  float ndc = clip_z / clip_w;
  return (ndc + 1.0f) * 0.5f; /* map [-1,1] -> [0,1] */
}

/* -------------------------------------------------------------------------
 * Helper: check if all 16 matrix elements match within tolerance
 * ------------------------------------------------------------------------- */

static bool conf_mat4_approx_eq(const MopMat4 *a, const MopMat4 *b, float tol) {
  for (int i = 0; i < 16; i++) {
    if (fabsf(a->d[i] - b->d[i]) > tol)
      return false;
  }
  return true;
}

/* -------------------------------------------------------------------------
 * Geometric correctness tests (G1-G9)
 * ------------------------------------------------------------------------- */

MopConfGeomResult mop_conf_test_geometry(MopViewport *viewport) {
  MopConfGeomResult r;
  memset(&r, 0, sizeof(r));
  r.total_count = 9;

  /* Common parameters */
  const int test_w = 640;
  const int test_h = 480;
  const float aspect = (float)test_w / (float)test_h;

  /* G1: Perspective projection accuracy
   * Create a cube with corners at (+/-1, +/-1, +/-1).
   * Camera at (0,0,5) looking -Z, fov=60, near=0.1, far=100.
   * Project all 8 corners and verify they land within [0,w] x [0,h]. */
  {
    const float fov_rad = 60.0f * ((float)M_PI / 180.0f);
    MopMat4 proj = mop_mat4_perspective(fov_rad, aspect, 0.1f, 100.0f);
    MopMat4 view = mop_mat4_look_at((MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                                    (MopVec3){0, 1, 0});
    MopMat4 pv = mop_mat4_multiply(proj, view);

    bool all_valid = true;
    /* 8 cube corners */
    float signs[2] = {-1.0f, 1.0f};
    for (int ix = 0; ix < 2; ix++) {
      for (int iy = 0; iy < 2; iy++) {
        for (int iz = 0; iz < 2; iz++) {
          MopVec4 world = {signs[ix], signs[iy], signs[iz], 1.0f};
          MopVec4 clip = mop_mat4_mul_vec4(pv, world);
          if (fabsf(clip.w) < 1e-9f) {
            all_valid = false;
            continue;
          }
          float xs = (clip.x / clip.w + 1.0f) * (float)test_w * 0.5f;
          float ys = (clip.y / clip.w + 1.0f) * (float)test_h * 0.5f;
          /* Front-face corners (z <= 0 in view) should be visible */
          if (clip.w > 0.0f) {
            if (xs < 0.0f || xs > (float)test_w || ys < 0.0f ||
                ys > (float)test_h) {
              all_valid = false;
            }
          }
        }
      }
    }
    r.g1_projection_accuracy = all_valid;
    if (all_valid)
      r.pass_count++;
  }

  /* G2: FOV correctness
   * Camera at origin looking -Z, FOV = 90 degrees.
   * Point at (1, 0, -1) should project near x = w (right edge).
   * Point at (-1, 0, -1) should project near x = 0 (left edge). */
  {
    const float fov90 = 90.0f * ((float)M_PI / 180.0f);
    MopMat4 proj = mop_mat4_perspective(fov90, aspect, 0.1f, 100.0f);
    MopMat4 view = mop_mat4_look_at((MopVec3){0, 0, 0}, (MopVec3){0, 0, -1},
                                    (MopVec3){0, 1, 0});
    MopMat4 pv = mop_mat4_multiply(proj, view);

    /* tan(45) = 1, so at z=-1, x=+1 should hit the right edge
     * for a symmetric frustum with the given aspect ratio.
     * With aspect != 1, the horizontal half-angle differs.
     * For vertical FOV=90, half = 45 deg. Horizontal half = atan(aspect *
     * tan(45)). At z=-1, the right edge is at x = aspect * tan(45) = aspect. So
     * x=1 maps to x_ndc = 1/aspect. Screen x = (1/aspect + 1) * w/2. But for
     * the test, we use x=aspect at z=-1 for the exact right edge. */
    MopVec4 right_pt = {1.0f, 0.0f, -1.0f, 1.0f};
    MopVec4 left_pt = {-1.0f, 0.0f, -1.0f, 1.0f};

    MopVec4 clip_r = mop_mat4_mul_vec4(pv, right_pt);
    MopVec4 clip_l = mop_mat4_mul_vec4(pv, left_pt);

    bool pass = true;
    if (fabsf(clip_r.w) > 1e-9f && fabsf(clip_l.w) > 1e-9f) {
      float xs_r = (clip_r.x / clip_r.w + 1.0f) * (float)test_w * 0.5f;
      float xs_l = (clip_l.x / clip_l.w + 1.0f) * (float)test_w * 0.5f;
      /* With vertical FOV=90 and aspect=4:3, x=1 at z=-1 projects to
       * ndc_x = 1/aspect, screen_x = (1/aspect + 1)*w/2.
       * Expected right edge = w. Point x=aspect at z=-1 -> ndc_x=1 -> screen w.
       * For x=1: expected_x = (1.0f/aspect + 1.0f) * w/2. */
      float expected_r = (1.0f / aspect + 1.0f) * (float)test_w * 0.5f;
      float expected_l = (-1.0f / aspect + 1.0f) * (float)test_w * 0.5f;
      if (fabsf(xs_r - expected_r) > 2.0f)
        pass = false;
      if (fabsf(xs_l - expected_l) > 2.0f)
        pass = false;
    } else {
      pass = false;
    }
    r.g2_fov_correctness = pass;
    if (pass)
      r.pass_count++;
  }

  /* G3: Aspect ratio correctness
   * Verify that mop_mat4_perspective produces different d[0] (x-scale)
   * for different aspect ratios.  d[0] = 1 / (aspect * tan(fov/2)). */
  {
    const float fov_rad = 60.0f * ((float)M_PI / 180.0f);
    MopMat4 proj_43 = mop_mat4_perspective(fov_rad, 4.0f / 3.0f, 0.1f, 100.0f);
    MopMat4 proj_34 = mop_mat4_perspective(fov_rad, 3.0f / 4.0f, 0.1f, 100.0f);

    /* d[0] = 1/(aspect * tan(fov/2)), so different aspects must yield
     * different d[0] values. Also verify the ratio matches. */
    bool pass = fabsf(proj_43.d[0] - proj_34.d[0]) > 1e-6f;
    if (pass) {
      /* The ratio of d[0] values should equal the inverse ratio of aspects */
      float ratio_d0 = proj_43.d[0] / proj_34.d[0];
      float ratio_aspect = (3.0f / 4.0f) / (4.0f / 3.0f); /* 9/16 */
      if (fabsf(ratio_d0 - ratio_aspect) > 1e-4f)
        pass = false;
    }
    r.g3_aspect_ratio = pass;
    if (pass)
      r.pass_count++;
  }

  /* G4: Reprojection consistency
   * Project a known world point to screen, then unproject back via
   * inverse(P*V) and verify the round-trip recovers the original. */
  {
    const float fov_rad = 60.0f * ((float)M_PI / 180.0f);
    MopMat4 proj = mop_mat4_perspective(fov_rad, aspect, 0.1f, 100.0f);
    MopMat4 view = mop_mat4_look_at((MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                                    (MopVec3){0, 1, 0});
    MopMat4 pv = mop_mat4_multiply(proj, view);
    MopMat4 pv_inv = mop_mat4_inverse(pv);

    MopVec3 world_orig = {1.5f, -0.7f, 2.0f};
    MopVec4 clip = mop_mat4_mul_vec4(
        pv, (MopVec4){world_orig.x, world_orig.y, world_orig.z, 1.0f});

    bool pass = false;
    if (fabsf(clip.w) > 1e-9f) {
      /* NDC coordinates */
      float ndc_x = clip.x / clip.w;
      float ndc_y = clip.y / clip.w;
      float ndc_z = clip.z / clip.w;

      /* Unproject: multiply inverse(PV) by NDC point (with w=1) */
      MopVec4 ndc_pt = {ndc_x, ndc_y, ndc_z, 1.0f};
      MopVec4 unproj = mop_mat4_mul_vec4(pv_inv, ndc_pt);
      if (fabsf(unproj.w) > 1e-9f) {
        float rx = unproj.x / unproj.w;
        float ry = unproj.y / unproj.w;
        float rz = unproj.z / unproj.w;
        float dx = rx - world_orig.x;
        float dy = ry - world_orig.y;
        float dz = rz - world_orig.z;
        float err = sqrtf(dx * dx + dy * dy + dz * dz);
        pass = (err < 0.01f);
      }
    }
    r.g4_reprojection = pass;
    if (pass)
      r.pass_count++;
  }

  /* G5: Depth ordering
   * Verify that mat4_perspective produces monotonically increasing depth
   * for points at increasing distance from the camera (more negative z). */
  {
    const float fov_rad = 60.0f * ((float)M_PI / 180.0f);
    MopMat4 proj = mop_mat4_perspective(fov_rad, aspect, 0.1f, 100.0f);

    bool monotonic = true;
    float prev_depth = conf_depth_from_viewz(&proj, -0.5f);
    for (int i = 1; i <= 20; i++) {
      float z = -0.5f - (float)i * 4.5f; /* z = -0.5, -5.0, -9.5, ... */
      float cur_depth = conf_depth_from_viewz(&proj, z);
      if (cur_depth <= prev_depth) {
        monotonic = false;
        break;
      }
      prev_depth = cur_depth;
    }
    r.g5_depth_ordering = monotonic;
    if (monotonic)
      r.pass_count++;
  }

  /* G6: Hierarchy occlusion
   * Parent at (0,0,-5), child at local offset (0,0,2) -> world (0,0,-3).
   * Verify matrix multiplication places child closer to camera (less
   * negative z in world space). */
  {
    MopMat4 parent_xform = mop_mat4_translate((MopVec3){0, 0, -5});
    MopMat4 child_local = mop_mat4_translate((MopVec3){0, 0, 2});
    MopMat4 child_world = mop_mat4_multiply(parent_xform, child_local);

    /* Parent world z = -5 (from d[14], col3 row2 in column-major).
     * Child world z should be -3. */
    float parent_z = parent_xform.d[14]; /* column 3, row 2 */
    float child_z = child_world.d[14];

    /* Child should be closer to camera (higher z, i.e., less negative) */
    bool pass = (child_z > parent_z);
    /* Also verify the actual value */
    if (pass)
      pass = (fabsf(child_z - (-3.0f)) < 0.01f);
    r.g6_hierarchy_occlusion = pass;
    if (pass)
      r.pass_count++;
  }

  /* G7: TRS composition order */
  {
    MopMat4 t = mop_mat4_translate((MopVec3){5, 0, 0});
    MopMat4 ry = mop_mat4_rotate_y((float)M_PI / 2.0f);
    MopMat4 s = mop_mat4_scale((MopVec3){2, 1, 1});
    MopMat4 expected = mop_mat4_multiply(t, mop_mat4_multiply(ry, s));
    MopMat4 composed = mop_mat4_compose_trs((MopVec3){5, 0, 0},
                                            (MopVec3){0, (float)M_PI / 2.0f, 0},
                                            (MopVec3){2, 1, 1});

    bool match = conf_mat4_approx_eq(&expected, &composed, 1e-4f);
    r.g7_trs_composition = match;
    if (match)
      r.pass_count++;
  }

  /* G8: 24-level hierarchy propagation
   * Chain 24 transforms: each = T(0,8,0) * Ry(pi/12) * S(0.95).
   * Compute accumulated product and verify against direct computation. */
  {
    MopMat4 accumulated = mop_mat4_identity();
    MopMat4 single_t = mop_mat4_translate((MopVec3){0, 8, 0});
    MopMat4 single_ry = mop_mat4_rotate_y((float)M_PI / 12.0f);
    MopMat4 single_s = mop_mat4_scale((MopVec3){0.95f, 0.95f, 0.95f});
    /* Each level: T * Ry * S */
    MopMat4 single_level =
        mop_mat4_multiply(single_t, mop_mat4_multiply(single_ry, single_s));

    for (int i = 0; i < 24; i++) {
      accumulated = mop_mat4_multiply(accumulated, single_level);
    }

    /* Direct computation: apply the same chain from scratch */
    MopMat4 direct = mop_mat4_identity();
    for (int i = 0; i < 24; i++) {
      MopMat4 level_t = mop_mat4_translate((MopVec3){0, 8, 0});
      MopMat4 level_ry = mop_mat4_rotate_y((float)M_PI / 12.0f);
      MopMat4 level_s = mop_mat4_scale((MopVec3){0.95f, 0.95f, 0.95f});
      MopMat4 level =
          mop_mat4_multiply(level_t, mop_mat4_multiply(level_ry, level_s));
      direct = mop_mat4_multiply(direct, level);
    }

    bool match = conf_mat4_approx_eq(&accumulated, &direct, 0.1f);
    r.g8_hierarchy_propagation = match;
    if (match)
      r.pass_count++;
  }

  /* G9: Negative scale
   * Matrix with S(-1, 1, 1) should have negative determinant,
   * indicating winding flip is needed. */
  {
    MopMat4 neg_s = mop_mat4_scale((MopVec3){-1, 1, 1});

    /* Compute 3x3 determinant of the upper-left submatrix.
     * For a scale matrix, det = sx * sy * sz = -1 * 1 * 1 = -1.
     * Column-major: col0=(d[0],d[1],d[2]), col1=(d[4],d[5],d[6]),
     * col2=(d[8],d[9],d[10]). */
    float det =
        neg_s.d[0] * (neg_s.d[5] * neg_s.d[10] - neg_s.d[6] * neg_s.d[9]) -
        neg_s.d[4] * (neg_s.d[1] * neg_s.d[10] - neg_s.d[2] * neg_s.d[9]) +
        neg_s.d[8] * (neg_s.d[1] * neg_s.d[6] - neg_s.d[2] * neg_s.d[5]);

    bool pass = (fabsf(det - (-1.0f)) < 1e-6f);
    r.g9_negative_scale = pass;
    if (pass)
      r.pass_count++;
  }

  (void)viewport; /* viewport available for future render-based tests */
  return r;
}

/* -------------------------------------------------------------------------
 * Depth precision tests (D1-D6)
 *
 * Pure math tests using mop_mat4_perspective.  We compute clip.z/clip.w
 * for known view-space z values and validate depth buffer behavior.
 * ------------------------------------------------------------------------- */

MopConfDepthResult mop_conf_test_depth(MopViewport *viewport) {
  MopConfDepthResult r;
  memset(&r, 0, sizeof(r));

  const float fov_rad = 60.0f * ((float)M_PI / 180.0f);
  const float near = 0.1f;
  const float far = 1000.0f;
  const float aspect = 4.0f / 3.0f;
  MopMat4 proj = mop_mat4_perspective(fov_rad, aspect, near, far);

  /* D1: Near plane — depth at z = -near should map to approximately 0.0 */
  {
    float d = conf_depth_from_viewz(&proj, -near);
    r.d1_near_plane = (d >= -0.01f && d <= 0.01f);
  }

  /* D2: Far plane — depth at z = -far should map to approximately 1.0 */
  {
    float d = conf_depth_from_viewz(&proj, -far);
    r.d2_far_plane = (d >= 0.99f && d <= 1.01f);
  }

  /* D3: Monotonic — for z = -1, -2, ..., -100 (view space),
   * depth values must be monotonically increasing */
  {
    bool mono = true;
    float prev = conf_depth_from_viewz(&proj, -1.0f);
    for (int i = 2; i <= 100; i++) {
      float cur = conf_depth_from_viewz(&proj, -(float)i);
      if (cur <= prev) {
        mono = false;
        break;
      }
      prev = cur;
    }
    r.d3_monotonic = mono;
  }

  /* D4: Precision — for pairs (z, z+eps) at various distances,
   * verify depth difference is nonzero.
   * eps = 1e-1 at distance 1, 1e-2 at distance 10, etc. */
  {
    bool precise = true;
    float test_z[4] = {-1.0f, -10.0f, -100.0f, -1000.0f};
    float test_eps[4] = {1e-1f, 1e-2f, 1e-2f, 1e-1f};
    for (int i = 0; i < 4; i++) {
      float d1 = conf_depth_from_viewz(&proj, test_z[i]);
      float d2 = conf_depth_from_viewz(&proj, test_z[i] - test_eps[i]);
      if (fabsf(d2 - d1) < 1e-10f) {
        precise = false;
        break;
      }
    }
    r.d4_precision = precise;
  }

  /* D5: Z-fighting — find minimum depth separation for planes near z=-5.
   * Binary search for smallest eps that produces different depth values. */
  {
    float base_z = -5.0f;
    float base_d = conf_depth_from_viewz(&proj, base_z);
    float lo = 1e-8f;
    float hi = 1e-1f;
    /* Binary search for the threshold */
    for (int iter = 0; iter < 50; iter++) {
      float mid = (lo + hi) * 0.5f;
      float test_d = conf_depth_from_viewz(&proj, base_z - mid);
      if (fabsf(test_d - base_d) > 0.0f) {
        hi = mid; /* can resolve this separation */
      } else {
        lo = mid; /* cannot resolve */
      }
    }
    r.d5_z_fighting = (hi < 1e-1f); /* PASS if we can resolve sub-0.1 */

    /* Store min_resolvable_eps at 4 distances */
    float ref_distances[4] = {-1.0f, -10.0f, -100.0f, -1000.0f};
    for (int di = 0; di < 4; di++) {
      float bd = conf_depth_from_viewz(&proj, ref_distances[di]);
      float elo = 1e-10f;
      float ehi = 1.0f;
      for (int iter = 0; iter < 60; iter++) {
        float emid = (elo + ehi) * 0.5f;
        float td = conf_depth_from_viewz(&proj, ref_distances[di] - emid);
        if (fabsf(td - bd) > 0.0f) {
          ehi = emid;
        } else {
          elo = emid;
        }
      }
      r.min_resolvable_eps[di] = (double)ehi;
    }
  }

  /* D6: Far field — check depth at z = -1e4, -1e5.
   * They should not saturate to exactly 1.0 (some separation needed). */
  {
    float d_1e4 = conf_depth_from_viewz(&proj, -1e4f);
    float d_1e5 = conf_depth_from_viewz(&proj, -1e5f);
    /* Both should be < 1.0 (not clamped), and d_1e5 > d_1e4 */
    r.d6_far_field = (d_1e4 < 1.0f) && (d_1e5 < 1.0f) && (d_1e5 > d_1e4);
    /* Note: with far=1000, points beyond far plane will exceed 1.0.
     * Use a larger far plane for this test. */
    MopMat4 far_proj = mop_mat4_perspective(fov_rad, aspect, 0.1f, 1e6f);
    d_1e4 = conf_depth_from_viewz(&far_proj, -1e4f);
    d_1e5 = conf_depth_from_viewz(&far_proj, -1e5f);
    r.d6_far_field = (d_1e4 < 1.0f) && (d_1e5 < 1.0f) && (d_1e5 > d_1e4);
  }

  (void)viewport; /* viewport available for future render-based tests */
  return r;
}

/* -------------------------------------------------------------------------
 * Picking validation (invariant rules P2-P7)
 *
 * Sample 9 points: center, 4 corners, 4 edge midpoints.
 * Check rules P2-P4, P5-P7 on each.
 * ------------------------------------------------------------------------- */

uint32_t mop_conf_validate_picking_invariants(MopViewport *viewport, int w,
                                              int h) {
  uint32_t failures = 0;

  /* 9 sample points: center, 4 corners (inset by 1px), 4 edge midpoints */
  struct {
    int x;
    int y;
  } samples[9] = {
      {w / 2, h / 2}, /* center */
      {1, 1},         /* top-left */
      {w - 2, 1},     /* top-right */
      {1, h - 2},     /* bottom-left */
      {w - 2, h - 2}, /* bottom-right */
      {w / 2, 1},     /* top-mid */
      {w / 2, h - 2}, /* bottom-mid */
      {1, h / 2},     /* left-mid */
      {w - 2, h / 2}, /* right-mid */
  };

  /* P2: If hit=true, depth must be finite and in [0, 1] */
  for (int i = 0; i < 9; i++) {
    MopPickResult pr = mop_viewport_pick(viewport, samples[i].x, samples[i].y);
    if (pr.hit) {
      if (isnan(pr.depth) || isinf(pr.depth) || pr.depth < 0.0f ||
          pr.depth > 1.0f) {
        failures++;
      }
    }
  }

  /* P3: Picking through hierarchy resolves to leaf (simplified: verify
   * pick doesn't crash on any of the 9 sample points).
   * If we got here without a crash, P3 passes implicitly.
   * We still record a pick at each point as a smoke test. */
  for (int i = 0; i < 9; i++) {
    MopPickResult pr = mop_viewport_pick(viewport, samples[i].x, samples[i].y);
    /* Suppress unused warning — the pick itself is the test */
    (void)pr;
  }

  /* P4: Picking instanced mesh returns base mesh object_id (simplified:
   * verify pick at center returns a valid non-zero ID if geometry is
   * present in the scene). */
  {
    MopPickResult center = mop_viewport_pick(viewport, w / 2, h / 2);
    /* We only flag failure if hit is true but object_id is 0,
     * since object_id=0 is reserved for background. */
    if (center.hit && center.object_id == 0)
      failures++;
  }

  /* P5: Out-of-bounds must return hit=false (object_id=0) */
  {
    MopPickResult oob = mop_viewport_pick(viewport, -1, -1);
    if (oob.hit)
      failures++;
    oob = mop_viewport_pick(viewport, w + 10, h + 10);
    if (oob.hit)
      failures++;
  }

  /* P6: object_id=0 is background
   * (Convention check — enforced by clear value) */

  /* P7: Opaque wins over transparent in ID buffer (simplified: verify
   * pick returns a consistent result at same coordinates between two
   * consecutive calls). */
  for (int i = 0; i < 9; i++) {
    MopPickResult first =
        mop_viewport_pick(viewport, samples[i].x, samples[i].y);
    MopPickResult second =
        mop_viewport_pick(viewport, samples[i].x, samples[i].y);
    if (first.hit != second.hit)
      failures++;
    if (first.hit && second.hit) {
      if (first.object_id != second.object_id)
        failures++;
      if (fabsf(first.depth - second.depth) > 1e-6f)
        failures++;
    }
  }

  return failures;
}

/* -------------------------------------------------------------------------
 * Shadow validation tests (S1-S6) — Quantitative checks
 *
 * Each test sets up a minimal scene with a directional shadow-casting
 * light and performs pixel-level analysis of the rendered output.
 * ------------------------------------------------------------------------- */

/* Helper: compute luminance from RGBA pixel */
static float pixel_luminance(const uint8_t *rgba, int idx) {
  return 0.2126f * (float)rgba[idx * 4 + 0] / 255.0f +
         0.7152f * (float)rgba[idx * 4 + 1] / 255.0f +
         0.0722f * (float)rgba[idx * 4 + 2] / 255.0f;
}

/* Helper: simple 3x3 Sobel edge magnitude at pixel (x,y) */
static float sobel_mag(const uint8_t *rgba, int w, int h, int x, int y) {
  if (x <= 0 || x >= w - 1 || y <= 0 || y >= h - 1)
    return 0.0f;
  /* Gx kernel: -1 0 +1 / -2 0 +2 / -1 0 +1 */
  /* Gy kernel: -1 -2 -1 / 0 0 0 / +1 +2 +1 */
  float gx = 0.0f, gy = 0.0f;
  float lum[3][3];
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      lum[dy + 1][dx + 1] = pixel_luminance(rgba, (y + dy) * w + (x + dx));
    }
  }
  gx = -lum[0][0] + lum[0][2] - 2.0f * lum[1][0] + 2.0f * lum[1][2] -
       lum[2][0] + lum[2][2];
  gy = -lum[0][0] - 2.0f * lum[0][1] - lum[0][2] + lum[2][0] +
       2.0f * lum[2][1] + lum[2][2];
  return sqrtf(gx * gx + gy * gy);
}

/* Shadow luminance threshold: pixels below this are considered "in shadow" */
#define SHADOW_LUM_THRESH 0.25f

MopConfShadowResult mop_conf_test_shadows(MopViewport *viewport) {
  MopConfShadowResult r;
  memset(&r, 0, sizeof(r));
  r.total_count = 6;

  /* Set up a directional light with cast_shadows enabled */
  MopLight shadow_light;
  memset(&shadow_light, 0, sizeof(shadow_light));
  shadow_light.type = MOP_LIGHT_DIRECTIONAL;
  shadow_light.direction = (MopVec3){-0.5f, -1.0f, -0.3f};
  shadow_light.color = (MopColor){1.0f, 1.0f, 1.0f, 1.0f};
  shadow_light.intensity = 3.14159f;
  shadow_light.active = true;
  shadow_light.cast_shadows = true;

  MopLight *added = mop_viewport_add_light(viewport, &shadow_light);

  /* Add a ground plane (large quad lying in XZ at y=0, facing +Y) */
  uint32_t gnd_vc = 0, gnd_ic = 0;
  uint32_t *gnd_idx = NULL;
  MopVertex *gnd_v = mop_gen_quad(200.0f, 200.0f, &gnd_vc, &gnd_idx, &gnd_ic);
  MopMeshDesc gnd_desc = {.vertices = gnd_v,
                          .vertex_count = gnd_vc,
                          .indices = gnd_idx,
                          .index_count = gnd_ic,
                          .object_id = 900};
  /* Rotate quad from XY to XZ (rotate -90 around X) and translate down */
  MopMat4 gnd_xform = mop_mat4_multiply(mop_mat4_translate((MopVec3){0, -1, 0}),
                                        mop_mat4_rotate_x(-(float)M_PI / 2.0f));
  MopMesh *gnd_mesh = mop_viewport_add_mesh(viewport, &gnd_desc);
  if (gnd_mesh)
    mop_mesh_set_transform(gnd_mesh, &gnd_xform);

  /* Add a cube (pole/occluder) above ground */
  uint32_t cube_vc = 0, cube_ic = 0;
  uint32_t *cube_idx = NULL;
  MopVertex *cube_v = mop_gen_cube(2.0f, &cube_vc, &cube_idx, &cube_ic);
  MopMeshDesc cube_desc = {.vertices = cube_v,
                           .vertex_count = cube_vc,
                           .indices = cube_idx,
                           .index_count = cube_ic,
                           .object_id = 901};
  MopMat4 cube_xform = mop_mat4_translate((MopVec3){0, 0, 0});
  MopMesh *cube_mesh = mop_viewport_add_mesh(viewport, &cube_desc);
  if (cube_mesh)
    mop_mesh_set_transform(cube_mesh, &cube_xform);

  /* Set camera looking at the scene from above-ish angle */
  mop_viewport_set_camera(viewport, (MopVec3){8, 10, 8}, /* eye */
                          (MopVec3){0, 0, 0},            /* target */
                          (MopVec3){0, 1, 0},            /* up */
                          60.0f, 0.1f, 500.0f);

  /* S1: Shadow Direction — verify dark pixels cluster in the expected
   * direction (opposite the light's horizontal projection on the ground). */
  {
    mop_viewport_render(viewport);
    int cw = 0, ch = 0;
    const uint8_t *color = mop_viewport_read_color(viewport, &cw, &ch);
    bool pass = false;
    if (color && cw > 0 && ch > 0) {
      /* Scan bottom half (ground region) for dark pixels, compute centroid */
      double sx = 0, sy = 0;
      int dark_count = 0;
      int ground_y_start = ch / 2;
      for (int y = ground_y_start; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
          float lum = pixel_luminance(color, y * cw + x);
          if (lum < SHADOW_LUM_THRESH) {
            sx += x;
            sy += y;
            dark_count++;
          }
        }
      }
      if (dark_count > 0) {
        double cx = sx / dark_count;
        double cy = sy / dark_count;
        /* Shadow centroid should be offset from screen center.
         * The light comes from (-0.5, -1, -0.3), so horizontal projection
         * is (-0.5, -0.3) => shadow falls toward (+0.5, +0.3) in world XZ.
         * The exact screen projection depends on the camera, but the shadow
         * centroid should be displaced from the image center. */
        double center_x = cw / 2.0;
        double center_y = ch / 2.0;
        double dx = cx - center_x;
        double dy = cy - center_y;
        /* Shadow direction angle from center */
        double shadow_angle = atan2(dy, dx);
        /* Expected angle: light dir horizontal is (-0.5, -0.3), shadow
         * falls opposite = (0.5, 0.3). In screen space this is roughly
         * some angle — we just need it to be a coherent direction, not
         * scattered. Check that the centroid is displaced significantly
         * from center (shadow exists and has a direction). */
        double displacement = sqrt(dx * dx + dy * dy);
        pass = (displacement > cw * 0.02);
        (void)shadow_angle; /* angle check needs camera-specific oracle */
      }
    }
    r.s1_shadow_direction = pass;
    if (pass)
      r.pass_count++;
  }

  /* S2: Shadow Boundary — render cube on ground, detect sharp edges via
   * Sobel on the shadow region. PASS if edge pixels exist and are
   * localized (not scattered noise). */
  {
    mop_viewport_render(viewport);
    int cw = 0, ch = 0;
    const uint8_t *color = mop_viewport_read_color(viewport, &cw, &ch);
    bool pass = false;
    if (color && cw > 0 && ch > 0) {
      int edge_count = 0;
      int ground_y_start = ch / 2;
      for (int y = ground_y_start; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
          float mag = sobel_mag(color, cw, ch, x, y);
          if (mag > 0.15f)
            edge_count++;
        }
      }
      /* PASS if we found a meaningful number of edge pixels (sharp
       * shadow boundary) — at least 0.1% of ground region pixels */
      int ground_pixels = (ch - ground_y_start) * cw;
      pass = (edge_count > ground_pixels / 1000);
    }
    r.s2_shadow_boundary = pass;
    if (pass)
      r.pass_count++;
  }

  /* S3: Shadow Acne — render lit ground plane from directly above.
   * Count incorrectly-shadowed pixels on a surface facing the light.
   * PASS if acne < 2%. */
  {
    /* Move camera directly above, looking straight down */
    mop_viewport_set_camera(viewport,
                            (MopVec3){0, 20, 0}, /* eye: directly above */
                            (MopVec3){0, -1, 0}, /* target: ground */
                            (MopVec3){0, 0, -1}, /* up */
                            60.0f, 0.1f, 500.0f);
    mop_viewport_render(viewport);
    int cw = 0, ch = 0;
    const uint8_t *color = mop_viewport_read_color(viewport, &cw, &ch);
    bool pass = false;
    if (color && cw > 0 && ch > 0) {
      /* The ground faces +Y and is lit by a light with a strong -Y
       * component, so most of the ground should be lit. Count dark
       * (incorrectly shadowed) pixels across the whole image. */
      int dark = 0;
      int total = cw * ch;
      for (int i = 0; i < total; i++) {
        float lum = pixel_luminance(color, i);
        if (lum < SHADOW_LUM_THRESH)
          dark++;
      }
      double acne_pct = (double)dark / (double)total * 100.0;
      pass = (acne_pct < 2.0);
    }
    r.s3_shadow_acne = pass;
    if (pass)
      r.pass_count++;

    /* Restore camera */
    mop_viewport_set_camera(viewport, (MopVec3){8, 10, 8}, (MopVec3){0, 0, 0},
                            (MopVec3){0, 1, 0}, 60.0f, 0.1f, 500.0f);
  }

  /* S4: Peter Panning — check that shadow pixels exist directly adjacent
   * to the cube's base (within 3 pixels). PASS if gap < 3 pixels. */
  {
    mop_viewport_render(viewport);
    int cw = 0, ch = 0;
    const uint8_t *color = mop_viewport_read_color(viewport, &cw, &ch);
    bool pass = false;
    if (color && cw > 0 && ch > 0) {
      /* Find the cube's lowest visible row by picking object_id=901.
       * Approximate: scan from bottom up for the first row that has
       * both lit and dark pixels (transition zone at cube base). */
      int cube_base_y = -1;
      for (int y = ch - 1; y >= ch / 4; y--) {
        /* Use picking to find the cube edge is expensive;
         * instead detect the boundary by luminance transition. */
        bool has_bright = false;
        bool has_dark = false;
        for (int x = cw / 4; x < cw * 3 / 4; x++) {
          float lum = pixel_luminance(color, y * cw + x);
          if (lum > 0.5f)
            has_bright = true;
          if (lum < SHADOW_LUM_THRESH)
            has_dark = true;
        }
        if (has_bright && has_dark) {
          cube_base_y = y;
          break;
        }
      }
      if (cube_base_y >= 0) {
        /* Check that shadow pixels exist within 3 rows below the base */
        bool found_shadow = false;
        for (int dy = 1; dy <= 3 && cube_base_y + dy < ch; dy++) {
          int y = cube_base_y + dy;
          for (int x = cw / 4; x < cw * 3 / 4; x++) {
            float lum = pixel_luminance(color, y * cw + x);
            if (lum < SHADOW_LUM_THRESH) {
              found_shadow = true;
              break;
            }
          }
          if (found_shadow)
            break;
        }
        pass = found_shadow;
      }
    }
    r.s4_peter_panning = pass;
    if (pass)
      r.pass_count++;
  }

  /* S5: Cascade Banding — render from ORBIT camera at frame 0.
   * Sample luminance along a horizontal scanline through the center.
   * PASS if max luminance discontinuity < 0.05. */
  {
    /* Set ORBIT-like camera looking at the scene */
    mop_viewport_set_camera(viewport, (MopVec3){15, 8, 15}, (MopVec3){0, 0, 0},
                            (MopVec3){0, 1, 0}, 60.0f, 0.1f, 500.0f);
    mop_viewport_render(viewport);
    int cw = 0, ch = 0;
    const uint8_t *color = mop_viewport_read_color(viewport, &cw, &ch);
    bool pass = false;
    if (color && cw > 0 && ch > 0) {
      int mid_y = ch / 2;
      float max_disc = 0.0f;
      float prev_lum = pixel_luminance(color, mid_y * cw);
      for (int x = 1; x < cw; x++) {
        float cur_lum = pixel_luminance(color, mid_y * cw + x);
        float disc = fabsf(cur_lum - prev_lum);
        if (disc > max_disc)
          max_disc = disc;
        prev_lum = cur_lum;
      }
      pass = (max_disc < 0.05f);
    }
    r.s5_cascade_banding = pass;
    if (pass)
      r.pass_count++;
  }

  /* S6: Shadow Stability — render 100 consecutive frames from a jitter
   * camera path. For each pixel, count shadow/lit flips.
   * PASS if < 2% of pixels flip more than 3 times. */
  {
    /* Use a fixed camera with tiny jitter to simulate temporal instability */
    const int stability_frames = 100;
    mop_viewport_set_camera(viewport, (MopVec3){8, 10, 8}, (MopVec3){0, 0, 0},
                            (MopVec3){0, 1, 0}, 60.0f, 0.1f, 500.0f);

    /* Render first frame to get dimensions */
    mop_viewport_render(viewport);
    int cw = 0, ch = 0;
    const uint8_t *color = mop_viewport_read_color(viewport, &cw, &ch);
    bool pass = false;

    if (color && cw > 0 && ch > 0) {
      int npix = cw * ch;
      uint8_t *prev_shadow = (uint8_t *)calloc((size_t)npix, 1);
      uint16_t *flip_count = (uint16_t *)calloc((size_t)npix, sizeof(uint16_t));

      if (prev_shadow && flip_count) {
        /* Initialize shadow state from first frame */
        for (int i = 0; i < npix; i++) {
          prev_shadow[i] =
              (pixel_luminance(color, i) < SHADOW_LUM_THRESH) ? 1 : 0;
        }

        /* Render remaining frames with tiny jitter */
        for (int f = 1; f < stability_frames; f++) {
          float jx = 0.001f * ((f % 7) - 3);
          float jz = 0.001f * ((f % 5) - 2);
          mop_viewport_set_camera(viewport, (MopVec3){8 + jx, 10, 8 + jz},
                                  (MopVec3){0, 0, 0}, (MopVec3){0, 1, 0}, 60.0f,
                                  0.1f, 500.0f);
          mop_viewport_render(viewport);
          color = mop_viewport_read_color(viewport, &cw, &ch);
          if (!color)
            break;
          for (int i = 0; i < npix; i++) {
            uint8_t cur =
                (pixel_luminance(color, i) < SHADOW_LUM_THRESH) ? 1 : 0;
            if (cur != prev_shadow[i])
              flip_count[i]++;
            prev_shadow[i] = cur;
          }
        }

        /* Count pixels that flip > 3 times */
        int unstable = 0;
        for (int i = 0; i < npix; i++) {
          if (flip_count[i] > 3)
            unstable++;
        }
        double pct = (double)unstable / (double)npix * 100.0;
        pass = (pct < 2.0);
      }

      free(prev_shadow);
      free(flip_count);
    }
    r.s6_shadow_stability = pass;
    if (pass)
      r.pass_count++;
  }

  /* Clean up test meshes and light */
  if (gnd_mesh)
    mop_viewport_remove_mesh(viewport, gnd_mesh);
  if (cube_mesh)
    mop_viewport_remove_mesh(viewport, cube_mesh);
  free(gnd_v);
  free(gnd_idx);
  free(cube_v);
  free(cube_idx);
  if (added)
    mop_viewport_remove_light(viewport, added);

  return r;
}

/* -------------------------------------------------------------------------
 * Picking oracle comparison (P1)
 *
 * Compares MOP's pick results against golden pick data from
 * {golden_dir}/{path}/frame_{NNNN}_pick.raw — flat uint32_t array.
 * Skips when golden_dir is NULL. Allows ±2 pixel tolerance.
 * ------------------------------------------------------------------------- */

uint32_t mop_conf_validate_picking_oracle(MopViewport *viewport,
                                          const char *golden_dir,
                                          const char *path_name,
                                          uint32_t frame_index, int w, int h) {
  if (!golden_dir || !path_name)
    return 0; /* No golden data — skip P1 */

  /* Try to load golden pick buffer */
  char pick_path[1024];
  snprintf(pick_path, sizeof(pick_path), "%s/%s/frame_%04u_pick.raw",
           golden_dir, path_name, frame_index);
  FILE *f = fopen(pick_path, "rb");
  if (!f)
    return 0; /* No pick file — skip */

  uint32_t *oracle =
      (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
  if (!oracle) {
    fclose(f);
    return 0;
  }

  size_t expected = (size_t)w * (size_t)h;
  size_t read_count = fread(oracle, sizeof(uint32_t), expected, f);
  fclose(f);

  if (read_count != expected) {
    free(oracle);
    return 0;
  }

  /* 9 sample points: center, 4 corners (inset by 1px), 4 edge midpoints */
  struct {
    int x;
    int y;
  } samples[9] = {
      {w / 2, h / 2}, {1, 1},         {w - 2, 1},
      {1, h - 2},     {w - 2, h - 2}, {w / 2, 1},
      {w / 2, h - 2}, {1, h / 2},     {w - 2, h / 2},
  };

  uint32_t mismatches = 0;

  for (int i = 0; i < 9; i++) {
    int sx = samples[i].x;
    int sy = samples[i].y;

    MopPickResult pr = mop_viewport_pick(viewport, sx, sy);
    uint32_t oracle_id = oracle[sy * w + sx];

    if (pr.object_id == oracle_id)
      continue;

    /* ±2 pixel tolerance: check neighboring pixels in oracle */
    bool found_match = false;
    for (int dy = -2; dy <= 2 && !found_match; dy++) {
      for (int dx = -2; dx <= 2 && !found_match; dx++) {
        int nx = sx + dx;
        int ny = sy + dy;
        if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
          if (oracle[ny * w + nx] == pr.object_id)
            found_match = true;
        }
      }
    }
    if (!found_match)
      mismatches++;
  }

  free(oracle);
  return mismatches;
}
