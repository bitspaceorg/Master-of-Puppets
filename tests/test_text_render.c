/*
 * Master of Puppets — Test Suite
 * test_text_render.c — End-to-end text rasterization on the CPU backend
 *
 * Validates the slice-2 text path:
 *   1. bake a small TTF → .mfa (system-font fallback if no JBM available)
 *   2. create a CPU viewport with a known clear color
 *   3. submit a 2D text draw using mop_text_draw_2d
 *   4. render + read back
 *   5. assert: at least one pixel inside the expected glyph bbox
 *      differs from the clear color (text actually painted pixels)
 *      and outside the expected text region the framebuffer is
 *      undisturbed (text didn't leak across the whole frame).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Bake helper — copied shape from test_font.c.  Skips cleanly when
 * neither a TTF nor the bake tool is on disk.
 * ------------------------------------------------------------------------- */

static const char *find_test_ttf(void) {
  static const char *cands[] = {
      "/System/Library/Fonts/SFNSMono.ttf",
      "/System/Library/Fonts/Geneva.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
      NULL,
  };
  struct stat st;
  for (int i = 0; cands[i]; i++)
    if (stat(cands[i], &st) == 0)
      return cands[i];
  return NULL;
}

static const char *find_bake_tool(void) {
  static const char *cands[] = {
      "build/tools/mop_font_bake",
      "../build/tools/mop_font_bake",
      NULL,
  };
  struct stat st;
  for (int i = 0; cands[i]; i++)
    if (stat(cands[i], &st) == 0 && (st.st_mode & 0111))
      return cands[i];
  return NULL;
}

static const char *bake_test_atlas(void) {
  static const char *out_path = "/tmp/mop_text_render_atlas.mfa";
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
 * Scan a rectangular region for any pixel whose RGB differs from the
 * reference (the clear color, encoded to 8-bit).  Returns the count.
 * ------------------------------------------------------------------------- */

static int count_pixels_differing(const uint8_t *rgba, int w, int h, int x0,
                                  int y0, int x1, int y1, uint8_t cr,
                                  uint8_t cg, uint8_t cb) {
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 > w)
    x1 = w;
  if (y1 > h)
    y1 = h;

  int n = 0;
  for (int y = y0; y < y1; y++) {
    const uint8_t *row = rgba + (size_t)y * (size_t)w * 4u;
    for (int x = x0; x < x1; x++) {
      const uint8_t *p = row + (size_t)x * 4u;
      if (p[0] != cr || p[1] != cg || p[2] != cb)
        n++;
    }
  }
  return n;
}

/* -------------------------------------------------------------------------
 * Test
 * ------------------------------------------------------------------------- */

static void test_text_paints_pixels(const char *atlas_path) {
  TEST_BEGIN("text_paints_pixels");

  MopFont *font = mop_font_load(atlas_path);
  TEST_ASSERT(font != NULL);
  if (font) {
    const int W = 256;
    const int H = 96;
    /* Linear-space clear color; post-process is disabled below so the
     * 8-bit framebuffer is a direct multiply with no gamma. */
    MopColor clear = {0.10f, 0.10f, 0.12f, 1.0f};

    MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
        .width = W,
        .height = H,
        .backend = MOP_BACKEND_CPU,
        .ssaa_factor = 1,
    });
    TEST_ASSERT(vp != NULL);
    if (vp) {
      mop_viewport_set_clear_color(vp, clear);
      mop_viewport_set_chrome(vp, false);
      mop_viewport_set_post_effects(vp, 0);

      mop_text_draw_2d(vp, font, "MOP", 16.0f, 32.0f,
                       (MopTextStyle){
                           .color = MOP_COLOR_BONE,
                           .px_size = 24.0f,
                       });

      TEST_ASSERT(mop_viewport_render(vp) == MOP_RENDER_OK);

      int rw = 0, rh = 0;
      const uint8_t *fb = mop_viewport_read_color(vp, &rw, &rh);
      TEST_ASSERT(fb != NULL);
      TEST_ASSERT(rw == W);
      TEST_ASSERT(rh == H);

      uint8_t cr = (uint8_t)(clear.r * 255.0f + 0.5f);
      uint8_t cg = (uint8_t)(clear.g * 255.0f + 0.5f);
      uint8_t cb = (uint8_t)(clear.b * 255.0f + 0.5f);

      int painted =
          count_pixels_differing(fb, rw, rh, 12, 28, 80, 60, cr, cg, cb);
      TEST_ASSERT_MSG(painted > 50, "text didn't paint enough pixels");

      int leaked =
          count_pixels_differing(fb, rw, rh, 200, 0, rw, rh, cr, cg, cb);
      TEST_ASSERT_MSG(leaked == 0, "text leaked outside its region");

      mop_viewport_destroy(vp);
    }
    mop_font_free(font);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void) {
  TEST_SUITE_BEGIN("text_render");

  const char *atlas = bake_test_atlas();
  if (!atlas) {
    printf("  SKIP  text-render — no TTF / bake tool available\n");
    TEST_REPORT();
    TEST_EXIT();
  }

  test_text_paints_pixels(atlas);

  unlink(atlas);

  TEST_REPORT();
  TEST_EXIT();
}
