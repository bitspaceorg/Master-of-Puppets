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
 * ------------------------------------------------------------------------- */

static int mop_tests_run = 0;
static int mop_tests_passed = 0;
static int mop_tests_failed = 0;

/* Per-test failure tracking */
static int mop_current_test_failed = 0;

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

#endif /* MOP_TEST_HARNESS_H */
