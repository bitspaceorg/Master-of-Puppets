/*
 * Master of Puppets — Test Harness
 * test_harness.h — Minimal test framework, no external deps
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_TEST_HARNESS_H
#define MOP_TEST_HARNESS_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * ANSI color codes
 * ------------------------------------------------------------------------- */

#define MOP_TEST_RED "\033[1;31m"
#define MOP_TEST_GREEN "\033[1;32m"
#define MOP_TEST_YELLOW "\033[1;33m"
#define MOP_TEST_RESET "\033[0m"

/* -------------------------------------------------------------------------
 * Global test state
 *
 * Marked unused so CPU-only builds of conditionally-compiled Vulkan tests
 * (main() is a printf stub when MOP_HAS_VULKAN is undefined) don't trip
 * -Werror=unused-variable on gcc.  Both gcc and clang accept the
 * attribute; tests that actually use these still benefit from dead-store
 * warnings unrelated to the declaration itself.
 * ------------------------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#define MOP_TEST_UNUSED __attribute__((unused))
#else
#define MOP_TEST_UNUSED
#endif

static int mop_tests_run MOP_TEST_UNUSED = 0;
static int mop_tests_passed MOP_TEST_UNUSED = 0;
static int mop_tests_failed MOP_TEST_UNUSED = 0;

/* Per-test failure tracking */
static int mop_current_test_failed MOP_TEST_UNUSED = 0;

/* -------------------------------------------------------------------------
 * Core macros
 * ------------------------------------------------------------------------- */

#define TEST_BEGIN(name)                                                       \
  do {                                                                         \
    mop_tests_run++;                                                           \
    mop_current_test_failed = 0;                                               \
    const char *_test_name = (name);                                           \
    (void)_test_name;

#define TEST_END()                                                             \
  if (!mop_current_test_failed) {                                              \
    mop_tests_passed++;                                                        \
    printf(MOP_TEST_GREEN "  PASS" MOP_TEST_RESET " %s\n", _test_name);        \
  }                                                                            \
  }                                                                            \
  while (0)

#define TEST_ASSERT(expr)                                                      \
  do {                                                                         \
    if (!(expr)) {                                                             \
      mop_tests_failed++;                                                      \
      mop_current_test_failed = 1;                                             \
      printf(MOP_TEST_RED "  FAIL" MOP_TEST_RESET " %s\n"                      \
                          "       %s:%d: %s\n",                                \
             _test_name, __FILE__, __LINE__, #expr);                           \
      break;                                                                   \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_MSG(expr, msg)                                             \
  do {                                                                         \
    if (!(expr)) {                                                             \
      mop_tests_failed++;                                                      \
      mop_current_test_failed = 1;                                             \
      printf(MOP_TEST_RED "  FAIL" MOP_TEST_RESET " %s\n"                      \
                          "       %s:%d: %s (%s)\n",                           \
             _test_name, __FILE__, __LINE__, #expr, (msg));                    \
      break;                                                                   \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_FLOAT_EQ(a, b)                                             \
  do {                                                                         \
    float _a = (a), _b = (b);                                                  \
    if (fabsf(_a - _b) > 1e-4f) {                                              \
      mop_tests_failed++;                                                      \
      mop_current_test_failed = 1;                                             \
      printf(MOP_TEST_RED "  FAIL" MOP_TEST_RESET " %s\n"                      \
                          "       %s:%d: %.6f != %.6f\n",                      \
             _test_name, __FILE__, __LINE__, (double)_a, (double)_b);          \
      break;                                                                   \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_VEC3_EQ(v, ex, ey, ez)                                     \
  do {                                                                         \
    MopVec3 _v = (v);                                                          \
    if (fabsf(_v.x - (ex)) > 1e-4f || fabsf(_v.y - (ey)) > 1e-4f ||            \
        fabsf(_v.z - (ez)) > 1e-4f) {                                          \
      mop_tests_failed++;                                                      \
      mop_current_test_failed = 1;                                             \
      printf(MOP_TEST_RED                                                      \
             "  FAIL" MOP_TEST_RESET " %s\n"                                   \
             "       %s:%d: (%.4f,%.4f,%.4f) != (%.4f,%.4f,%.4f)\n",           \
             _test_name, __FILE__, __LINE__, (double)_v.x, (double)_v.y,       \
             (double)_v.z, (double)(ex), (double)(ey), (double)(ez));          \
      break;                                                                   \
    }                                                                          \
  } while (0)

/* -------------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------------- */

#define TEST_RUN(fn) fn()

#define TEST_SUITE_BEGIN(name)                                                 \
  printf(MOP_TEST_YELLOW "\n[%s]" MOP_TEST_RESET "\n", (name))

#define TEST_REPORT()                                                          \
  do {                                                                         \
    printf("\n────────────────────────────────────────\n");                    \
    printf("  Total:  %d\n", mop_tests_run);                                   \
    printf("  " MOP_TEST_GREEN "Passed: %d" MOP_TEST_RESET "\n",               \
           mop_tests_passed);                                                  \
    if (mop_tests_failed > 0)                                                  \
      printf("  " MOP_TEST_RED "Failed: %d" MOP_TEST_RESET "\n",               \
             mop_tests_failed);                                                \
    printf("────────────────────────────────────────\n");                      \
  } while (0)

#define TEST_EXIT() return (mop_tests_failed > 0) ? 1 : 0

/* -------------------------------------------------------------------------
 * Extended assertions for torture tests
 * ------------------------------------------------------------------------- */

#define TEST_ASSERT_MAT4_NEAR(a, b, tol)                                       \
  do {                                                                         \
    MopMat4 _ma = (a), _mb = (b);                                              \
    int _mat_ok = 1;                                                           \
    for (int _mi = 0; _mi < 16; _mi++) {                                       \
      if (fabsf(_ma.d[_mi] - _mb.d[_mi]) > (tol)) {                            \
        _mat_ok = 0;                                                           \
        break;                                                                 \
      }                                                                        \
    }                                                                          \
    if (!_mat_ok) {                                                            \
      mop_tests_failed++;                                                      \
      mop_current_test_failed = 1;                                             \
      printf(MOP_TEST_RED "  FAIL" MOP_TEST_RESET " %s\n"                      \
                          "       %s:%d: matrices differ (tol=%.1e)\n",        \
             _test_name, __FILE__, __LINE__, (double)(tol));                   \
      break;                                                                   \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_NO_NAN(val)                                                \
  TEST_ASSERT_MSG(!isnan(val) && !isinf(val), "NaN/Inf detected")

#define TEST_ASSERT_FINITE_VEC3(v)                                             \
  do {                                                                         \
    MopVec3 _fv = (v);                                                         \
    TEST_ASSERT_MSG(!isnan(_fv.x) && !isinf(_fv.x) && !isnan(_fv.y) &&         \
                        !isinf(_fv.y) && !isnan(_fv.z) && !isinf(_fv.z),       \
                    "NaN/Inf in vec3");                                        \
  } while (0)

#endif /* MOP_TEST_HARNESS_H */
