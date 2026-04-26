/*
 * Master of Puppets — Test Suite
 * test_text_embed.c — HUD font embed availability
 *
 * When the build was produced with `make fonts && make`, the HUD
 * font is linked directly into libmop and mop_font_hud() returns a
 * usable pointer with no disk I/O.  Without the embed it returns
 * NULL — which is the correct, defensive behavior, but uninteresting
 * to assert.  We skip if the embed wasn't present at link time.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

static void test_hud_font_metrics(void) {
  TEST_BEGIN("hud_font_metrics");
  const MopFont *hud = mop_font_hud();
  if (hud) {
    MopFontMetrics m = mop_font_metrics(hud);
    TEST_ASSERT(m.glyph_count > 90);
    TEST_ASSERT(m.ascent > 0.0f);
    TEST_ASSERT(m.descent < 0.0f);
    TEST_ASSERT(m.line_height > 0.0f);
    TEST_ASSERT(m.em_size > 0.0f);
  } else {
    printf("    skip — no embedded HUD font in this build\n");
  }
  TEST_END();
}

static void test_hud_font_measure(void) {
  TEST_BEGIN("hud_font_measure");
  const MopFont *hud = mop_font_hud();
  if (hud) {
    float w12 = mop_text_measure(hud, "frame 16ms", 12.0f);
    float w24 = mop_text_measure(hud, "frame 16ms", 24.0f);
    TEST_ASSERT(w12 > 0.0f);
    TEST_ASSERT_FLOAT_EQ(w24, w12 * 2.0f);
  } else {
    printf("    skip — no embedded HUD font in this build\n");
  }
  TEST_END();
}

int main(void) {
  TEST_SUITE_BEGIN("text_embed");
  test_hud_font_metrics();
  test_hud_font_measure();
  TEST_REPORT();
  TEST_EXIT();
}
