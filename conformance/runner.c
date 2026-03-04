/*
 * Master of Puppets — Conformance Framework
 * runner.c — Main conformance runner and CLI entry point
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_paths.h"
#include "conformance.h"
#include "metrics.h"
#include "report.h"
#include "scene_gen.h"
#include "stability.h"
#include "validator.h"

#include "stb_image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Vulkan validation layer error tracking
 *
 * The VK_EXT_debug_utils callback in vulkan_backend.c calls these via
 * the mop_vk_on_validation_error / mop_vk_on_sync_hazard function pointers.
 * We register our counter functions at startup (see register_validation_hooks).
 * ------------------------------------------------------------------------- */

static uint32_t s_validation_errors = 0;
static uint32_t s_sync_hazards = 0;

#if defined(MOP_HAS_VULKAN)
#include "backend/vulkan/vulkan_internal.h"

static void on_validation_error(void) { s_validation_errors++; }

static void on_sync_hazard(void) { s_sync_hazards++; }
#endif

static void register_validation_hooks(void) {
#if defined(MOP_HAS_VULKAN)
  mop_vk_on_validation_error = on_validation_error;
  mop_vk_on_sync_hazard = on_sync_hazard;
#endif
}

static void reset_validation_counters(void) {
  s_validation_errors = 0;
  s_sync_hazards = 0;
}

/* -------------------------------------------------------------------------
 * Golden baseline loading via stb_image
 * ------------------------------------------------------------------------- */

static uint8_t *load_golden_frame(const char *golden_dir, const char *path_name,
                                  uint32_t frame_index, int *out_w,
                                  int *out_h) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s/frame_%04u.png", golden_dir, path_name,
           frame_index);
  int channels;
  uint8_t *data = stbi_load(path, out_w, out_h, &channels, 4); /* force RGBA */
  return data; /* caller frees with stbi_image_free() */
}

/* -------------------------------------------------------------------------
 * Lighting configurations (Section 3 of the spec)
 * ------------------------------------------------------------------------- */

static const char *light_config_name(MopLightConfigId config) {
  switch (config) {
  case MOP_LIGHT_CONFIG_SUNONLY:
    return "SUNONLY";
  case MOP_LIGHT_CONFIG_POINTS:
    return "POINTS";
  case MOP_LIGHT_CONFIG_SPOTS:
    return "SPOTS";
  case MOP_LIGHT_CONFIG_FULL:
    return "FULL";
  case MOP_LIGHT_CONFIG_AMBIENT_ONLY:
    return "AMBIENT_ONLY";
  case MOP_LIGHT_CONFIG_DARK:
    return "DARK";
  case MOP_LIGHT_CONFIG_SINGLE_POINT:
    return "SINGLE_POINT";
  case MOP_LIGHT_CONFIG_SINGLE_SPOT:
    return "SINGLE_SPOT";
  default:
    return "UNKNOWN";
  }
}

static void apply_light_config(MopViewport *vp, MopLightConfigId config) {
  mop_viewport_clear_lights(vp);

  /* L1: Directional (sun) */
  MopLight l1 = {.type = MOP_LIGHT_DIRECTIONAL,
                 .direction = {-0.3f, -1.0f, -0.5f},
                 .color = {1, 1, 1, 1},
                 .intensity = 3.14159f,
                 .active = true,
                 .cast_shadows = true};

  /* L2-L5: Point lights at corners */
  MopLight l2 = {.type = MOP_LIGHT_POINT,
                 .position = {-100, 50, -100},
                 .color = {1, 0.8f, 0.6f, 1},
                 .intensity = 4.0f * 3.14159f * 3.14159f,
                 .range = 300,
                 .active = true,
                 .cast_shadows = true};
  MopLight l3 = {.type = MOP_LIGHT_POINT,
                 .position = {100, 50, -100},
                 .color = {0.6f, 0.8f, 1, 1},
                 .intensity = 4.0f * 3.14159f * 3.14159f,
                 .range = 300,
                 .active = true,
                 .cast_shadows = true};
  MopLight l4 = {.type = MOP_LIGHT_POINT,
                 .position = {-100, 50, -700},
                 .color = {1, 1, 0.8f, 1},
                 .intensity = 4.0f * 3.14159f * 3.14159f,
                 .range = 300,
                 .active = true};
  MopLight l5 = {.type = MOP_LIGHT_POINT,
                 .position = {100, 50, -700},
                 .color = {0.8f, 1, 1, 1},
                 .intensity = 4.0f * 3.14159f * 3.14159f,
                 .range = 300,
                 .active = true};

  /* L6-L7: Spot lights */
  MopLight l6 = {.type = MOP_LIGHT_SPOT,
                 .position = {0, 100, -250},
                 .direction = {0, -1, 0},
                 .color = {1, 1, 1, 1},
                 .intensity = 4.0f * 3.14159f * 3.14159f,
                 .range = 500,
                 .spot_inner_cos = 0.866f, /* cos(30) */
                 .spot_outer_cos = 0.707f, /* cos(45) */
                 .active = true,
                 .cast_shadows = true};
  MopLight l7 = {.type = MOP_LIGHT_SPOT,
                 .position = {200, 80, -400},
                 .direction = {0, -1, 0},
                 .color = {1, 0.9f, 0.8f, 1},
                 .intensity = 4.0f * 3.14159f * 3.14159f,
                 .range = 400,
                 .spot_inner_cos = 0.940f, /* cos(20) */
                 .spot_outer_cos = 0.819f, /* cos(35) */
                 .active = true,
                 .cast_shadows = true};

  switch (config) {
  case MOP_LIGHT_CONFIG_SUNONLY:
    mop_viewport_add_light(vp, &l1);
    break;
  case MOP_LIGHT_CONFIG_POINTS:
    mop_viewport_add_light(vp, &l2);
    mop_viewport_add_light(vp, &l3);
    mop_viewport_add_light(vp, &l4);
    mop_viewport_add_light(vp, &l5);
    break;
  case MOP_LIGHT_CONFIG_SPOTS:
    mop_viewport_add_light(vp, &l6);
    mop_viewport_add_light(vp, &l7);
    break;
  case MOP_LIGHT_CONFIG_FULL:
    mop_viewport_add_light(vp, &l1);
    mop_viewport_add_light(vp, &l2);
    mop_viewport_add_light(vp, &l3);
    mop_viewport_add_light(vp, &l4);
    mop_viewport_add_light(vp, &l5);
    mop_viewport_add_light(vp, &l6);
    mop_viewport_add_light(vp, &l7);
    break;
  case MOP_LIGHT_CONFIG_AMBIENT_ONLY:
    mop_viewport_set_ambient(vp, 1.0f);
    break;
  case MOP_LIGHT_CONFIG_DARK:
    mop_viewport_set_ambient(vp, 0.0f);
    break;
  case MOP_LIGHT_CONFIG_SINGLE_POINT:
    mop_viewport_add_light(vp, &l2);
    break;
  case MOP_LIGHT_CONFIG_SINGLE_SPOT:
    mop_viewport_add_light(vp, &l6);
    break;
  default:
    break;
  }
}

/* -------------------------------------------------------------------------
 * Scene population — add procedural meshes to viewport
 * ------------------------------------------------------------------------- */

static void populate_scene(MopViewport *vp, const MopConfScene *scene) {
  /* Zone A: instanced sphere grid */
  MopMeshDesc sphere_desc = {.vertices = scene->sphere_verts,
                             .vertex_count = scene->sphere_vert_count,
                             .indices = scene->sphere_indices,
                             .index_count = scene->sphere_index_count,
                             .object_id = 1};
  mop_viewport_add_instanced_mesh(vp, &sphere_desc, scene->sphere_transforms,
                                  scene->sphere_instance_count);

  /* Zone B: hierarchy tower */
  MopMesh *tower_meshes[24];
  for (int i = 0; i < 24; i++) {
    MopMeshDesc cyl_desc = {.vertices = scene->cylinder_verts,
                            .vertex_count = scene->cylinder_vert_count,
                            .indices = scene->cylinder_indices,
                            .index_count = scene->cylinder_index_count,
                            .object_id = (uint32_t)(100 + i)};
    tower_meshes[i] = mop_viewport_add_mesh(vp, &cyl_desc);
    if (tower_meshes[i]) {
      mop_mesh_set_transform(tower_meshes[i],
                             &scene->tower_world_transforms[i]);
    }
  }

  /* Zone C: precision stress cube */
  MopMeshDesc cube_desc = {.vertices = scene->cube_verts,
                           .vertex_count = scene->cube_vert_count,
                           .indices = scene->cube_indices,
                           .index_count = scene->cube_index_count,
                           .object_id = 200};
  MopMesh *stress_cube = mop_viewport_add_mesh(vp, &cube_desc);
  (void)stress_cube;

  /* Zone C: precision stress quad */
  MopMeshDesc quad_desc = {.vertices = scene->quad_verts,
                           .vertex_count = scene->quad_vert_count,
                           .indices = scene->quad_indices,
                           .index_count = scene->quad_index_count,
                           .object_id = 201};
  MopMesh *stress_quad = mop_viewport_add_mesh(vp, &quad_desc);
  (void)stress_quad;
}

/* -------------------------------------------------------------------------
 * Run a single camera path, validating each frame
 * ------------------------------------------------------------------------- */

static void
run_camera_path(MopViewport *vp, MopCameraPathId path_id,
                const MopConfThresholds *thresholds,
                MopConfFrameResult *out_frames, uint32_t *out_frame_count,
                MopConfRunnerResult *accum, const MopConfRunnerConfig *config,
                double *gpu_timing_buf, uint32_t *gpu_timing_count) {
  uint32_t count = mop_camera_path_frame_count(path_id);
  const char *name = mop_camera_path_name(path_id);
  bool verbose = config->verbose;
  const char *golden_dir = config->golden_dir;

  if (verbose) {
    fprintf(stderr, "  Path %s: %u frames\n", name, count);
  }

  /* Track SSIM/RMSE accumulators for golden comparison */
  double ssim_sum = 0.0;
  double ssim_min = 1.0;
  double rmse_sum = 0.0;
  double rmse_max = 0.0;
  uint32_t golden_compared = 0;

  for (uint32_t t = 0; t < count; t++) {
    MopConfCameraState cam = mop_camera_path_evaluate(path_id, t);

    mop_viewport_set_camera(vp, cam.eye, cam.target, cam.up, cam.fov_degrees,
                            cam.near_plane, cam.far_plane);
    mop_viewport_render(vp);

    /* Read GPU frame time if timing collection is enabled */
    if (config->collect_timing && gpu_timing_buf && gpu_timing_count) {
      float gpu_ms = mop_viewport_gpu_frame_time_ms(vp);
      gpu_timing_buf[*gpu_timing_count] = (double)gpu_ms;
      (*gpu_timing_count)++;
    }

    int w = 0, h = 0;
    const uint8_t *pixels = mop_viewport_read_color(vp, &w, &h);

    MopConfFrameResult fr;
    memset(&fr, 0, sizeof(fr));
    fr.frame_index = *out_frame_count;

    /* NaN scan */
    if (pixels) {
      fr.has_nan = (mop_scan_nan_rgba(pixels, w, h) > 0);
    }

    /* Picking invariants */
    fr.pick_invariant_fails = mop_conf_validate_picking_invariants(vp, w, h);

    /* P1: Oracle pick comparison (when golden data available) */
    if (golden_dir) {
      fr.pick_oracle_mismatches =
          mop_conf_validate_picking_oracle(vp, golden_dir, name, t, w, h);
    }

    /* Golden baseline comparison */
    if (golden_dir && pixels) {
      int gw = 0, gh = 0;
      uint8_t *golden = load_golden_frame(golden_dir, name, t, &gw, &gh);
      if (golden) {
        if (gw == w && gh == h) {
          /* Full validation pipeline with reference image */
          fr = mop_conf_validate_frame(pixels, golden, NULL, NULL, w, h,
                                       thresholds);
          fr.frame_index = *out_frame_count;

          /* Accumulate golden metrics */
          ssim_sum += fr.metrics.ssim;
          if (fr.metrics.ssim < ssim_min)
            ssim_min = fr.metrics.ssim;
          rmse_sum += fr.metrics.rmse;
          if (fr.metrics.rmse > rmse_max)
            rmse_max = fr.metrics.rmse;
          golden_compared++;

          /* Generate diff image for FAIL frames */
          if (fr.verdict == MOP_CONF_FAIL && config->output_dir) {
            mop_conf_report_diff_image(config->output_dir, fr.frame_index,
                                       pixels, golden, w, h);
          }
        } else {
          if (verbose) {
            fprintf(stderr,
                    "    WARNING: golden frame %u size mismatch "
                    "(%dx%d vs %dx%d)\n",
                    t, gw, gh, w, h);
          }
        }
        stbi_image_free(golden);
      }
      /* If golden not found, fall through to self-consistency checks */
    }

    /* If no golden comparison was done, use self-consistency verdict */
    if (!golden_dir || golden_compared == 0 ||
        fr.frame_index == *out_frame_count) {
      if (fr.verdict == 0 && !fr.has_nan && !fr.has_inf &&
          fr.pick_invariant_fails == 0) {
        fr.verdict = MOP_CONF_PASS;
      } else if (fr.has_nan || fr.has_inf) {
        fr.verdict = MOP_CONF_FAIL;
      } else if (fr.pick_invariant_fails > 0) {
        fr.verdict = MOP_CONF_FAIL;
      }
    }

    /* Accumulate */
    accum->total_frames++;
    if (fr.verdict == MOP_CONF_PASS)
      accum->pass_frames++;
    else if (fr.verdict == MOP_CONF_WARN)
      accum->warn_frames++;
    else
      accum->fail_frames++;

    if (fr.has_nan)
      accum->nan_detected++;
    accum->pick_invariant_failures += fr.pick_invariant_fails;

    out_frames[*out_frame_count] = fr;
    (*out_frame_count)++;

    if (verbose && (t % 500 == 0 || t == count - 1)) {
      fprintf(stderr, "    frame %u/%u  verdict=%s\n", t, count,
              fr.verdict == MOP_CONF_PASS   ? "PASS"
              : fr.verdict == MOP_CONF_WARN ? "WARN"
                                            : "FAIL");
    }
  }

  /* Accumulate golden metrics into the runner result */
  if (golden_compared > 0) {
    double path_mean_ssim = ssim_sum / golden_compared;
    double path_mean_rmse = rmse_sum / golden_compared;

    /* Weighted merge into running accumulators.
     * Use frame count weighting for means. */
    uint32_t prev_golden = accum->total_frames - golden_compared;
    if (prev_golden > 0 && accum->mean_ssim > 0.0) {
      accum->mean_ssim =
          (accum->mean_ssim * prev_golden + path_mean_ssim * golden_compared) /
          accum->total_frames;
      accum->mean_rmse =
          (accum->mean_rmse * prev_golden + path_mean_rmse * golden_compared) /
          accum->total_frames;
    } else {
      accum->mean_ssim = path_mean_ssim;
      accum->mean_rmse = path_mean_rmse;
    }

    if (ssim_min < accum->min_ssim || accum->min_ssim == 0.0)
      accum->min_ssim = ssim_min;
    if (rmse_max > accum->max_rmse)
      accum->max_rmse = rmse_max;

    if (verbose) {
      fprintf(stderr,
              "    Golden comparison: %u frames, mean_ssim=%.4f, "
              "min_ssim=%.4f, mean_rmse=%.4f, max_rmse=%.4f\n",
              golden_compared, path_mean_ssim, ssim_min, path_mean_rmse,
              rmse_max);
    }
  }
}

/* -------------------------------------------------------------------------
 * Tier implementations
 * ------------------------------------------------------------------------- */

static MopConfRunnerResult run_tier1(MopViewport *vp, bool verbose) {
  MopConfRunnerResult result;
  memset(&result, 0, sizeof(result));

  if (verbose)
    fprintf(stderr, "[Tier 1] Geometric correctness\n");

  /* G1-G9: Geometric correctness */
  MopConfGeomResult geom = mop_conf_test_geometry(vp);
  if (geom.pass_count < geom.total_count) {
    result.overall = MOP_CONF_FAIL;
    if (verbose)
      fprintf(stderr, "  Geometry: %u/%u PASS\n", geom.pass_count,
              geom.total_count);
  }

  /* D1-D6: Depth precision */
  if (verbose)
    fprintf(stderr, "[Tier 1] Depth precision\n");
  MopConfDepthResult depth = mop_conf_test_depth(vp);
  bool depth_ok = depth.d1_near_plane && depth.d2_far_plane &&
                  depth.d3_monotonic && depth.d4_precision &&
                  depth.d5_z_fighting && depth.d6_far_field;
  if (!depth_ok) {
    result.overall = MOP_CONF_FAIL;
    if (verbose)
      fprintf(stderr, "  Depth: FAIL\n");
  }

  /* Determinism */
  if (verbose)
    fprintf(stderr, "[Tier 1] Determinism\n");
  MopConfDriftResult drift = mop_conf_test_determinism(vp, 100);
  if (!drift.deterministic) {
    result.overall = MOP_CONF_FAIL;
    if (verbose)
      fprintf(stderr, "  Determinism: FAIL (hash mismatch)\n");
  }

  /* Matrix drift */
  double matrix_err = mop_conf_test_matrix_drift(10000);
  if (matrix_err > 1e-3) {
    result.overall = MOP_CONF_FAIL;
    if (verbose)
      fprintf(stderr, "  Matrix drift: FAIL (error=%.6f)\n", matrix_err);
  }

  /* Picking invariants (100 frames) */
  if (verbose)
    fprintf(stderr, "[Tier 1] Picking + NaN scan (100 frames)\n");
  MopConfCameraState cam = mop_camera_path_evaluate(MOP_PATH_ORBIT, 0);
  mop_viewport_set_camera(vp, cam.eye, cam.target, cam.up, cam.fov_degrees,
                          cam.near_plane, cam.far_plane);

  for (uint32_t i = 0; i < 100; i++) {
    mop_viewport_render(vp);
    int w = 0, h = 0;
    const uint8_t *pixels = mop_viewport_read_color(vp, &w, &h);
    if (pixels && mop_scan_nan_rgba(pixels, w, h) > 0) {
      result.nan_detected++;
      result.overall = MOP_CONF_FAIL;
    }
    result.pick_invariant_failures +=
        mop_conf_validate_picking_invariants(vp, w, h);
  }

  if (result.pick_invariant_failures > 0)
    result.overall = MOP_CONF_FAIL;

  if (verbose) {
    fprintf(stderr, "[Tier 1] Complete — %s\n",
            result.overall == MOP_CONF_PASS ? "PASS" : "FAIL");
  }

  return result;
}

static MopConfRunnerResult run_tier2(MopViewport *vp,
                                     const MopConfRunnerConfig *config) {
  /* Tier 2 includes Tier 1 */
  MopConfRunnerResult result = run_tier1(vp, config->verbose);

  /* 3 camera paths: ORBIT, ZOOM, FOV_SWEEP (6,000 frames) */
  MopCameraPathId paths[] = {MOP_PATH_ORBIT, MOP_PATH_ZOOM, MOP_PATH_FOV_SWEEP};
  uint32_t total_frames = 0;
  for (int i = 0; i < 3; i++)
    total_frames += mop_camera_path_frame_count(paths[i]);

  MopConfFrameResult *frames =
      (MopConfFrameResult *)calloc(total_frames, sizeof(MopConfFrameResult));
  if (!frames)
    return result;

  /* Timing arrays */
  double *gpu_timing = NULL;
  uint32_t gpu_timing_count = 0;
  if (config->collect_timing) {
    gpu_timing = (double *)calloc(total_frames, sizeof(double));
  }

  MopConfThresholds thresholds = mop_conf_thresholds_standard();
  uint32_t frame_idx = 0;

  if (config->verbose)
    fprintf(stderr, "[Tier 2] Camera paths (3 paths, %u frames)\n",
            total_frames);

  for (int i = 0; i < 3; i++) {
    run_camera_path(vp, paths[i], &thresholds, frames, &frame_idx, &result,
                    config, gpu_timing, &gpu_timing_count);
  }

  /* Memory leak detection (1,000 frames) */
  if (config->verbose)
    fprintf(stderr, "[Tier 2] Memory leak detection (1000 frames)\n");
  MopConfMemoryResult mem = mop_conf_test_memory(vp, 1000);
  result.rss_slope_kb_per_frame = mem.slope_kb_per_frame;
  if (!mem.passed)
    result.overall = MOP_CONF_FAIL;

  /* Generate reports */
  if (config->output_dir) {
    MopConfGeomResult geom = mop_conf_test_geometry(vp);
    MopConfDepthResult depth = mop_conf_test_depth(vp);
    mop_conf_report_json(config->output_dir, frames, frame_idx, &result);
    mop_conf_report_summary(config->output_dir, &result, &geom, &depth, NULL);
    mop_conf_report_memory(config->output_dir, mem.rss_samples_kb,
                           mem.sample_count, mem.slope_kb_per_frame);
    if (config->collect_timing && gpu_timing) {
      mop_conf_report_timings(config->output_dir, gpu_timing, NULL,
                              gpu_timing_count);
    }
  }

  /* Determine final verdict */
  if (result.fail_frames > 0 && result.overall < MOP_CONF_FAIL)
    result.overall = MOP_CONF_FAIL;
  else if (result.warn_frames > 0 && result.overall < MOP_CONF_WARN)
    result.overall = MOP_CONF_WARN;

  if (config->verbose) {
    fprintf(stderr, "[Tier 2] Complete — %s (%u pass, %u warn, %u fail)\n",
            result.overall == MOP_CONF_PASS   ? "PASS"
            : result.overall == MOP_CONF_WARN ? "WARN"
                                              : "FAIL",
            result.pass_frames, result.warn_frames, result.fail_frames);
  }

  free(gpu_timing);
  free(frames);
  return result;
}

static MopConfRunnerResult run_tier3(MopViewport *vp,
                                     const MopConfRunnerConfig *config) {
  /* Tier 3 includes Tier 1 checks, then all 7 camera paths */
  MopConfRunnerResult result = run_tier1(vp, config->verbose);

  /* All 7 camera paths (10,200 frames) */
  uint32_t total_frames = 0;
  for (int i = 0; i < MOP_PATH_COUNT; i++)
    total_frames += mop_camera_path_frame_count((MopCameraPathId)i);

  MopConfFrameResult *frames =
      (MopConfFrameResult *)calloc(total_frames, sizeof(MopConfFrameResult));
  if (!frames)
    return result;

  /* Timing arrays */
  double *gpu_timing = NULL;
  uint32_t gpu_timing_count = 0;
  if (config->collect_timing) {
    gpu_timing = (double *)calloc(total_frames, sizeof(double));
  }

  MopConfThresholds thresholds = mop_conf_thresholds_standard();
  uint32_t frame_idx = 0;

  if (config->verbose)
    fprintf(stderr, "[Tier 3] All camera paths (%u frames)\n", total_frames);

  for (int i = 0; i < MOP_PATH_COUNT; i++) {
    run_camera_path(vp, (MopCameraPathId)i, &thresholds, frames, &frame_idx,
                    &result, config, gpu_timing, &gpu_timing_count);
  }

  /* Shadow tests S1-S6 */
  if (config->verbose)
    fprintf(stderr, "[Tier 3] Shadow tests (S1-S6)\n");
  MopConfShadowResult shadow = mop_conf_test_shadows(vp);
  if (shadow.pass_count < shadow.total_count) {
    result.overall = MOP_CONF_FAIL;
    if (config->verbose)
      fprintf(stderr, "  Shadows: %u/%u PASS\n", shadow.pass_count,
              shadow.total_count);
  }

  /* Stability tests */
  if (config->verbose)
    fprintf(stderr, "[Tier 3] Memory leak detection (10000 frames)\n");
  MopConfMemoryResult mem = mop_conf_test_memory(vp, 10000);
  result.rss_slope_kb_per_frame = mem.slope_kb_per_frame;
  if (!mem.passed)
    result.overall = MOP_CONF_FAIL;

  if (config->verbose)
    fprintf(stderr, "[Tier 3] Determinism test\n");
  MopConfDriftResult drift = mop_conf_test_determinism(vp, 10000);
  if (!drift.deterministic) {
    result.overall = MOP_CONF_FAIL;
    if (config->verbose)
      fprintf(stderr, "  Determinism: FAIL\n");
  }

  /* Generate reports */
  if (config->output_dir) {
    MopConfGeomResult geom = mop_conf_test_geometry(vp);
    MopConfDepthResult depth = mop_conf_test_depth(vp);
    mop_conf_report_json(config->output_dir, frames, frame_idx, &result);
    mop_conf_report_summary(config->output_dir, &result, &geom, &depth,
                            &shadow);
    mop_conf_report_memory(config->output_dir, mem.rss_samples_kb,
                           mem.sample_count, mem.slope_kb_per_frame);
    if (config->collect_timing && gpu_timing) {
      mop_conf_report_timings(config->output_dir, gpu_timing, NULL,
                              gpu_timing_count);
    }
  }

  if (result.fail_frames > 0 && result.overall < MOP_CONF_FAIL)
    result.overall = MOP_CONF_FAIL;
  else if (result.warn_frames > 0 && result.overall < MOP_CONF_WARN)
    result.overall = MOP_CONF_WARN;

  if (config->verbose) {
    fprintf(stderr, "[Tier 3] Complete — %s (%u pass, %u warn, %u fail)\n",
            result.overall == MOP_CONF_PASS   ? "PASS"
            : result.overall == MOP_CONF_WARN ? "WARN"
                                              : "FAIL",
            result.pass_frames, result.warn_frames, result.fail_frames);
  }

  free(gpu_timing);
  free(frames);
  return result;
}

/* -------------------------------------------------------------------------
 * Tier 4 — Exhaustive: 8 lighting configs x 3 selected camera paths = 24 runs
 * ------------------------------------------------------------------------- */

static MopConfRunnerResult run_tier4(MopViewport *vp,
                                     const MopConfRunnerConfig *config) {
  /* Start with Tier 1 checks */
  MopConfRunnerResult result = run_tier1(vp, config->verbose);

  /* 8 lighting configs x 3 paths (ORBIT, ZOOM, FOV_SWEEP) = 24 runs */
  MopCameraPathId paths[] = {MOP_PATH_ORBIT, MOP_PATH_ZOOM, MOP_PATH_FOV_SWEEP};
  int num_paths = 3;

  /* Calculate total frames across all 24 runs */
  uint32_t frames_per_run = 0;
  for (int p = 0; p < num_paths; p++)
    frames_per_run += mop_camera_path_frame_count(paths[p]);
  uint32_t total_frames = frames_per_run * MOP_LIGHT_CONFIG_COUNT;

  MopConfFrameResult *frames =
      (MopConfFrameResult *)calloc(total_frames, sizeof(MopConfFrameResult));
  if (!frames)
    return result;

  /* Timing arrays */
  double *gpu_timing = NULL;
  uint32_t gpu_timing_count = 0;
  if (config->collect_timing) {
    gpu_timing = (double *)calloc(total_frames, sizeof(double));
  }

  MopConfThresholds thresholds = mop_conf_thresholds_standard();
  uint32_t frame_idx = 0;

  if (config->verbose)
    fprintf(stderr,
            "[Tier 4] Exhaustive: %d lighting x %d paths = %d runs "
            "(%u frames)\n",
            MOP_LIGHT_CONFIG_COUNT, num_paths,
            MOP_LIGHT_CONFIG_COUNT * num_paths, total_frames);

  for (int lc = 0; lc < MOP_LIGHT_CONFIG_COUNT; lc++) {
    MopLightConfigId light_config = (MopLightConfigId)lc;
    apply_light_config(vp, light_config);

    if (config->verbose)
      fprintf(stderr, "  Lighting: %s\n", light_config_name(light_config));

    for (int p = 0; p < num_paths; p++) {
      run_camera_path(vp, paths[p], &thresholds, frames, &frame_idx, &result,
                      config, gpu_timing, &gpu_timing_count);
    }
  }

  /* Shadow tests S1-S6 */
  if (config->verbose)
    fprintf(stderr, "[Tier 4] Shadow tests (S1-S6)\n");
  MopConfShadowResult shadow = mop_conf_test_shadows(vp);
  if (shadow.pass_count < shadow.total_count) {
    result.overall = MOP_CONF_FAIL;
    if (config->verbose)
      fprintf(stderr, "  Shadows: %u/%u PASS\n", shadow.pass_count,
              shadow.total_count);
  }

  /* Stability tests */
  if (config->verbose)
    fprintf(stderr, "[Tier 4] Memory leak detection (10000 frames)\n");
  MopConfMemoryResult mem = mop_conf_test_memory(vp, 10000);
  result.rss_slope_kb_per_frame = mem.slope_kb_per_frame;
  if (!mem.passed)
    result.overall = MOP_CONF_FAIL;

  if (config->verbose)
    fprintf(stderr, "[Tier 4] Determinism test\n");
  MopConfDriftResult drift = mop_conf_test_determinism(vp, 10000);
  if (!drift.deterministic) {
    result.overall = MOP_CONF_FAIL;
    if (config->verbose)
      fprintf(stderr, "  Determinism: FAIL\n");
  }

  /* Generate reports */
  if (config->output_dir) {
    MopConfGeomResult geom = mop_conf_test_geometry(vp);
    MopConfDepthResult depth = mop_conf_test_depth(vp);
    mop_conf_report_json(config->output_dir, frames, frame_idx, &result);
    mop_conf_report_summary(config->output_dir, &result, &geom, &depth,
                            &shadow);
    mop_conf_report_memory(config->output_dir, mem.rss_samples_kb,
                           mem.sample_count, mem.slope_kb_per_frame);
    if (config->collect_timing && gpu_timing) {
      mop_conf_report_timings(config->output_dir, gpu_timing, NULL,
                              gpu_timing_count);
    }
  }

  if (result.fail_frames > 0 && result.overall < MOP_CONF_FAIL)
    result.overall = MOP_CONF_FAIL;
  else if (result.warn_frames > 0 && result.overall < MOP_CONF_WARN)
    result.overall = MOP_CONF_WARN;

  if (config->verbose) {
    fprintf(stderr, "[Tier 4] Complete — %s (%u pass, %u warn, %u fail)\n",
            result.overall == MOP_CONF_PASS   ? "PASS"
            : result.overall == MOP_CONF_WARN ? "WARN"
                                              : "FAIL",
            result.pass_frames, result.warn_frames, result.fail_frames);
  }

  free(gpu_timing);
  free(frames);
  return result;
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------------- */

MopConfRunnerResult mop_conformance_run(const MopConfRunnerConfig *config) {
  MopConfRunnerResult result;
  memset(&result, 0, sizeof(result));

  /* Register Vulkan validation hooks and reset counters */
  register_validation_hooks();
  reset_validation_counters();

  int w = config->width > 0 ? config->width : 1920;
  int h = config->height > 0 ? config->height : 1080;

  if (config->verbose) {
    fprintf(stderr, "=== MOP Conformance Runner ===\n");
    fprintf(stderr, "  Tier:    %d\n", config->tier);
    fprintf(stderr, "  Size:    %dx%d\n", w, h);
    fprintf(stderr, "  Backend: %d\n", config->backend);
    if (config->golden_dir)
      fprintf(stderr, "  Golden:  %s\n", config->golden_dir);
    if (config->collect_timing)
      fprintf(stderr, "  Timing:  enabled\n");
    fprintf(stderr, "\n");
  }

  /* Create viewport */
  MopViewportDesc vp_desc = {
      .width = w, .height = h, .backend = config->backend};
  MopViewport *vp = mop_viewport_create(&vp_desc);
  if (!vp) {
    fprintf(stderr, "ERROR: Failed to create viewport\n");
    result.overall = MOP_CONF_FAIL;
    return result;
  }

  /* Disable editor chrome for clean rendering */
  mop_viewport_set_chrome(vp, false);
  mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);

  /* Generate scene */
  if (config->verbose)
    fprintf(stderr, "Generating conformance scene...\n");

  MopConfScene *scene = mop_conf_scene_create();
  if (!scene) {
    fprintf(stderr, "ERROR: Failed to create conformance scene\n");
    mop_viewport_destroy(vp);
    result.overall = MOP_CONF_FAIL;
    return result;
  }

  /* Set tower positions for HIERARCHY_FLY camera path */
  MopVec3 tower_positions[24];
  for (int i = 0; i < 24; i++) {
    /* Extract translation from world transform (column 3) */
    tower_positions[i].x = scene->tower_world_transforms[i].d[12];
    tower_positions[i].y = scene->tower_world_transforms[i].d[13];
    tower_positions[i].z = scene->tower_world_transforms[i].d[14];
  }
  mop_camera_path_set_tower_positions(tower_positions, 24);

  /* Populate viewport with scene meshes */
  populate_scene(vp, scene);

  /* Apply default lighting (full config) */
  apply_light_config(vp, MOP_LIGHT_CONFIG_FULL);

  /* Run the appropriate tier */
  switch (config->tier) {
  case MOP_CONF_TIER1:
    result = run_tier1(vp, config->verbose);
    break;
  case MOP_CONF_TIER2:
    result = run_tier2(vp, config);
    break;
  case MOP_CONF_TIER3:
    result = run_tier3(vp, config);
    break;
  case MOP_CONF_TIER4:
    result = run_tier4(vp, config);
    break;
  default:
    result = run_tier1(vp, config->verbose);
    break;
  }

  /* Capture Vulkan validation layer errors */
  result.validation_errors = s_validation_errors;
  result.sync_hazards = s_sync_hazards;

  /* Write validation.log */
  if (config->output_dir) {
    mop_conf_report_validation_log(config->output_dir, result.validation_errors,
                                   result.sync_hazards);
  }

  /* Cleanup */
  mop_conf_scene_destroy(scene);
  mop_viewport_destroy(vp);

  return result;
}

/* -------------------------------------------------------------------------
 * CLI entry point
 * ------------------------------------------------------------------------- */

static void print_usage(const char *argv0) {
  fprintf(stderr, "Usage: %s [options]\n", argv0);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --tier=N       Test tier (1-4, default: 1)\n");
  fprintf(stderr, "  --backend=cpu  Backend type (cpu, default: cpu)\n");
  fprintf(stderr, "  --width=N      Framebuffer width (default: 1920)\n");
  fprintf(stderr, "  --height=N     Framebuffer height (default: 1080)\n");
  fprintf(stderr, "  --output=DIR   Output directory for results\n");
  fprintf(stderr, "  --golden=DIR   Golden baseline directory\n");
  fprintf(stderr, "  --reverse-z    Enable reversed-Z depth\n");
  fprintf(stderr, "  --timing       Enable GPU timing collection\n");
  fprintf(stderr, "  --verbose      Print per-frame info\n");
  fprintf(stderr, "  --help         Show this message\n");
}

int main(int argc, char **argv) {
  MopConfRunnerConfig config;
  memset(&config, 0, sizeof(config));
  config.tier = MOP_CONF_TIER1;
  config.backend = MOP_BACKEND_CPU;
  config.width = 1920;
  config.height = 1080;
  config.verbose = true;

  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "--tier=", 7) == 0) {
      config.tier = (MopConfTier)atoi(argv[i] + 7);
    } else if (strncmp(argv[i], "--backend=", 10) == 0) {
      const char *b = argv[i] + 10;
      if (strcmp(b, "cpu") == 0)
        config.backend = MOP_BACKEND_CPU;
#ifdef MOP_HAS_VULKAN
      else if (strcmp(b, "vulkan") == 0)
        config.backend = MOP_BACKEND_VULKAN;
#endif
#ifdef MOP_HAS_OPENGL
      else if (strcmp(b, "opengl") == 0)
        config.backend = MOP_BACKEND_OPENGL;
#endif
      else {
        fprintf(stderr, "Unknown backend: %s\n", b);
        return 1;
      }
    } else if (strncmp(argv[i], "--width=", 8) == 0) {
      config.width = atoi(argv[i] + 8);
    } else if (strncmp(argv[i], "--height=", 9) == 0) {
      config.height = atoi(argv[i] + 9);
    } else if (strncmp(argv[i], "--output=", 9) == 0) {
      config.output_dir = argv[i] + 9;
    } else if (strncmp(argv[i], "--golden=", 9) == 0) {
      config.golden_dir = argv[i] + 9;
    } else if (strcmp(argv[i], "--reverse-z") == 0) {
      config.reverse_z = true;
    } else if (strcmp(argv[i], "--timing") == 0) {
      config.collect_timing = true;
    } else if (strcmp(argv[i], "--verbose") == 0) {
      config.verbose = true;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  /* Default output dir */
  if (!config.output_dir)
    config.output_dir = "conformance/results";

  MopConfRunnerResult result = mop_conformance_run(&config);

  /* Print final verdict */
  printf("\n");
  printf("========================================\n");
  printf("  CONFORMANCE VERDICT: %s\n", result.overall == MOP_CONF_PASS ? "PASS"
                                        : result.overall == MOP_CONF_WARN
                                            ? "WARN"
                                            : "FAIL");
  printf("========================================\n");
  printf("  Total frames:     %u\n", result.total_frames);
  printf("  Pass:             %u\n", result.pass_frames);
  printf("  Warn:             %u\n", result.warn_frames);
  printf("  Fail:             %u\n", result.fail_frames);
  printf("  NaN detected:     %u\n", result.nan_detected);
  printf("  Pick failures:    %u\n", result.pick_invariant_failures);
  printf("  RSS slope:        %.4f KB/frame\n", result.rss_slope_kb_per_frame);
  if (result.mean_ssim > 0.0) {
    printf("  Mean SSIM:        %.4f\n", result.mean_ssim);
    printf("  Min SSIM:         %.4f\n", result.min_ssim);
    printf("  Mean RMSE:        %.4f\n", result.mean_rmse);
    printf("  Max RMSE:         %.4f\n", result.max_rmse);
  }
  if (result.validation_errors > 0 || result.sync_hazards > 0) {
    printf("  Validation errs:  %u\n", result.validation_errors);
    printf("  Sync hazards:     %u\n", result.sync_hazards);
  }
  printf("========================================\n");

  return (result.overall == MOP_CONF_FAIL) ? 1 : 0;
}
