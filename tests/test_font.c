/*
 * Master of Puppets — Test Suite
 * test_font.c — .mfa load, metrics, measure round-trip
 *
 * Bake-tool output is generated at test time using a system TTF when
 * one is available; otherwise the test skips.  This keeps the
 * checked-in repo free of font binaries while still exercising the
 * full bake → load → query path on developer machines.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/core/font.h>
#include <mop/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Locate a TTF the bake tool can consume.  We look for a few common
 * system fonts and pick the first one that exists.  If none is found
 * we MOP_TEST_SKIP — the test still validates that the build wired
 * the loader API correctly (compile + link).
 * ------------------------------------------------------------------------- */

static const char *find_test_ttf(void) {
  static const char *candidates[] = {
      "/System/Library/Fonts/SFNSMono.ttf", /* macOS, monospace      */
      "/System/Library/Fonts/Geneva.ttf",   /* macOS, sans           */
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", /* Debian   */
      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
      NULL,
  };
  struct stat st;
  for (int i = 0; candidates[i]; i++) {
    if (stat(candidates[i], &st) == 0)
      return candidates[i];
  }
  return NULL;
}

/* -------------------------------------------------------------------------
 * Locate the bake tool — either freshly built in this tree or on PATH.
 * ------------------------------------------------------------------------- */

static const char *find_bake_tool(void) {
  static const char *candidates[] = {
      "build/tools/mop_font_bake",
      "../build/tools/mop_font_bake",
      NULL,
  };
  struct stat st;
  for (int i = 0; candidates[i]; i++) {
    if (stat(candidates[i], &st) == 0 && (st.st_mode & 0111))
      return candidates[i];
  }
  return NULL;
}

/* -------------------------------------------------------------------------
 * Bake a fresh atlas for the test.  Returns the .mfa path or NULL.
 * Uses ascii glyph set, 64px source, padding 4 — same as the HUD bake.
 * ------------------------------------------------------------------------- */

static const char *bake_test_atlas(void) {
  static const char *out_path = "/tmp/mop_font_test_atlas.mfa";

  const char *tool = find_bake_tool();
  const char *ttf = find_test_ttf();
  if (!tool || !ttf)
    return NULL;

  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "%s %s --out %s --glyphs ascii --size 64 --padding 4 "
           "> /dev/null 2>&1",
           tool, ttf, out_path);
  if (system(cmd) != 0)
    return NULL;

  struct stat st;
  if (stat(out_path, &st) != 0 || st.st_size < 128)
    return NULL;
  return out_path;
}

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_load_invalid_blob(void) {
  TEST_BEGIN("font_load_invalid_blob");

  /* Too small. */
  uint8_t tiny[16] = {0};
  TEST_ASSERT(mop_font_load_memory(tiny, sizeof(tiny)) == NULL);

  /* Bad magic. */
  uint8_t header[128] = {0};
  uint32_t bad_magic = 0xDEADBEEF;
  memcpy(header, &bad_magic, sizeof(bad_magic));
  TEST_ASSERT(mop_font_load_memory(header, sizeof(header)) == NULL);

  /* NULL inputs. */
  TEST_ASSERT(mop_font_load(NULL) == NULL);
  TEST_ASSERT(mop_font_load_memory(NULL, 1024) == NULL);
  mop_font_free(NULL); /* must not crash */

  TEST_END();
}

static void test_load_baked_atlas(const char *path) {
  TEST_BEGIN("font_load_baked_atlas");

  MopFont *f = mop_font_load(path);
  TEST_ASSERT(f != NULL);
  if (f) {
    MopFontMetrics m = mop_font_metrics(f);
    /* 96 glyphs = 95 printable ASCII + 1 .notdef. */
    TEST_ASSERT(m.glyph_count == 96);
    TEST_ASSERT(m.ascent > 0.0f);
    TEST_ASSERT(m.descent < 0.0f);
    TEST_ASSERT(m.line_height > 0.0f);
    TEST_ASSERT(m.em_size == 64.0f);
    TEST_ASSERT(m.px_range == 4.0f);
    TEST_ASSERT(mop_font_atlas_type(f) == MOP_FONT_ATLAS_SDF);
    mop_font_free(f);
  }
  TEST_END();
}

static void test_measure_text(const char *path) {
  TEST_BEGIN("font_measure_text");

  MopFont *f = mop_font_load(path);
  TEST_ASSERT(f != NULL);
  if (f) {
    /* Empty string is zero. */
    TEST_ASSERT_FLOAT_EQ(mop_text_measure(f, "", 16.0f), 0.0f);

    /* Identical strings at identical sizes measure identically. */
    float a = mop_text_measure(f, "frame 16.4ms", 12.0f);
    float b = mop_text_measure(f, "frame 16.4ms", 12.0f);
    TEST_ASSERT_FLOAT_EQ(a, b);
    TEST_ASSERT(a > 0.0f);

    /* Doubling pixel size doubles the measured width. */
    float small = mop_text_measure(f, "MOP", 12.0f);
    float big = mop_text_measure(f, "MOP", 24.0f);
    TEST_ASSERT_FLOAT_EQ(big, small * 2.0f);

    /* Newlines reset the kerning state but the measure function
     * returns total width; assert it is finite and positive. */
    float multi = mop_text_measure(f, "line1\nline2", 12.0f);
    TEST_ASSERT(multi > 0.0f);

    mop_font_free(f);
  }
  TEST_END();
}

static void test_missing_glyph_does_not_wedge_layout(const char *path) {
  TEST_BEGIN("font_missing_glyph_advance");

  MopFont *f = mop_font_load(path);
  TEST_ASSERT(f != NULL);
  if (f) {
    /* CJK / box-drawing codepoints aren't in the ASCII bake but the
     * measurer must still advance — half-em fallback per glyph. */
    float w = mop_text_measure(f, "\xe4\xb8\xad\xe6\x96\x87", 16.0f);
    TEST_ASSERT(w > 0.0f);
    mop_font_free(f);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("font");

  TEST_RUN(test_load_invalid_blob);

  const char *path = bake_test_atlas();
  if (!path) {
    printf("  SKIP  bake-and-load tests — no TTF / bake tool available\n");
    TEST_REPORT();
    TEST_EXIT();
  }

  test_load_baked_atlas(path);
  test_measure_text(path);
  test_missing_glyph_does_not_wedge_layout(path);

  unlink(path);

  TEST_REPORT();
  TEST_EXIT();
}
