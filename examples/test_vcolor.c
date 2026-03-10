/*
 * Headless vertex color diagnostic — isolate which mesh gets wrong colors.
 * Renders 4 separate frames, each with a single cube.
 */
#include "common/geometry.h"
#include <mop/export/image_export.h>
#include <mop/mop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill_colored_cube(MopVertex *out, MopColor c) {
  memcpy(out, CUBE_VERTICES, sizeof(MopVertex) * CUBE_VERTEX_COUNT);
  for (int i = 0; i < CUBE_VERTEX_COUNT; i++)
    out[i].color = c;
}

static void setup_vp(MopViewport *vp) {
  mop_viewport_set_chrome(vp, false);
  mop_viewport_set_camera(vp, (MopVec3){3, 2, 3}, (MopVec3){0, 0.5f, 0},
                          (MopVec3){0, 1, 0}, 50.0f, 0.1f, 100.0f);
  mop_viewport_clear_lights(vp);
  mop_viewport_set_ambient(vp, 0.2f);
  mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_DIRECTIONAL,
                                         .direction = {-0.4f, -0.8f, -0.3f},
                                         .color = {1, 1, 1, 1},
                                         .intensity = 5.0f,
                                         .active = true});
  mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);
}

static void check_center(MopViewport *vp, const char *label) {
  int w, h;
  const uint8_t *px = mop_viewport_read_color(vp, &w, &h);
  if (!px)
    return;
  int cx = w / 2, cy = h / 2;
  int i = (cy * w + cx) * 4;
  printf("  %s center: R=%d G=%d B=%d\n", label, px[i], px[i + 1], px[i + 2]);
}

int main(void) {
  MopBackendType backend = MOP_BACKEND_CPU;
  const char *env = getenv("MOP_BACKEND");
  if (env && strcmp(env, "vulkan") == 0)
    backend = MOP_BACKEND_VULKAN;
  printf("Backend: %s\n", env ? env : "cpu");

  MopVertex cv[CUBE_VERTEX_COUNT];
  char path[256];

  /* Test: create viewport, add ONE cube, render, destroy viewport.
   * Repeat for each color. This tests if the issue is per-viewport or
   * cumulative across multiple add_mesh calls. */

  static const struct {
    const char *name;
    MopColor col;
  } tests[] = {
      {"stone", {0.4f, 0.38f, 0.35f, 1.0f}},
      {"obsidian", {0.1f, 0.1f, 0.12f, 1.0f}},
      {"gold", {0.85f, 0.65f, 0.2f, 1.0f}},
      {"red", {1.0f, 0.0f, 0.0f, 1.0f}},
  };

  printf("\n=== ISOLATED: one cube per viewport ===\n");
  for (int t = 0; t < 4; t++) {
    MopViewport *vp = mop_viewport_create(
        &(MopViewportDesc){.width = 256, .height = 256, .backend = backend});
    setup_vp(vp);
    fill_colored_cube(cv, tests[t].col);
    MopMesh *m = mop_viewport_add_mesh(
        vp, &(MopMeshDesc){.vertices = cv,
                           .vertex_count = CUBE_VERTEX_COUNT,
                           .indices = CUBE_INDICES,
                           .index_count = CUBE_INDEX_COUNT,
                           .object_id = 0});
    mop_mesh_set_material(m, &(MopMaterial){.base_color = tests[t].col,
                                            .metallic = 0.5f,
                                            .roughness = 0.5f});
    mop_viewport_render(vp);
    check_center(vp, tests[t].name);
    snprintf(path, sizeof(path), "/tmp/mop_isolated_%s.png", tests[t].name);
    mop_export_png(vp, path);
    mop_viewport_destroy(vp);
  }

  printf("\n=== CUMULATIVE: all cubes in ONE viewport (dark_sanctum pattern) "
         "===\n");
  {
    MopViewport *vp = mop_viewport_create(
        &(MopViewportDesc){.width = 512, .height = 256, .backend = backend});
    setup_vp(vp);
    mop_viewport_set_camera(vp, (MopVec3){0, 4, 8}, (MopVec3){0, 0.5f, 0},
                            (MopVec3){0, 1, 0}, 50.0f, 0.1f, 100.0f);

    for (int t = 0; t < 4; t++) {
      fill_colored_cube(cv, tests[t].col);
      /* Verify cv has correct colors */
      printf("  Before add_mesh[%d] (%s): cv[0]={%.2f,%.2f,%.2f} "
             "cv[8]={%.2f,%.2f,%.2f}\n",
             t, tests[t].name, cv[0].color.r, cv[0].color.g, cv[0].color.b,
             cv[8].color.r, cv[8].color.g, cv[8].color.b);

      MopMesh *m = mop_viewport_add_mesh(
          vp, &(MopMeshDesc){.vertices = cv,
                             .vertex_count = CUBE_VERTEX_COUNT,
                             .indices = CUBE_INDICES,
                             .index_count = CUBE_INDEX_COUNT,
                             .object_id = 0});
      float x = (float)(t - 1.5f) * 2.5f;
      mop_mesh_set_position(m, (MopVec3){x, 0.5f, 0});
      mop_mesh_set_material(m, &(MopMaterial){.base_color = tests[t].col,
                                              .metallic = 0.5f,
                                              .roughness = 0.5f});
    }

    mop_viewport_render(vp);
    mop_export_png(vp, "/tmp/mop_cumulative.png");
    printf("  Saved /tmp/mop_cumulative.png\n");

    /* Sample at each cube's approximate position */
    int w, h;
    const uint8_t *px = mop_viewport_read_color(vp, &w, &h);
    if (px) {
      for (int t = 0; t < 4; t++) {
        int sx = (int)((float)(t + 0.5f) / 4.0f * (float)w);
        int sy = h / 2;
        int si = (sy * w + sx) * 4;
        printf("  %s pixel @(%d,%d): R=%d G=%d B=%d\n", tests[t].name, sx, sy,
               px[si], px[si + 1], px[si + 2]);
      }
    }

    mop_viewport_destroy(vp);
  }

  printf("\n=== SEPARATE ARRAYS: each cube gets its own vertex array ===\n");
  {
    MopViewport *vp = mop_viewport_create(
        &(MopViewportDesc){.width = 512, .height = 256, .backend = backend});
    setup_vp(vp);
    mop_viewport_set_camera(vp, (MopVec3){0, 4, 8}, (MopVec3){0, 0.5f, 0},
                            (MopVec3){0, 1, 0}, 50.0f, 0.1f, 100.0f);

    /* Use SEPARATE vertex arrays — no buffer reuse */
    MopVertex v0[CUBE_VERTEX_COUNT], v1[CUBE_VERTEX_COUNT],
        v2[CUBE_VERTEX_COUNT], v3[CUBE_VERTEX_COUNT];
    MopVertex *arrays[] = {v0, v1, v2, v3};

    for (int t = 0; t < 4; t++) {
      fill_colored_cube(arrays[t], tests[t].col);
      MopMesh *m = mop_viewport_add_mesh(
          vp, &(MopMeshDesc){.vertices = arrays[t],
                             .vertex_count = CUBE_VERTEX_COUNT,
                             .indices = CUBE_INDICES,
                             .index_count = CUBE_INDEX_COUNT,
                             .object_id = 0});
      float x = (float)(t - 1.5f) * 2.5f;
      mop_mesh_set_position(m, (MopVec3){x, 0.5f, 0});
      mop_mesh_set_material(m, &(MopMaterial){.base_color = tests[t].col,
                                              .metallic = 0.5f,
                                              .roughness = 0.5f});
    }

    mop_viewport_render(vp);
    mop_export_png(vp, "/tmp/mop_separate.png");
    printf("  Saved /tmp/mop_separate.png\n");
    mop_viewport_destroy(vp);
  }

  printf("\nDone. Compare /tmp/mop_cumulative.png vs /tmp/mop_separate.png\n");

  /* ================================================================
   * POST-PROCESSING TEST: reproduce dark_sanctum post-process issue
   * ================================================================ */
  printf("\n=== POST-PROCESSING: bright light + bloom + tonemap + SSAO ===\n");
  {
    MopViewport *vp = mop_viewport_create(
        &(MopViewportDesc){.width = 512, .height = 512, .backend = backend});
    mop_viewport_set_chrome(vp, false);
    mop_viewport_set_camera(vp, (MopVec3){3, 2, 3}, (MopVec3){0, 0.5f, 0},
                            (MopVec3){0, 1, 0}, 50.0f, 0.1f, 100.0f);
    mop_viewport_clear_lights(vp);
    mop_viewport_set_ambient(vp, 0.2f);

    /* Bright point light near the cube — similar to dark_sanctum torches */
    mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_POINT,
                                           .position = {1.0f, 2.0f, 1.0f},
                                           .color = {1, 0.6f, 0.3f, 1},
                                           .intensity = 200.0f,
                                           .active = true});
    mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_DIRECTIONAL,
                                           .direction = {-0.4f, -0.8f, -0.3f},
                                           .color = {0.4f, 0.4f, 0.6f, 1},
                                           .intensity = 8.0f,
                                           .active = true});

    mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);

    /* Enable post-processing: tonemap + bloom + SSAO */
    mop_viewport_set_post_effects(vp, MOP_POST_TONEMAP | MOP_POST_BLOOM |
                                          MOP_POST_SSAO);
    mop_viewport_set_bloom(vp, 0.8f, 0.6f);
    mop_viewport_set_exposure(vp, 1.5f);

    /* Add a red cube */
    fill_colored_cube(cv, (MopColor){1.0f, 0.0f, 0.0f, 1.0f});
    MopMesh *m = mop_viewport_add_mesh(
        vp, &(MopMeshDesc){.vertices = cv,
                           .vertex_count = CUBE_VERTEX_COUNT,
                           .indices = CUBE_INDICES,
                           .index_count = CUBE_INDEX_COUNT,
                           .object_id = 0});
    mop_mesh_set_material(m,
                          &(MopMaterial){.base_color = {1.0f, 0.0f, 0.0f, 1.0f},
                                         .metallic = 0.5f,
                                         .roughness = 0.5f});

    mop_viewport_render(vp);

    int w, h;
    const uint8_t *px = mop_viewport_read_color(vp, &w, &h);
    if (px) {
      int cx = w / 2, cy = h / 2;
      int ci = (cy * w + cx) * 4;
      printf("  WITH postprocess center: R=%d G=%d B=%d A=%d\n", px[ci],
             px[ci + 1], px[ci + 2], px[ci + 3]);
      /* Sample a few positions to check for green artifacts */
      for (int sy = 0; sy < h; sy += h / 4) {
        for (int sx = 0; sx < w; sx += w / 4) {
          int si = (sy * w + sx) * 4;
          printf("    @(%3d,%3d): R=%3d G=%3d B=%3d\n", sx, sy, px[si],
                 px[si + 1], px[si + 2]);
        }
      }
    }
    mop_export_png(vp, "/tmp/mop_postprocess.png");
    printf("  Saved /tmp/mop_postprocess.png\n");
    mop_viewport_destroy(vp);
  }

  /* Test: SSAO only (no bloom) */
  printf("\n=== SSAO ONLY (no bloom): ===\n");
  {
    MopViewport *vp = mop_viewport_create(
        &(MopViewportDesc){.width = 512, .height = 512, .backend = backend});
    mop_viewport_set_chrome(vp, false);
    mop_viewport_set_camera(vp, (MopVec3){3, 2, 3}, (MopVec3){0, 0.5f, 0},
                            (MopVec3){0, 1, 0}, 50.0f, 0.1f, 100.0f);
    mop_viewport_clear_lights(vp);
    mop_viewport_set_ambient(vp, 0.2f);
    mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_POINT,
                                           .position = {1.0f, 2.0f, 1.0f},
                                           .color = {1, 0.6f, 0.3f, 1},
                                           .intensity = 200.0f,
                                           .active = true});
    mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_DIRECTIONAL,
                                           .direction = {-0.4f, -0.8f, -0.3f},
                                           .color = {0.4f, 0.4f, 0.6f, 1},
                                           .intensity = 8.0f,
                                           .active = true});
    mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);
    mop_viewport_set_post_effects(vp, MOP_POST_TONEMAP | MOP_POST_SSAO);
    mop_viewport_set_exposure(vp, 1.5f);
    fill_colored_cube(cv, (MopColor){1.0f, 0.0f, 0.0f, 1.0f});
    MopMesh *m = mop_viewport_add_mesh(
        vp, &(MopMeshDesc){.vertices = cv,
                           .vertex_count = CUBE_VERTEX_COUNT,
                           .indices = CUBE_INDICES,
                           .index_count = CUBE_INDEX_COUNT,
                           .object_id = 0});
    mop_mesh_set_material(m,
                          &(MopMaterial){.base_color = {1.0f, 0.0f, 0.0f, 1.0f},
                                         .metallic = 0.5f,
                                         .roughness = 0.5f});
    mop_viewport_render(vp);
    int w, h;
    const uint8_t *px = mop_viewport_read_color(vp, &w, &h);
    if (px) {
      int ci = (h / 2 * w + w / 2) * 4;
      printf("  SSAO-only center: R=%d G=%d B=%d\n", px[ci], px[ci + 1],
             px[ci + 2]);
    }
    mop_export_png(vp, "/tmp/mop_ssao_only.png");
    mop_viewport_destroy(vp);
  }

  /* Test: BLOOM only (no SSAO) */
  printf("\n=== BLOOM ONLY (no SSAO): ===\n");
  {
    MopViewport *vp = mop_viewport_create(
        &(MopViewportDesc){.width = 512, .height = 512, .backend = backend});
    mop_viewport_set_chrome(vp, false);
    mop_viewport_set_camera(vp, (MopVec3){3, 2, 3}, (MopVec3){0, 0.5f, 0},
                            (MopVec3){0, 1, 0}, 50.0f, 0.1f, 100.0f);
    mop_viewport_clear_lights(vp);
    mop_viewport_set_ambient(vp, 0.2f);
    mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_POINT,
                                           .position = {1.0f, 2.0f, 1.0f},
                                           .color = {1, 0.6f, 0.3f, 1},
                                           .intensity = 200.0f,
                                           .active = true});
    mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_DIRECTIONAL,
                                           .direction = {-0.4f, -0.8f, -0.3f},
                                           .color = {0.4f, 0.4f, 0.6f, 1},
                                           .intensity = 8.0f,
                                           .active = true});
    mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);
    mop_viewport_set_post_effects(vp, MOP_POST_TONEMAP | MOP_POST_BLOOM);
    mop_viewport_set_bloom(vp, 0.8f, 0.6f);
    mop_viewport_set_exposure(vp, 1.5f);
    fill_colored_cube(cv, (MopColor){1.0f, 0.0f, 0.0f, 1.0f});
    MopMesh *m = mop_viewport_add_mesh(
        vp, &(MopMeshDesc){.vertices = cv,
                           .vertex_count = CUBE_VERTEX_COUNT,
                           .indices = CUBE_INDICES,
                           .index_count = CUBE_INDEX_COUNT,
                           .object_id = 0});
    mop_mesh_set_material(m,
                          &(MopMaterial){.base_color = {1.0f, 0.0f, 0.0f, 1.0f},
                                         .metallic = 0.5f,
                                         .roughness = 0.5f});
    mop_viewport_render(vp);
    int w, h;
    const uint8_t *px = mop_viewport_read_color(vp, &w, &h);
    if (px) {
      int ci = (h / 2 * w + w / 2) * 4;
      printf("  BLOOM-only center: R=%d G=%d B=%d\n", px[ci], px[ci + 1],
             px[ci + 2]);
    }
    mop_export_png(vp, "/tmp/mop_bloom_only.png");
    mop_viewport_destroy(vp);
  }

  /* Same test WITHOUT post-processing for comparison */
  printf("\n=== NO POST-PROCESSING: same bright light ===\n");
  {
    MopViewport *vp = mop_viewport_create(
        &(MopViewportDesc){.width = 512, .height = 512, .backend = backend});
    mop_viewport_set_chrome(vp, false);
    mop_viewport_set_camera(vp, (MopVec3){3, 2, 3}, (MopVec3){0, 0.5f, 0},
                            (MopVec3){0, 1, 0}, 50.0f, 0.1f, 100.0f);
    mop_viewport_clear_lights(vp);
    mop_viewport_set_ambient(vp, 0.2f);
    mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_POINT,
                                           .position = {1.0f, 2.0f, 1.0f},
                                           .color = {1, 0.6f, 0.3f, 1},
                                           .intensity = 200.0f,
                                           .active = true});
    mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_DIRECTIONAL,
                                           .direction = {-0.4f, -0.8f, -0.3f},
                                           .color = {0.4f, 0.4f, 0.6f, 1},
                                           .intensity = 8.0f,
                                           .active = true});
    mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);

    fill_colored_cube(cv, (MopColor){1.0f, 0.0f, 0.0f, 1.0f});
    MopMesh *m = mop_viewport_add_mesh(
        vp, &(MopMeshDesc){.vertices = cv,
                           .vertex_count = CUBE_VERTEX_COUNT,
                           .indices = CUBE_INDICES,
                           .index_count = CUBE_INDEX_COUNT,
                           .object_id = 0});
    mop_mesh_set_material(m,
                          &(MopMaterial){.base_color = {1.0f, 0.0f, 0.0f, 1.0f},
                                         .metallic = 0.5f,
                                         .roughness = 0.5f});

    mop_viewport_render(vp);

    int w, h;
    const uint8_t *px = mop_viewport_read_color(vp, &w, &h);
    if (px) {
      int cx = w / 2, cy = h / 2;
      int ci = (cy * w + cx) * 4;
      printf("  WITHOUT postprocess center: R=%d G=%d B=%d A=%d\n", px[ci],
             px[ci + 1], px[ci + 2], px[ci + 3]);
    }
    mop_export_png(vp, "/tmp/mop_no_postprocess.png");
    printf("  Saved /tmp/mop_no_postprocess.png\n");
    mop_viewport_destroy(vp);
  }

  return 0;
}
