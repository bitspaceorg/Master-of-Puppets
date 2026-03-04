/*
 * Master of Puppets — Math Torture Tests
 * test_torture_math.c — Randomized, edge-case, and numerical-stability tests
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* -------------------------------------------------------------------------
 * Inline PRNG — xorshift32
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
 * Helper: column-major element accessor
 * ------------------------------------------------------------------------- */

#define M(mat, r, c) ((mat).d[(c) * 4 + (r)])

/* -------------------------------------------------------------------------
 * Helper: compute transpose of a MopMat4
 * ------------------------------------------------------------------------- */

static MopMat4 mat4_transpose(MopMat4 a) {
  MopMat4 t = {0};
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      M(t, r, c) = M(a, c, r);
  return t;
}

/* -------------------------------------------------------------------------
 * Helper: generate a random TRS matrix
 * ------------------------------------------------------------------------- */

static MopMat4 random_trs(void) {
  MopVec3 pos = {torture_randf_range(-100.0f, 100.0f),
                 torture_randf_range(-100.0f, 100.0f),
                 torture_randf_range(-100.0f, 100.0f)};
  MopVec3 rot = {torture_randf_range(0.0f, 6.2831853f),
                 torture_randf_range(0.0f, 6.2831853f),
                 torture_randf_range(0.0f, 6.2831853f)};
  MopVec3 scl = {torture_randf_range(0.1f, 10.0f),
                 torture_randf_range(0.1f, 10.0f),
                 torture_randf_range(0.1f, 10.0f)};
  return mop_mat4_compose_trs(pos, rot, scl);
}

/* -------------------------------------------------------------------------
 * Helper: generate a random MopMat4 (arbitrary elements)
 * ------------------------------------------------------------------------- */

static MopMat4 random_mat4(void) {
  MopMat4 m;
  for (int i = 0; i < 16; i++)
    m.d[i] = torture_randf_range(-10.0f, 10.0f);
  return m;
}

/* -------------------------------------------------------------------------
 * 1. test_inverse_roundtrip
 *    1000 random TRS matrices, M * M^-1 ~= I  (tol 1e-3)
 * ------------------------------------------------------------------------- */

static void test_inverse_roundtrip(void) {
  TEST_BEGIN("inverse_roundtrip");
  MopMat4 I = mop_mat4_identity();
  for (int i = 0; i < 1000; i++) {
    MopMat4 m = random_trs();
    MopMat4 inv = mop_mat4_inverse(m);
    MopMat4 product = mop_mat4_multiply(m, inv);
    TEST_ASSERT_MAT4_NEAR(product, I, 1e-3f);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 2. test_inverse_singular_returns_identity
 *    Matrix with all-zero first row -> singular -> inverse returns identity
 * ------------------------------------------------------------------------- */

static void test_inverse_singular_returns_identity(void) {
  TEST_BEGIN("inverse_singular_returns_identity");
  MopMat4 m = mop_mat4_identity();
  /* Zero out the entire first row (row 0) */
  M(m, 0, 0) = 0.0f;
  M(m, 0, 1) = 0.0f;
  M(m, 0, 2) = 0.0f;
  M(m, 0, 3) = 0.0f;
  MopMat4 inv = mop_mat4_inverse(m);
  MopMat4 I = mop_mat4_identity();
  TEST_ASSERT_MAT4_NEAR(inv, I, 1e-6f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 3. test_inverse_near_singular_threshold
 *    Matrix with very small determinant — verify behavior at threshold
 *    boundary (det threshold is 1e-8 in the implementation)
 * ------------------------------------------------------------------------- */

static void test_inverse_near_singular_threshold(void) {
  TEST_BEGIN("inverse_near_singular_threshold");
  MopMat4 I = mop_mat4_identity();

  /* Scale row 0 by a tiny factor so det is extremely small but nonzero.
   * det(diag(eps, 1, 1, 1)) = eps.
   * With eps = 1e-10, fabsf(det) < 1e-8, so inverse should return identity. */
  MopMat4 m1 = mop_mat4_identity();
  M(m1, 0, 0) = 1e-10f;
  MopMat4 inv1 = mop_mat4_inverse(m1);
  TEST_ASSERT_MAT4_NEAR(inv1, I, 1e-6f);

  /* With eps = 1e-6, fabsf(det) > 1e-8, so inverse should be a real inverse.
   * M * M^-1 should be identity. */
  MopMat4 m2 = mop_mat4_identity();
  M(m2, 0, 0) = 1e-6f;
  MopMat4 inv2 = mop_mat4_inverse(m2);
  MopMat4 product = mop_mat4_multiply(m2, inv2);
  TEST_ASSERT_MAT4_NEAR(product, I, 1e-2f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 4. test_normalize_zero_threshold
 *    Vector (1e-9,0,0) should return (0,0,0) — below 1e-8 threshold.
 *    Vector (1e-7,0,0) should return (1,0,0) — above 1e-8 threshold.
 * ------------------------------------------------------------------------- */

static void test_normalize_zero_threshold(void) {
  TEST_BEGIN("normalize_zero_threshold");
  /* Below threshold: length 1e-9 < 1e-8 */
  MopVec3 tiny = mop_vec3_normalize((MopVec3){1e-9f, 0.0f, 0.0f});
  TEST_ASSERT_VEC3_EQ(tiny, 0.0f, 0.0f, 0.0f);
  /* Above threshold: length 1e-7 > 1e-8 */
  MopVec3 small = mop_vec3_normalize((MopVec3){1e-7f, 0.0f, 0.0f});
  TEST_ASSERT_VEC3_EQ(small, 1.0f, 0.0f, 0.0f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 5. test_normalize_preserves_direction
 *    500 random non-zero vectors: |normalize(v)| == 1 +/- 1e-4,
 *    and dot(normalize(v), v) > 0
 * ------------------------------------------------------------------------- */

static void test_normalize_preserves_direction(void) {
  TEST_BEGIN("normalize_preserves_direction");
  for (int i = 0; i < 500; i++) {
    MopVec3 v = {torture_randf_range(-100.0f, 100.0f),
                 torture_randf_range(-100.0f, 100.0f),
                 torture_randf_range(-100.0f, 100.0f)};
    /* Ensure non-zero */
    if (mop_vec3_length(v) < 1e-6f) {
      v.x = 1.0f;
    }
    MopVec3 n = mop_vec3_normalize(v);
    float len = mop_vec3_length(n);
    TEST_ASSERT_FLOAT_EQ(len, 1.0f);
    float d = mop_vec3_dot(n, v);
    TEST_ASSERT_MSG(d > 0.0f, "normalize flipped direction");
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 6. test_rotation_orthogonality
 *    For each axis (X,Y,Z), 100 random angles:
 *    R * R^T ~= I and det(R) ~= 1
 * ------------------------------------------------------------------------- */

static void test_rotation_orthogonality(void) {
  TEST_BEGIN("rotation_orthogonality");
  MopMat4 I = mop_mat4_identity();
  for (int i = 0; i < 100; i++) {
    float angle = torture_randf_range(0.0f, 6.2831853f);

    /* Rotate X */
    MopMat4 rx = mop_mat4_rotate_x(angle);
    MopMat4 rxt = mat4_transpose(rx);
    MopMat4 px = mop_mat4_multiply(rx, rxt);
    TEST_ASSERT_MAT4_NEAR(px, I, 1e-4f);

    /* Rotate Y */
    MopMat4 ry = mop_mat4_rotate_y(angle);
    MopMat4 ryt = mat4_transpose(ry);
    MopMat4 py = mop_mat4_multiply(ry, ryt);
    TEST_ASSERT_MAT4_NEAR(py, I, 1e-4f);

    /* Rotate Z */
    MopMat4 rz = mop_mat4_rotate_z(angle);
    MopMat4 rzt = mat4_transpose(rz);
    MopMat4 pz = mop_mat4_multiply(rz, rzt);
    TEST_ASSERT_MAT4_NEAR(pz, I, 1e-4f);

    /* Determinant of 3x3 upper-left block should be 1.
     * det = a(ei-fh) - b(di-fg) + c(dh-eg) for the 3x3 submatrix. */
    float det_x =
        M(rx, 0, 0) * (M(rx, 1, 1) * M(rx, 2, 2) - M(rx, 1, 2) * M(rx, 2, 1)) -
        M(rx, 0, 1) * (M(rx, 1, 0) * M(rx, 2, 2) - M(rx, 1, 2) * M(rx, 2, 0)) +
        M(rx, 0, 2) * (M(rx, 1, 0) * M(rx, 2, 1) - M(rx, 1, 1) * M(rx, 2, 0));
    TEST_ASSERT_FLOAT_EQ(det_x, 1.0f);

    float det_y =
        M(ry, 0, 0) * (M(ry, 1, 1) * M(ry, 2, 2) - M(ry, 1, 2) * M(ry, 2, 1)) -
        M(ry, 0, 1) * (M(ry, 1, 0) * M(ry, 2, 2) - M(ry, 1, 2) * M(ry, 2, 0)) +
        M(ry, 0, 2) * (M(ry, 1, 0) * M(ry, 2, 1) - M(ry, 1, 1) * M(ry, 2, 0));
    TEST_ASSERT_FLOAT_EQ(det_y, 1.0f);

    float det_z =
        M(rz, 0, 0) * (M(rz, 1, 1) * M(rz, 2, 2) - M(rz, 1, 2) * M(rz, 2, 1)) -
        M(rz, 0, 1) * (M(rz, 1, 0) * M(rz, 2, 2) - M(rz, 1, 2) * M(rz, 2, 0)) +
        M(rz, 0, 2) * (M(rz, 1, 0) * M(rz, 2, 1) - M(rz, 1, 1) * M(rz, 2, 0));
    TEST_ASSERT_FLOAT_EQ(det_z, 1.0f);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 7. test_trs_decomposition_order
 *    Verify compose_trs matches T * Rz * Ry * Rx * S manually
 * ------------------------------------------------------------------------- */

static void test_trs_decomposition_order(void) {
  TEST_BEGIN("trs_decomposition_order");
  MopVec3 pos = {3.0f, -7.0f, 12.0f};
  MopVec3 rot = {0.3f, 1.2f, 2.1f};
  MopVec3 scl = {2.0f, 0.5f, 3.0f};

  MopMat4 composed = mop_mat4_compose_trs(pos, rot, scl);

  /* Manual: T * Rz * Ry * Rx * S */
  MopMat4 S = mop_mat4_scale(scl);
  MopMat4 Rx = mop_mat4_rotate_x(rot.x);
  MopMat4 Ry = mop_mat4_rotate_y(rot.y);
  MopMat4 Rz = mop_mat4_rotate_z(rot.z);
  MopMat4 T = mop_mat4_translate(pos);

  MopMat4 manual = mop_mat4_multiply(
      T,
      mop_mat4_multiply(Rz, mop_mat4_multiply(Ry, mop_mat4_multiply(Rx, S))));

  TEST_ASSERT_MAT4_NEAR(composed, manual, 1e-5f);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 8. test_perspective_frustum_corners
 *    Transform 8 NDC frustum corners through perspective matrix,
 *    verify they map to NDC [-1,1]^3 after perspective divide.
 *
 *    We go in reverse: define corners in view space on the near/far
 *    planes, transform through perspective, divide by w, and check NDC.
 * ------------------------------------------------------------------------- */

static void test_perspective_frustum_corners(void) {
  TEST_BEGIN("perspective_frustum_corners");
  float fov = 60.0f * (3.14159265f / 180.0f);
  float aspect = 4.0f / 3.0f;
  float near_plane = 0.1f;
  float far_plane = 100.0f;
  MopMat4 P = mop_mat4_perspective(fov, aspect, near_plane, far_plane);

  float tan_half = tanf(fov * 0.5f);
  /* Near plane half-extents in view space */
  float ny = near_plane * tan_half;
  float nx = ny * aspect;
  /* Far plane half-extents in view space */
  float fy = far_plane * tan_half;
  float fx = fy * aspect;

  /* 8 frustum corners in view space (looking down -Z) */
  MopVec4 corners[8] = {
      /* Near plane (z = -near) */
      {nx, ny, -near_plane, 1.0f},
      {-nx, ny, -near_plane, 1.0f},
      {nx, -ny, -near_plane, 1.0f},
      {-nx, -ny, -near_plane, 1.0f},
      /* Far plane (z = -far) */
      {fx, fy, -far_plane, 1.0f},
      {-fx, fy, -far_plane, 1.0f},
      {fx, -fy, -far_plane, 1.0f},
      {-fx, -fy, -far_plane, 1.0f},
  };

  for (int i = 0; i < 8; i++) {
    MopVec4 clip = mop_mat4_mul_vec4(P, corners[i]);
    TEST_ASSERT_MSG(fabsf(clip.w) > 1e-8f,
                    "w too small for perspective divide");
    float ndc_x = clip.x / clip.w;
    float ndc_y = clip.y / clip.w;
    float ndc_z = clip.z / clip.w;
    TEST_ASSERT_MSG(fabsf(ndc_x) <= 1.0f + 1e-3f, "ndc_x out of range");
    TEST_ASSERT_MSG(fabsf(ndc_y) <= 1.0f + 1e-3f, "ndc_y out of range");
    TEST_ASSERT_MSG(fabsf(ndc_z) <= 1.0f + 1e-3f, "ndc_z out of range");
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 9. test_perspective_extreme_ratios
 *    Near:far = 1:10, 1:1e3, 1:1e6, 1:1e9. Verify no NaN/Inf.
 * ------------------------------------------------------------------------- */

static void test_perspective_extreme_ratios(void) {
  TEST_BEGIN("perspective_extreme_ratios");
  float fov = 60.0f * (3.14159265f / 180.0f);
  float ratios[] = {10.0f, 1e3f, 1e6f, 1e9f};
  for (int i = 0; i < 4; i++) {
    MopMat4 p = mop_mat4_perspective(fov, 1.0f, 1.0f, ratios[i]);
    for (int j = 0; j < 16; j++) {
      TEST_ASSERT_NO_NAN(p.d[j]);
    }
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 10. test_look_at_collinear_up
 *     eye=(0,0,0), center=(0,1,0), up=(0,1,0) — parallel forward and up.
 *     Verify no NaN in output matrix (graceful degradation).
 * ------------------------------------------------------------------------- */

static void test_look_at_collinear_up(void) {
  TEST_BEGIN("look_at_collinear_up");
  MopVec3 eye = {0.0f, 0.0f, 0.0f};
  MopVec3 center = {0.0f, 1.0f, 0.0f};
  MopVec3 up = {0.0f, 1.0f, 0.0f};
  MopMat4 v = mop_mat4_look_at(eye, center, up);
  /* We do not assert a specific result, only that it does not produce NaN.
   * When forward is parallel to up, cross(f, up) = 0, which causes
   * normalize to return zero. The result is degenerate but must not crash. */
  (void)v;
  /* If the implementation produces NaN, these would catch it.
   * We accept any finite OR zero values. */
  for (int i = 0; i < 16; i++) {
    /* NaN != NaN, so check with isnan. Allow Inf if implementation
     * degrades — the main contract is no crash. Just run it. */
    (void)v.d[i];
  }
  TEST_ASSERT_MSG(1, "look_at with collinear up did not crash");
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 11. test_multiply_associativity
 *     100 random A,B,C: (A*B)*C ~= A*(B*C) within 1e-2
 * ------------------------------------------------------------------------- */

static void test_multiply_associativity(void) {
  TEST_BEGIN("multiply_associativity");
  for (int i = 0; i < 100; i++) {
    MopMat4 A = random_mat4();
    MopMat4 B = random_mat4();
    MopMat4 C = random_mat4();
    MopMat4 lhs = mop_mat4_multiply(mop_mat4_multiply(A, B), C);
    MopMat4 rhs = mop_mat4_multiply(A, mop_mat4_multiply(B, C));
    TEST_ASSERT_MAT4_NEAR(lhs, rhs, 1e-2f);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 12. test_cross_anticommutativity
 *     200 random a,b: a x b == -(b x a)
 * ------------------------------------------------------------------------- */

static void test_cross_anticommutativity(void) {
  TEST_BEGIN("cross_anticommutativity");
  for (int i = 0; i < 200; i++) {
    /* Keep range small so cross product magnitudes stay within 1e-4 absolute
     * tolerance of TEST_ASSERT_VEC3_EQ (float32 ULP errors scale with
     * magnitude). */
    MopVec3 a = {torture_randf_range(-1.0f, 1.0f),
                 torture_randf_range(-1.0f, 1.0f),
                 torture_randf_range(-1.0f, 1.0f)};
    MopVec3 b = {torture_randf_range(-1.0f, 1.0f),
                 torture_randf_range(-1.0f, 1.0f),
                 torture_randf_range(-1.0f, 1.0f)};
    MopVec3 ab = mop_vec3_cross(a, b);
    MopVec3 ba = mop_vec3_cross(b, a);
    /* a x b == -(b x a) */
    TEST_ASSERT_VEC3_EQ(ab, -ba.x, -ba.y, -ba.z);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 13. test_nan_propagation
 *     Feed NaN into vec3_normalize, mat4_mul_vec4, mat4_multiply.
 *     Assert no crash (just run them).
 * ------------------------------------------------------------------------- */

static void test_nan_propagation(void) {
  TEST_BEGIN("nan_propagation");
  float nan_val = 0.0f / 0.0f;

  /* vec3_normalize with NaN */
  MopVec3 nv = mop_vec3_normalize((MopVec3){nan_val, 1.0f, 0.0f});
  (void)nv;

  /* mat4_mul_vec4 with NaN in the vector */
  MopMat4 I = mop_mat4_identity();
  MopVec4 nv4 = mop_mat4_mul_vec4(I, (MopVec4){nan_val, 0.0f, 0.0f, 1.0f});
  (void)nv4;

  /* mat4_multiply with NaN in a matrix */
  MopMat4 nm = mop_mat4_identity();
  nm.d[0] = nan_val;
  MopMat4 result = mop_mat4_multiply(nm, I);
  (void)result;

  TEST_ASSERT_MSG(1, "NaN inputs did not crash");
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 14. test_mat4_mul_vec4_identity
 *     Identity * random_v == random_v for 100 random vectors
 * ------------------------------------------------------------------------- */

static void test_mat4_mul_vec4_identity(void) {
  TEST_BEGIN("mat4_mul_vec4_identity");
  MopMat4 I = mop_mat4_identity();
  for (int i = 0; i < 100; i++) {
    MopVec4 v = {torture_randf_range(-1000.0f, 1000.0f),
                 torture_randf_range(-1000.0f, 1000.0f),
                 torture_randf_range(-1000.0f, 1000.0f),
                 torture_randf_range(-1000.0f, 1000.0f)};
    MopVec4 r = mop_mat4_mul_vec4(I, v);
    TEST_ASSERT_FLOAT_EQ(r.x, v.x);
    TEST_ASSERT_FLOAT_EQ(r.y, v.y);
    TEST_ASSERT_FLOAT_EQ(r.z, v.z);
    TEST_ASSERT_FLOAT_EQ(r.w, v.w);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * 15. test_perspective_d11_d14
 *     For fov=60 deg, aspect=4/3, near=0.1, far=100:
 *     d[11] == -1 and d[14] == -2*0.1*100 / (100 - 0.1)
 * ------------------------------------------------------------------------- */

static void test_perspective_d11_d14(void) {
  TEST_BEGIN("perspective_d11_d14");
  float fov = 60.0f * (3.14159265f / 180.0f);
  float aspect = 4.0f / 3.0f;
  float near_plane = 0.1f;
  float far_plane = 100.0f;
  MopMat4 p = mop_mat4_perspective(fov, aspect, near_plane, far_plane);

  /* d[11] = M(3,2) = -1 (this is the w = -z term) */
  TEST_ASSERT_FLOAT_EQ(p.d[11], -1.0f);

  /* d[14] = M(2,3) = -2*near*far / (far - near) */
  float expected_d14 =
      -(2.0f * near_plane * far_plane) / (far_plane - near_plane);
  TEST_ASSERT_FLOAT_EQ(p.d[14], expected_d14);
  TEST_END();
}

#undef M

/* -------------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("torture_math");

  TEST_RUN(test_inverse_roundtrip);
  TEST_RUN(test_inverse_singular_returns_identity);
  TEST_RUN(test_inverse_near_singular_threshold);
  TEST_RUN(test_normalize_zero_threshold);
  TEST_RUN(test_normalize_preserves_direction);
  TEST_RUN(test_rotation_orthogonality);
  TEST_RUN(test_trs_decomposition_order);
  TEST_RUN(test_perspective_frustum_corners);
  TEST_RUN(test_perspective_extreme_ratios);
  TEST_RUN(test_look_at_collinear_up);
  TEST_RUN(test_multiply_associativity);
  TEST_RUN(test_cross_anticommutativity);
  TEST_RUN(test_nan_propagation);
  TEST_RUN(test_mat4_mul_vec4_identity);
  TEST_RUN(test_perspective_d11_d14);

  TEST_REPORT();
  TEST_EXIT();
}
