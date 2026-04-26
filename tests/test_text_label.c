/*
 * Master of Puppets — Test Suite
 * test_text_label.c — World-anchored label rasterization
 *
 * Validates the slice-3 label feature:
 *   1. add a small mesh at a known world position
 *   2. point the camera at it
 *   3. submit a label targeting the mesh
 *   4. render
 *   5. assert: text pixels appear in the projected screen region
 *      (the label landed near the mesh, not at (0,0) or off-screen)
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
 * Reuses the bake-helper trick from test_text_render.c.
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
  static const char *out_path = "/tmp/mop_text_label_atlas.mfa";
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
 * Build a 1×1×1 cube mesh at the origin so the test has a real
 * MopMesh* to anchor labels against.  Geometry is the canonical
 * 24-vertex / 36-index cube used in the existing examples.
 * ------------------------------------------------------------------------- */

static MopMesh *add_unit_cube(MopViewport *vp) {
  static const float F[6][4][3] = {
      {{1, -1, -1}, {1, -1, 1}, {1, 1, 1}, {1, 1, -1}},
      {{-1, -1, 1}, {-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1}},
      {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}},
      {{-1, -1, 1}, {1, -1, 1}, {1, -1, -1}, {-1, -1, -1}},
      {{-1, -1, 1}, {-1, 1, 1}, {1, 1, 1}, {1, -1, 1}},
      {{1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, -1}},
  };
  static const float N[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  MopVertex v[24];
  uint32_t idx[36];
  for (int f = 0; f < 6; f++) {
    for (int j = 0; j < 4; j++) {
      v[f * 4 + j].position = (MopVec3){F[f][j][0], F[f][j][1], F[f][j][2]};
      v[f * 4 + j].normal = (MopVec3){N[f][0], N[f][1], N[f][2]};
      v[f * 4 + j].color = (MopColor){0.7f, 0.7f, 0.7f, 1.0f};
      v[f * 4 + j].u = (float)(j & 1);
      v[f * 4 + j].v = (float)((j >> 1) & 1);
    }
    uint32_t b = (uint32_t)f * 4;
    idx[f * 6 + 0] = b + 0;
    idx[f * 6 + 1] = b + 2;
    idx[f * 6 + 2] = b + 1;
    idx[f * 6 + 3] = b + 0;
    idx[f * 6 + 4] = b + 3;
    idx[f * 6 + 5] = b + 2;
  }
  return mop_viewport_add_mesh(vp, &(MopMeshDesc){
                                       .vertices = v,
                                       .vertex_count = 24,
                                       .indices = idx,
                                       .index_count = 36,
                                       .object_id = 1,
                                   });
}

/* -------------------------------------------------------------------------
 * Find the brightest pixel in the framebuffer (any pixel that
 * differs significantly from the clear color) — the label's
 * rasterized text is the only source of non-scene pixels in this
 * test.  Returns the brightest pixel's screen coordinates.
 * ------------------------------------------------------------------------- */

static void brightest_text_pixel(const uint8_t *rgba, int w, int h, int *out_x,
                                 int *out_y, int *out_brightness) {
  int max_v = -1, mx = -1, my = -1;
  for (int y = 0; y < h; y++) {
    const uint8_t *row = rgba + (size_t)y * (size_t)w * 4u;
    for (int x = 0; x < w; x++) {
      const uint8_t *p = row + (size_t)x * 4u;
      int v = p[0] + p[1] + p[2];
      if (v > max_v) {
        max_v = v;
        mx = x;
        my = y;
      }
    }
  }
  *out_x = mx;
  *out_y = my;
  *out_brightness = max_v;
}

/* -------------------------------------------------------------------------
 * Test
 * ------------------------------------------------------------------------- */

static void test_label_lands_near_mesh(const char *atlas_path) {
  TEST_BEGIN("label_lands_near_mesh");

  MopFont *font = mop_font_load(atlas_path);
  TEST_ASSERT(font != NULL);
  if (font) {
    const int W = 320;
    const int H = 240;
    MopColor clear = {0.05f, 0.06f, 0.08f, 1.0f};

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
      /* Camera looks straight at the origin — cube's projected
       * screen anchor lands at (W/2, H/2). */
      mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                              (MopVec3){0, 1, 0}, 45.0f, 0.1f, 100.0f);

      MopMesh *cube = add_unit_cube(vp);
      TEST_ASSERT(cube != NULL);
      if (cube) {
        mop_text_draw_label(vp, font, cube, "cube", MOP_LABEL_TOP_CENTER,
                            MOP_LABEL_ALWAYS_ON_TOP,
                            (MopTextStyle){
                                .color = MOP_COLOR_BONE,
                                .px_size = 14.0f,
                            });

        TEST_ASSERT(mop_viewport_render(vp) == MOP_RENDER_OK);

        int rw = 0, rh = 0;
        const uint8_t *fb = mop_viewport_read_color(vp, &rw, &rh);
        TEST_ASSERT(fb != NULL);
        TEST_ASSERT(rw == W);
        TEST_ASSERT(rh == H);

        int bx = 0, by = 0, bv = 0;
        brightest_text_pixel(fb, rw, rh, &bx, &by, &bv);
        /* With camera-on-origin and the cube spanning ±1 in world
         * space, the cube's projected screen X is around W/2.  The
         * label should land *above* the cube — y < H/2.  Brightest
         * pixel should be in the upper half, near horizontal
         * center. */
        TEST_ASSERT_MSG(bv > 200, "label didn't paint bright text");
        TEST_ASSERT_MSG(by < H / 2,
                        "label rendered below mesh — anchor projection wrong");
        TEST_ASSERT_MSG(bx > W / 4 && bx < (W * 3) / 4,
                        "label rendered far from horizontal center");
      }

      mop_viewport_destroy(vp);
    }
    mop_font_free(font);
  }
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Behind-camera labels are silently dropped — no abort, no crash.
 * ------------------------------------------------------------------------- */

static void test_behind_camera_drops_label(const char *atlas_path) {
  TEST_BEGIN("label_behind_camera_dropped");

  MopFont *font = mop_font_load(atlas_path);
  TEST_ASSERT(font != NULL);
  if (font) {
    MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
        .width = 128,
        .height = 96,
        .backend = MOP_BACKEND_CPU,
        .ssaa_factor = 1,
    });
    TEST_ASSERT(vp != NULL);
    if (vp) {
      mop_viewport_set_chrome(vp, false);
      mop_viewport_set_post_effects(vp, 0);
      /* Camera at origin, looking at +Z.  Mesh placed behind the
       * camera at -Z — projection w should be <= 0, so the label
       * is dropped without crashing. */
      mop_viewport_set_camera(vp, (MopVec3){0, 0, 0}, (MopVec3){0, 0, 1},
                              (MopVec3){0, 1, 0}, 45.0f, 0.1f, 100.0f);

      MopMesh *cube = add_unit_cube(vp);
      if (cube) {
        mop_mesh_set_position(cube, (MopVec3){0, 0, -5});
        mop_text_draw_label(
            vp, font, cube, "behind", MOP_LABEL_TOP_CENTER,
            MOP_LABEL_ALWAYS_ON_TOP,
            (MopTextStyle){.color = MOP_COLOR_BONE, .px_size = 14.0f});
        TEST_ASSERT(mop_viewport_render(vp) == MOP_RENDER_OK);
        /* Sanity: render didn't fault.  We don't assert pixel
         * count here — the label being dropped means the buffer
         * matches the ordinary "render of nothing" output. */
      }
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
  TEST_SUITE_BEGIN("text_label");

  const char *atlas = bake_test_atlas();
  if (!atlas) {
    printf("  SKIP  text-label — no TTF / bake tool available\n");
    TEST_REPORT();
    TEST_EXIT();
  }

  test_label_lands_near_mesh(atlas);
  test_behind_camera_drops_label(atlas);

  unlink(atlas);

  TEST_REPORT();
  TEST_EXIT();
}
