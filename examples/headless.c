/*
 * Master of Puppets — Example: Rotating Cube
 *
 * Demonstrates:
 *   - Viewport creation with CPU backend
 *   - Adding a colored cube mesh
 *   - Camera setup
 *   - Rendering a rotating animation (frame sequence)
 *   - Picking an object by pixel coordinates
 *   - Viewport resize
 *   - Clean shutdown with no leaks
 *
 * Output: writes a single frame to "frame.ppm" (Netpbm P6 format).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <mop/mop.h>
#include <stdio.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Cube geometry — 8 vertices, 12 triangles (36 indices)
 *
 * Each face has a distinct color for visual clarity.
 * Normals point outward from each face.
 * ------------------------------------------------------------------------- */

static const MopVertex CUBE_VERTICES[] = {
    /* Front face (Z+) — red */
    {.position = {-0.5f, -0.5f, 0.5f},
     .normal = {0.0f, 0.0f, 1.0f},
     .color = {0.9f, 0.2f, 0.2f, 1.0f}},
    {.position = {0.5f, -0.5f, 0.5f},
     .normal = {0.0f, 0.0f, 1.0f},
     .color = {0.9f, 0.2f, 0.2f, 1.0f}},
    {.position = {0.5f, 0.5f, 0.5f},
     .normal = {0.0f, 0.0f, 1.0f},
     .color = {0.9f, 0.2f, 0.2f, 1.0f}},
    {.position = {-0.5f, 0.5f, 0.5f},
     .normal = {0.0f, 0.0f, 1.0f},
     .color = {0.9f, 0.2f, 0.2f, 1.0f}},

    /* Back face (Z-) — green */
    {.position = {0.5f, -0.5f, -0.5f},
     .normal = {0.0f, 0.0f, -1.0f},
     .color = {0.2f, 0.9f, 0.2f, 1.0f}},
    {.position = {-0.5f, -0.5f, -0.5f},
     .normal = {0.0f, 0.0f, -1.0f},
     .color = {0.2f, 0.9f, 0.2f, 1.0f}},
    {.position = {-0.5f, 0.5f, -0.5f},
     .normal = {0.0f, 0.0f, -1.0f},
     .color = {0.2f, 0.9f, 0.2f, 1.0f}},
    {.position = {0.5f, 0.5f, -0.5f},
     .normal = {0.0f, 0.0f, -1.0f},
     .color = {0.2f, 0.9f, 0.2f, 1.0f}},

    /* Top face (Y+) — blue */
    {.position = {-0.5f, 0.5f, 0.5f},
     .normal = {0.0f, 1.0f, 0.0f},
     .color = {0.2f, 0.2f, 0.9f, 1.0f}},
    {.position = {0.5f, 0.5f, 0.5f},
     .normal = {0.0f, 1.0f, 0.0f},
     .color = {0.2f, 0.2f, 0.9f, 1.0f}},
    {.position = {0.5f, 0.5f, -0.5f},
     .normal = {0.0f, 1.0f, 0.0f},
     .color = {0.2f, 0.2f, 0.9f, 1.0f}},
    {.position = {-0.5f, 0.5f, -0.5f},
     .normal = {0.0f, 1.0f, 0.0f},
     .color = {0.2f, 0.2f, 0.9f, 1.0f}},

    /* Bottom face (Y-) — yellow */
    {.position = {-0.5f, -0.5f, -0.5f},
     .normal = {0.0f, -1.0f, 0.0f},
     .color = {0.9f, 0.9f, 0.2f, 1.0f}},
    {.position = {0.5f, -0.5f, -0.5f},
     .normal = {0.0f, -1.0f, 0.0f},
     .color = {0.9f, 0.9f, 0.2f, 1.0f}},
    {.position = {0.5f, -0.5f, 0.5f},
     .normal = {0.0f, -1.0f, 0.0f},
     .color = {0.9f, 0.9f, 0.2f, 1.0f}},
    {.position = {-0.5f, -0.5f, 0.5f},
     .normal = {0.0f, -1.0f, 0.0f},
     .color = {0.9f, 0.9f, 0.2f, 1.0f}},

    /* Right face (X+) — cyan */
    {.position = {0.5f, -0.5f, 0.5f},
     .normal = {1.0f, 0.0f, 0.0f},
     .color = {0.2f, 0.9f, 0.9f, 1.0f}},
    {.position = {0.5f, -0.5f, -0.5f},
     .normal = {1.0f, 0.0f, 0.0f},
     .color = {0.2f, 0.9f, 0.9f, 1.0f}},
    {.position = {0.5f, 0.5f, -0.5f},
     .normal = {1.0f, 0.0f, 0.0f},
     .color = {0.2f, 0.9f, 0.9f, 1.0f}},
    {.position = {0.5f, 0.5f, 0.5f},
     .normal = {1.0f, 0.0f, 0.0f},
     .color = {0.2f, 0.9f, 0.9f, 1.0f}},

    /* Left face (X-) — magenta */
    {.position = {-0.5f, -0.5f, -0.5f},
     .normal = {-1.0f, 0.0f, 0.0f},
     .color = {0.9f, 0.2f, 0.9f, 1.0f}},
    {.position = {-0.5f, -0.5f, 0.5f},
     .normal = {-1.0f, 0.0f, 0.0f},
     .color = {0.9f, 0.2f, 0.9f, 1.0f}},
    {.position = {-0.5f, 0.5f, 0.5f},
     .normal = {-1.0f, 0.0f, 0.0f},
     .color = {0.9f, 0.2f, 0.9f, 1.0f}},
    {.position = {-0.5f, 0.5f, -0.5f},
     .normal = {-1.0f, 0.0f, 0.0f},
     .color = {0.9f, 0.2f, 0.9f, 1.0f}},
};

static const uint32_t CUBE_INDICES[] = {
    0,  1,  2,  2,  3,  0,  /* Front  */
    4,  5,  6,  6,  7,  4,  /* Back   */
    8,  9,  10, 10, 11, 8,  /* Top    */
    12, 13, 14, 14, 15, 12, /* Bottom */
    16, 17, 18, 18, 19, 16, /* Right  */
    20, 21, 22, 22, 23, 20, /* Left   */
};

#define CUBE_VERTEX_COUNT (sizeof(CUBE_VERTICES) / sizeof(CUBE_VERTICES[0]))
#define CUBE_INDEX_COUNT (sizeof(CUBE_INDICES) / sizeof(CUBE_INDICES[0]))

/* -------------------------------------------------------------------------
 * Write framebuffer to PPM file
 * ------------------------------------------------------------------------- */

static bool write_ppm(const char *path, const uint8_t *pixels, int width,
                      int height) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;

  fprintf(f, "P6\n%d %d\n255\n", width, height);
  for (int i = 0; i < width * height; i++) {
    fwrite(&pixels[i * 4], 1, 3, f); /* Write RGB, skip A */
  }

  fclose(f);
  return true;
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  printf("Master of Puppets — Rotating Cube Example\n");
  printf("Backend: %s\n", mop_backend_name(MOP_BACKEND_CPU));

  /* Create viewport with CPU backend */
  MopViewportDesc desc = {
      .width = 800, .height = 600, .backend = MOP_BACKEND_CPU};

  MopViewport *viewport = mop_viewport_create(&desc);
  if (!viewport) {
    fprintf(stderr, "Failed to create viewport\n");
    return 1;
  }

  printf("Viewport created: %dx%d\n", desc.width, desc.height);

  /* Set camera */
  mop_viewport_set_camera(viewport, (MopVec3){2.0f, 2.0f, 3.0f}, /* eye    */
                          (MopVec3){0.0f, 0.0f, 0.0f},           /* target */
                          (MopVec3){0.0f, 1.0f, 0.0f},           /* up     */
                          60.0f,                                 /* fov    */
                          0.1f,                                  /* near   */
                          50.0f                                  /* far    */
  );

  /* Set clear color */
  mop_viewport_set_clear_color(viewport, (MopColor){0.15f, 0.15f, 0.2f, 1.0f});

  /* Add cube mesh with object_id = 1 */
  MopMeshDesc mesh_desc = {.vertices = CUBE_VERTICES,
                           .vertex_count = CUBE_VERTEX_COUNT,
                           .indices = CUBE_INDICES,
                           .index_count = CUBE_INDEX_COUNT,
                           .object_id = 1};

  MopMesh *cube = mop_viewport_add_mesh(viewport, &mesh_desc);
  if (!cube) {
    fprintf(stderr, "Failed to add cube mesh\n");
    mop_viewport_destroy(viewport);
    return 1;
  }

  printf("Cube mesh added (object_id = 1)\n");

  /* Render multiple rotation frames */
  int frame_count = 60;
  printf("Rendering %d frames...\n", frame_count);

  for (int frame = 0; frame < frame_count; frame++) {
    float angle = (float)frame * (6.28318530718f / (float)frame_count);
    MopMat4 rotation = mop_mat4_rotate_y(angle);
    mop_mesh_set_transform(cube, &rotation);
    mop_viewport_render(viewport);
  }

  /* Write the last frame to disk */
  int fb_w, fb_h;
  const uint8_t *pixels = mop_viewport_read_color(viewport, &fb_w, &fb_h);
  if (pixels && write_ppm("frame.ppm", pixels, fb_w, fb_h)) {
    printf("Written frame.ppm (%dx%d)\n", fb_w, fb_h);
  }

  /* Demonstrate picking at the center of the viewport */
  MopPickResult pick = mop_viewport_pick(viewport, 400, 300);
  if (pick.hit) {
    printf("Pick at (400,300): object_id=%u, depth=%.4f\n", pick.object_id,
           pick.depth);
  } else {
    printf("Pick at (400,300): no hit (background)\n");
  }

  /* Demonstrate viewport resize */
  printf("Resizing viewport to 1024x768...\n");
  mop_viewport_resize(viewport, 1024, 768);

  /* Re-render after resize */
  mop_viewport_render(viewport);
  pixels = mop_viewport_read_color(viewport, &fb_w, &fb_h);
  if (pixels && write_ppm("frame_resized.ppm", pixels, fb_w, fb_h)) {
    printf("Written frame_resized.ppm (%dx%d)\n", fb_w, fb_h);
  }

  /* Demonstrate wireframe mode */
  printf("Switching to wireframe mode...\n");
  mop_viewport_set_render_mode(viewport, MOP_RENDER_WIREFRAME);
  mop_viewport_render(viewport);
  pixels = mop_viewport_read_color(viewport, &fb_w, &fb_h);
  if (pixels && write_ppm("frame_wireframe.ppm", pixels, fb_w, fb_h)) {
    printf("Written frame_wireframe.ppm (%dx%d)\n", fb_w, fb_h);
  }

  /* Clean shutdown */
  mop_viewport_remove_mesh(viewport, cube);
  mop_viewport_destroy(viewport);

  printf("Clean shutdown complete.\n");
  return 0;
}
