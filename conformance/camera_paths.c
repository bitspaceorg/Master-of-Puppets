/*
 * Master of Puppets — Conformance Framework
 * camera_paths.c — Parametric camera stress paths
 *
 * Seven deterministic camera trajectories for conformance testing:
 *   ORBIT          — circular orbit around scene center
 *   ZOOM           — logarithmic zoom from far to near-collision
 *   FOV_SWEEP      — fixed position, FOV swept 1..179 degrees
 *   JITTER         — high-frequency sub-pixel oscillation
 *   EXTREME_NEAR   — near plane at 1e-6, camera touching geometry
 *   HIERARCHY_FLY  — fly through spiral tower nodes (Zone B)
 *   TRANSPARENCY   — orbit around Zone D transparent objects
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_paths.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ORBIT_FRAMES 3000u
#define ZOOM_FRAMES 2000u
#define FOV_SWEEP_FRAMES 1000u
#define JITTER_FRAMES 2000u
#define EXTREME_NEAR_FRAMES 500u
#define HIERARCHY_FRAMES 1200u
#define TRANSPARENCY_FRAMES 500u

/* Maximum tower nodes we store for HIERARCHY_FLY */
#define MAX_TOWER_NODES 256

/* ------------------------------------------------------------------------- */
/* Static state for HIERARCHY_FLY tower positions                            */
/* ------------------------------------------------------------------------- */

static MopVec3 s_tower_positions[MAX_TOWER_NODES];
static uint32_t s_tower_count;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static inline MopVec3 vec3(float x, float y, float z) {
  MopVec3 v;
  v.x = x;
  v.y = y;
  v.z = z;
  return v;
}

static inline float maxf(float a, float b) { return a > b ? a : b; }

/* ------------------------------------------------------------------------- */
/* Frame counts                                                              */
/* ------------------------------------------------------------------------- */

static const uint32_t s_frame_counts[MOP_PATH_COUNT] = {
    [MOP_PATH_ORBIT] = ORBIT_FRAMES,
    [MOP_PATH_ZOOM] = ZOOM_FRAMES,
    [MOP_PATH_FOV_SWEEP] = FOV_SWEEP_FRAMES,
    [MOP_PATH_JITTER] = JITTER_FRAMES,
    [MOP_PATH_EXTREME_NEAR] = EXTREME_NEAR_FRAMES,
    [MOP_PATH_HIERARCHY_FLY] = HIERARCHY_FRAMES,
    [MOP_PATH_TRANSPARENCY] = TRANSPARENCY_FRAMES,
};

/* ------------------------------------------------------------------------- */
/* Path names                                                                */
/* ------------------------------------------------------------------------- */

static const char *s_path_names[MOP_PATH_COUNT] = {
    [MOP_PATH_ORBIT] = "ORBIT",
    [MOP_PATH_ZOOM] = "ZOOM",
    [MOP_PATH_FOV_SWEEP] = "FOV_SWEEP",
    [MOP_PATH_JITTER] = "JITTER",
    [MOP_PATH_EXTREME_NEAR] = "EXTREME_NEAR",
    [MOP_PATH_HIERARCHY_FLY] = "HIERARCHY_FLY",
    [MOP_PATH_TRANSPARENCY] = "TRANSPARENCY",
};

/* ------------------------------------------------------------------------- */
/* Individual path evaluators                                                */
/* ------------------------------------------------------------------------- */

static MopConfCameraState eval_orbit(uint32_t t) {
  const float R = 800.0f;
  const float H = 200.0f;
  const float w = (float)(2.0 * M_PI / 3000.0);

  MopConfCameraState s;
  s.eye = vec3(R * cosf(t * w), H, R * sinf(t * w));
  s.target = vec3(0.0f, 50.0f, -400.0f);
  s.up = vec3(0.0f, 1.0f, 0.0f);
  s.fov_degrees = 60.0f;
  s.near_plane = 0.1f;
  s.far_plane = 10000.0f;
  return s;
}

static MopConfCameraState eval_zoom(uint32_t t) {
  /* D(t) = 1000 * 10^(-3t/2000)   maps [0..2000] -> [1000..1.0] */
  double exponent = -3.0 * (double)t / 2000.0;
  float D = (float)(1000.0 * pow(10.0, exponent));

  MopConfCameraState s;
  s.eye = vec3(0.0f, 50.0f, D);
  s.target = vec3(0.0f, 0.0f, -5.0f);
  s.up = vec3(0.0f, 1.0f, 0.0f);
  s.fov_degrees = 60.0f;
  s.near_plane = maxf(D * 0.001f, 1e-6f);
  s.far_plane = maxf(D * 100.0f, 1e8f);
  return s;
}

static MopConfCameraState eval_fov_sweep(uint32_t t) {
  MopConfCameraState s;
  s.eye = vec3(0.0f, 100.0f, 200.0f);
  s.target = vec3(0.0f, 0.0f, -400.0f);
  s.up = vec3(0.0f, 1.0f, 0.0f);
  s.fov_degrees = 1.0f + 178.0f * ((float)t / 1000.0f);
  s.near_plane = 0.01f;
  s.far_plane = 50000.0f;
  return s;
}

static MopConfCameraState eval_jitter(uint32_t t) {
  float ft = (float)t;

  MopConfCameraState s;
  s.eye =
      vec3(0.0f + sinf(ft * 17.3f) * 0.001f, 100.0f + cosf(ft * 23.7f) * 0.001f,
           200.0f + sinf(ft * 31.1f) * 0.001f);
  s.target = vec3(0.0f, 0.0f, -400.0f);
  s.up = vec3(0.0f, 1.0f, 0.0f);
  s.fov_degrees = 60.0f;
  s.near_plane = 0.1f;
  s.far_plane = 10000.0f;
  return s;
}

static MopConfCameraState eval_extreme_near(uint32_t t) {
  (void)t;

  MopConfCameraState s;
  s.eye = vec3(0.0f, 0.001f, 0.001f);
  s.target = vec3(0.0f, 0.0f, 0.0f);
  s.up = vec3(0.0f, 1.0f, 0.0f);
  s.fov_degrees = 90.0f;
  s.near_plane = 1e-6f;
  s.far_plane = 1e8f;
  return s;
}

static MopConfCameraState eval_hierarchy_fly(uint32_t t) {
  /* Index into tower positions: one node per 50 frames */
  uint32_t idx = t / 50;
  if (idx >= s_tower_count) {
    idx = s_tower_count > 0 ? s_tower_count - 1 : 0;
  }

  MopVec3 node_pos =
      (s_tower_count > 0) ? s_tower_positions[idx] : vec3(0.0f, 0.0f, 0.0f);

  MopConfCameraState s;
  s.eye = vec3(node_pos.x + 5.0f, node_pos.y, node_pos.z + 5.0f);
  s.target = node_pos;
  s.up = vec3(0.0f, 1.0f, 0.0f);
  s.fov_degrees = 60.0f;
  s.near_plane = 0.1f;
  s.far_plane = 5000.0f;
  return s;
}

static MopConfCameraState eval_transparency(uint32_t t) {
  const float w = (float)(2.0 * M_PI / 500.0);

  MopConfCameraState s;
  s.eye =
      vec3(200.0f + 30.0f * cosf(t * w), 10.0f, -400.0f + 30.0f * sinf(t * w));
  s.target = vec3(200.0f, 0.0f, -400.0f);
  s.up = vec3(0.0f, 1.0f, 0.0f);
  s.fov_degrees = 60.0f;
  s.near_plane = 0.1f;
  s.far_plane = 5000.0f;
  return s;
}

/* ------------------------------------------------------------------------- */
/* Dispatch table                                                            */
/* ------------------------------------------------------------------------- */

typedef MopConfCameraState (*PathEvalFn)(uint32_t frame);

static const PathEvalFn s_evaluators[MOP_PATH_COUNT] = {
    [MOP_PATH_ORBIT] = eval_orbit,
    [MOP_PATH_ZOOM] = eval_zoom,
    [MOP_PATH_FOV_SWEEP] = eval_fov_sweep,
    [MOP_PATH_JITTER] = eval_jitter,
    [MOP_PATH_EXTREME_NEAR] = eval_extreme_near,
    [MOP_PATH_HIERARCHY_FLY] = eval_hierarchy_fly,
    [MOP_PATH_TRANSPARENCY] = eval_transparency,
};

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

uint32_t mop_camera_path_frame_count(MopCameraPathId path) {
  if (path < 0 || path >= MOP_PATH_COUNT)
    return 0;
  return s_frame_counts[path];
}

MopConfCameraState mop_camera_path_evaluate(MopCameraPathId path,
                                            uint32_t frame) {
  if (path >= 0 && path < MOP_PATH_COUNT && s_evaluators[path]) {
    return s_evaluators[path](frame);
  }
  /* Fallback: identity-ish camera */
  MopConfCameraState s;
  memset(&s, 0, sizeof(s));
  s.eye = vec3(0.0f, 0.0f, 5.0f);
  s.target = vec3(0.0f, 0.0f, 0.0f);
  s.up = vec3(0.0f, 1.0f, 0.0f);
  s.fov_degrees = 60.0f;
  s.near_plane = 0.1f;
  s.far_plane = 1000.0f;
  return s;
}

const char *mop_camera_path_name(MopCameraPathId path) {
  if (path < 0 || path >= MOP_PATH_COUNT)
    return "UNKNOWN";
  return s_path_names[path];
}

void mop_camera_path_set_tower_positions(const MopVec3 *positions,
                                         uint32_t count) {
  if (!positions || count == 0) {
    s_tower_count = 0;
    return;
  }
  if (count > MAX_TOWER_NODES)
    count = MAX_TOWER_NODES;
  memcpy(s_tower_positions, positions, count * sizeof(MopVec3));
  s_tower_count = count;
}
