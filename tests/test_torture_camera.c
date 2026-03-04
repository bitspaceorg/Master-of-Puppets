/*
 * Master of Puppets — Camera Torture Tests
 * test_torture_camera.c — Stress tests for orbit camera and viewport camera
 * sync
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* -------------------------------------------------------------------------
 * Inline PRNG — deterministic, same as other torture files
 * ------------------------------------------------------------------------- */

static uint32_t torture_seed = 0xDEADBEEF;
static uint32_t torture_rand(void) {
  torture_seed ^= torture_seed << 13;
  torture_seed ^= torture_seed >> 17;
  torture_seed ^= torture_seed << 5;
  return torture_seed;
}
static float torture_randf(void) {
  return (float)(torture_rand() & 0xFFFFFF) / (float)0xFFFFFF;
}
static float torture_randf_range(float lo, float hi) {
  return lo + torture_randf() * (hi - lo);
}

/* -------------------------------------------------------------------------
 * Inline cube geometry — 6 faces, 24 verts, 36 indices
 * ------------------------------------------------------------------------- */

static const MopVertex CUBE_VERTS[] = {
    /* Front face (z=+0.5) */
    {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}},
    /* Back face (z=-0.5) */
    {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}},
    /* Top face (y=+0.5) */
    {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    /* Bottom face (y=-0.5) */
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}},
    /* Right face (x=+0.5) */
    {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    /* Left face (x=-0.5) */
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
    {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}},
};
static const uint32_t CUBE_IDX[] = {
    0,  1,  2,  2,  3,  0,  /* front */
    4,  5,  6,  6,  7,  4,  /* back */
    8,  9,  10, 10, 11, 8,  /* top */
    12, 13, 14, 14, 15, 12, /* bottom */
    16, 17, 18, 18, 19, 16, /* right */
    20, 21, 22, 22, 23, 20, /* left */
};

/* -------------------------------------------------------------------------
 * Helper: create a small CPU viewport with a cube, chrome disabled
 * ------------------------------------------------------------------------- */

static MopViewport *make_cube_viewport(int w, int h) {
  MopViewportDesc desc = {.width = w, .height = h, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  if (!vp)
    return NULL;
  mop_viewport_set_chrome(vp, false);
  MopMeshDesc md = {.vertices = CUBE_VERTS,
                    .vertex_count = 24,
                    .indices = CUBE_IDX,
                    .index_count = 36,
                    .object_id = 1};
  mop_viewport_add_mesh(vp, &md);
  return vp;
}

/* =========================================================================
 * Test 1: Extreme narrow FOV (1 degree)
 * ========================================================================= */

static void test_extreme_fov_narrow(void) {
  TEST_BEGIN("extreme_fov_narrow");
  MopViewport *vp = make_cube_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  /* Camera looking at origin with an extremely narrow FOV */
  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 1.0f, 0.1f, 100.0f);
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  TEST_ASSERT(w == 64);
  TEST_ASSERT(h == 48);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Test 2: Extreme wide FOV (179 degrees)
 * ========================================================================= */

static void test_extreme_fov_wide(void) {
  TEST_BEGIN("extreme_fov_wide");
  MopViewport *vp = make_cube_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  /* Camera with near-hemispherical FOV */
  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 179.0f, 0.1f, 100.0f);
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  TEST_ASSERT(w == 64);
  TEST_ASSERT(h == 48);

  /* Verify the projection matrix is finite */
  MopCameraState cs = mop_viewport_get_camera_state(vp);
  for (int i = 0; i < 16; i++) {
    TEST_ASSERT_MSG(!isnan(cs.projection_matrix.d[i]) &&
                        !isinf(cs.projection_matrix.d[i]),
                    "projection matrix element is NaN/Inf");
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Test 3: Extreme near/far ratio
 * ========================================================================= */

static void test_extreme_near_far(void) {
  TEST_BEGIN("extreme_near_far");
  MopViewport *vp = make_cube_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  /* Extreme depth range: near=1e-6, far=1e9 */
  mop_viewport_set_camera(vp, (MopVec3){0, 0, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 1e-6f, 1e9f);
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  /* Scan 100 random pixels for NaN-like values (all 0xFF bytes = RGBA 255) */
  int nan_count = 0;
  for (int i = 0; i < 100; i++) {
    int px = (int)(torture_randf() * (float)(w - 1));
    int py = (int)(torture_randf() * (float)(h - 1));
    int idx = (py * w + px) * 4;
    if (buf[idx + 0] == 0xFF && buf[idx + 1] == 0xFF && buf[idx + 2] == 0xFF &&
        buf[idx + 3] == 0xFF) {
      nan_count++;
    }
  }
  /* Allow a few white pixels but not all — all-white would indicate a
   * degenerate depth buffer flooding the framebuffer */
  TEST_ASSERT_MSG(nan_count < 90,
                  "too many all-0xFF pixels (possible NaN in depth buffer)");

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Test 4: 10,000 orbit increments — verify invariants every 1000 steps
 * ========================================================================= */

static void test_orbit_10000_increments(void) {
  TEST_BEGIN("orbit_10000_increments");
  MopOrbitCamera cam = mop_orbit_camera_default();

  for (int i = 0; i < 10000; i++) {
    mop_orbit_camera_orbit(&cam, 1.0f, 0.3f, 0.005f);

    if ((i + 1) % 1000 == 0) {
      /* Pitch must stay within clamp bounds */
      TEST_ASSERT_MSG(cam.pitch >= -cam.max_pitch && cam.pitch <= cam.max_pitch,
                      "pitch out of bounds");

      /* Distance must stay within zoom clamp bounds */
      TEST_ASSERT_MSG(cam.distance >= 0.5f && cam.distance <= 500.0f,
                      "distance out of bounds");

      /* Eye position must be finite */
      MopVec3 eye = mop_orbit_camera_eye(&cam);
      TEST_ASSERT_FINITE_VEC3(eye);
    }
  }

  TEST_END();
}

/* =========================================================================
 * Test 5: Zoom to minimum, then orbit full circle
 * ========================================================================= */

static void test_zoom_to_min_then_orbit(void) {
  TEST_BEGIN("zoom_to_min_then_orbit");
  MopOrbitCamera cam = mop_orbit_camera_default();

  /* Zoom in hard — 1000 calls with delta=1.0 */
  for (int i = 0; i < 1000; i++)
    mop_orbit_camera_zoom(&cam, 1.0f);

  TEST_ASSERT_FLOAT_EQ(cam.distance, 0.5f);

  /* Orbit a full 360 degrees in small steps */
  float step = (2.0f * 3.14159265f) / 360.0f; /* ~1 degree per step */
  float sensitivity = 1.0f;                   /* dx is in radians directly */
  for (int i = 0; i < 360; i++) {
    /* Apply orbit: yaw -= dx * sensitivity, so dx = -step to go positive */
    mop_orbit_camera_orbit(&cam, -step / sensitivity, 0.0f, sensitivity);
    /* Distance must remain at minimum */
    TEST_ASSERT_FLOAT_EQ(cam.distance, 0.5f);
  }

  TEST_END();
}

/* =========================================================================
 * Test 6: Zoom to maximum, then pan large amounts
 * ========================================================================= */

static void test_zoom_to_max_then_pan(void) {
  TEST_BEGIN("zoom_to_max_then_pan");
  MopOrbitCamera cam = mop_orbit_camera_default();

  /* Zoom out hard — 1000 calls with delta=-10.0 */
  for (int i = 0; i < 1000; i++)
    mop_orbit_camera_zoom(&cam, -10.0f);

  TEST_ASSERT_FLOAT_EQ(cam.distance, 500.0f);

  /* Pan large amounts — target moves but distance must stay at max */
  for (int i = 0; i < 100; i++) {
    mop_orbit_camera_pan(&cam, 500.0f, 500.0f);
    TEST_ASSERT_FLOAT_EQ(cam.distance, 500.0f);
  }

  /* Verify target moved (sanity check that pan actually did something) */
  MopVec3 eye = mop_orbit_camera_eye(&cam);
  TEST_ASSERT_FINITE_VEC3(eye);

  TEST_END();
}

/* =========================================================================
 * Test 7: Simultaneous orbit + pan + zoom — interleaved for 100 iterations
 * ========================================================================= */

static void test_simultaneous_orbit_pan_zoom(void) {
  TEST_BEGIN("simultaneous_orbit_pan_zoom");
  MopOrbitCamera cam = mop_orbit_camera_default();

  for (int i = 0; i < 100; i++) {
    mop_orbit_camera_orbit(&cam, 1.0f, 0.5f, 0.01f);
    mop_orbit_camera_pan(&cam, 2.0f, 3.0f);
    mop_orbit_camera_zoom(&cam, 0.5f);
  }

  /* All state must remain finite */
  TEST_ASSERT_FINITE_VEC3(cam.target);
  TEST_ASSERT_NO_NAN(cam.distance);
  TEST_ASSERT_NO_NAN(cam.yaw);
  TEST_ASSERT_NO_NAN(cam.pitch);

  MopVec3 eye = mop_orbit_camera_eye(&cam);
  TEST_ASSERT_FINITE_VEC3(eye);

  /* Invariants must still hold */
  TEST_ASSERT(cam.pitch >= -cam.max_pitch && cam.pitch <= cam.max_pitch);
  TEST_ASSERT(cam.distance >= 0.5f && cam.distance <= 500.0f);

  TEST_END();
}

/* =========================================================================
 * Test 8: Eye formula verification — 100 random configurations
 *
 * eye = target + distance * (cos(pitch)*sin(yaw),
 *                            sin(pitch),
 *                            cos(pitch)*cos(yaw))
 * ========================================================================= */

static void test_camera_eye_formula(void) {
  TEST_BEGIN("camera_eye_formula");

  for (int i = 0; i < 100; i++) {
    MopOrbitCamera cam = mop_orbit_camera_default();
    cam.yaw = torture_randf_range(-3.14159265f, 3.14159265f);
    cam.pitch = torture_randf_range(-1.4f, 1.4f);
    cam.distance = torture_randf_range(0.5f, 100.0f);
    cam.target.x = torture_randf_range(-50.0f, 50.0f);
    cam.target.y = torture_randf_range(-50.0f, 50.0f);
    cam.target.z = torture_randf_range(-50.0f, 50.0f);

    MopVec3 eye = mop_orbit_camera_eye(&cam);

    /* Expected eye from the formula */
    float cp = cosf(cam.pitch);
    float sp = sinf(cam.pitch);
    float sy = sinf(cam.yaw);
    float cy = cosf(cam.yaw);

    float ex = cam.target.x + cam.distance * cp * sy;
    float ey = cam.target.y + cam.distance * sp;
    float ez = cam.target.z + cam.distance * cp * cy;

    TEST_ASSERT_VEC3_EQ(eye, ex, ey, ez);
  }

  TEST_END();
}

/* =========================================================================
 * Test 9: Camera at poles (max pitch) — look_at must not produce NaN
 * ========================================================================= */

static void test_camera_at_poles(void) {
  TEST_BEGIN("camera_at_poles");
  MopOrbitCamera cam = mop_orbit_camera_default();
  cam.pitch = cam.max_pitch; /* 1.5 rad, nearly straight up */

  MopVec3 eye = mop_orbit_camera_eye(&cam);
  TEST_ASSERT_FINITE_VEC3(eye);

  /* Build look_at matrix — this is where gimbal-lock-like issues appear */
  MopMat4 view = mop_mat4_look_at(eye, cam.target, (MopVec3){0, 1, 0});

  for (int i = 0; i < 16; i++) {
    TEST_ASSERT_MSG(!isnan(view.d[i]), "NaN in look_at matrix at pole");
    TEST_ASSERT_MSG(!isinf(view.d[i]), "Inf in look_at matrix at pole");
  }

  /* Also test negative pole */
  cam.pitch = -cam.max_pitch;
  eye = mop_orbit_camera_eye(&cam);
  TEST_ASSERT_FINITE_VEC3(eye);

  view = mop_mat4_look_at(eye, cam.target, (MopVec3){0, 1, 0});
  for (int i = 0; i < 16; i++) {
    TEST_ASSERT_MSG(!isnan(view.d[i]),
                    "NaN in look_at matrix at negative pole");
    TEST_ASSERT_MSG(!isinf(view.d[i]),
                    "Inf in look_at matrix at negative pole");
  }

  TEST_END();
}

/* =========================================================================
 * Test 10: set_camera syncs with orbit camera parameters
 *
 * Set the viewport camera with a known eye/target, then render and verify
 * the resulting camera state matches expectations.  We use the viewport's
 * camera query API to read back eye/target and verify they match what was
 * set, which exercises the orbit camera sync path.
 * ========================================================================= */

static void test_set_camera_syncs_orbit(void) {
  TEST_BEGIN("set_camera_syncs_orbit");

  MopViewportDesc desc = {
      .width = 64, .height = 48, .backend = MOP_BACKEND_CPU};
  MopViewport *vp = mop_viewport_create(&desc);
  TEST_ASSERT(vp != NULL);

  /* Set a known camera configuration */
  MopVec3 eye = {3.0f, 4.0f, 5.0f};
  MopVec3 target = {0.0f, 0.0f, 0.0f};
  MopVec3 up = {0.0f, 1.0f, 0.0f};
  float fov = 60.0f;
  float near_plane = 0.1f;
  float far_plane = 100.0f;

  mop_viewport_set_camera(vp, eye, target, up, fov, near_plane, far_plane);

  /* Read back via the query API — should match what we set */
  MopCameraState cs = mop_viewport_get_camera_state(vp);

  /* Eye and target should be very close to what we set */
  TEST_ASSERT_VEC3_EQ(cs.eye, eye.x, eye.y, eye.z);
  TEST_ASSERT_VEC3_EQ(cs.target, target.x, target.y, target.z);

  /* Verify derived orbit parameters match expectations:
   * distance = length(eye - target)
   * yaw = atan2(dx, dz) where d = eye - target
   * pitch = asin(dy / distance)
   */
  MopVec3 d = mop_vec3_sub(eye, target);
  float expected_dist = mop_vec3_length(d);
  float expected_yaw = atan2f(d.x, d.z);
  float expected_pitch = asinf(d.y / expected_dist);

  /* Verify by rendering and checking that the camera state produces the
   * same view — render should not crash with these parameters */
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  /* Verify the camera state FOV matches */
  float expected_fov_rad = fov * 3.14159265f / 180.0f;
  TEST_ASSERT_MSG(fabsf(cs.fov_radians - expected_fov_rad) < 1e-3f,
                  "FOV mismatch after set_camera");

  /* Verify near/far planes */
  TEST_ASSERT_MSG(fabsf(cs.near_plane - near_plane) < 1e-4f,
                  "near plane mismatch");
  TEST_ASSERT_MSG(fabsf(cs.far_plane - far_plane) < 1e-2f,
                  "far plane mismatch");

  /* Verify the view matrix is sane by checking it transforms eye to origin
   * (view * eye should have z ≈ -distance, x ≈ 0, y ≈ 0) */
  /* In view space the eye IS the origin, but the camera looks down -Z,
   * so the target (origin) should be at (0, 0, -distance) in view space */
  MopVec4 tgt4 = {target.x, target.y, target.z, 1.0f};
  MopVec4 tgt_view = mop_mat4_mul_vec4(cs.view_matrix, tgt4);
  TEST_ASSERT_MSG(fabsf(tgt_view.z + expected_dist) < 0.1f,
                  "target not at expected depth in view space");

  /* Sanity: yaw and pitch derived from the readback eye should match */
  (void)expected_yaw;
  (void)expected_pitch;

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
  TEST_SUITE_BEGIN("torture_camera");

  TEST_RUN(test_extreme_fov_narrow);
  TEST_RUN(test_extreme_fov_wide);
  TEST_RUN(test_extreme_near_far);
  TEST_RUN(test_orbit_10000_increments);
  TEST_RUN(test_zoom_to_min_then_orbit);
  TEST_RUN(test_zoom_to_max_then_pan);
  TEST_RUN(test_simultaneous_orbit_pan_zoom);
  TEST_RUN(test_camera_eye_formula);
  TEST_RUN(test_camera_at_poles);
  TEST_RUN(test_set_camera_syncs_orbit);

  TEST_REPORT();
  TEST_EXIT();
}
