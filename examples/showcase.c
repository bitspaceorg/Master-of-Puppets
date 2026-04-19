/*
 * Master of Puppets — Showcase Example
 *
 * ONE comprehensive example of how to embed MOP in a host application
 * (DCC plugin, game engine viewport, custom tool). Walks through the full
 * integration path end-to-end:
 *
 *   1. Create a viewport                   — one call, a few struct fields
 *   2. Build a scene                       — meshes, lights, materials, env
 *   3. Own the output texture              — host provides a MopTexture
 *   4. Mutate from a worker thread         — scene_lock keeps render safe
 *   5. Render + present-to-texture         — one frame, one call each
 *   6. Read back + save                    — or hand the texture to a
 *                                            GPU compositor in a real host
 *
 * This file simulates a host by running a mock "host thread" that animates
 * the scene while the main thread drives the render loop. The final output
 * is a folder of PNGs — in a real embed these would be frames displayed in
 * the host's UI or fed to a compositor.
 *
 *   Usage:  nix run .#showcase                      (60 frames to
 *                                                    /tmp/mop-showcase-out/)
 *           ./build/examples/showcase [output_dir]
 *           ./build/examples/showcase --vulkan       (Vulkan backend)
 *           ./build/examples/showcase --4k           (3840x2160 output)
 *           ./build/examples/showcase --hdri PATH    (custom .hdr / .exr)
 *
 * Total integration surface: roughly 15 MOP calls. Everything else below
 * is either host-side plumbing (threading, file I/O) or procedural
 * geometry construction.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FRAME_COUNT 60
#define DEFAULT_WIDTH 960
#define DEFAULT_HEIGHT 540

/* =========================================================================
 * Procedural geometry — kept short; not the point of this example.
 * ========================================================================= */

static MopMesh *add_cube(MopViewport *vp, uint32_t object_id, MopColor color) {
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
      v[f * 4 + j].color = color;
      v[f * 4 + j].u = (float)(j & 1);
      v[f * 4 + j].v = (float)((j >> 1) & 1);
    }
    /* CCW winding viewed from outside (matches MOP convention).
     * cross(v1-v0, v2-v0) points along the +normal for each face. */
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
                                       .object_id = object_id,
                                   });
}

static MopMesh *add_sphere(MopViewport *vp, uint32_t object_id,
                           MopColor color) {
  /* Tessellation: 48 stacks × 64 slices → 5.6° per slice around the
   * equator, giving a smooth silhouette even at 4K zoom. A UV sphere's
   * silhouette is a (slices)-gon; smooth shading only affects the
   * shaded surface, not the geometric outline. */
  const int S = 48, R = 64;
  uint32_t vc = (uint32_t)((S + 1) * (R + 1));
  uint32_t ic = (uint32_t)(S * R * 6);
  MopVertex *v = calloc(vc, sizeof(MopVertex));
  uint32_t *idx = calloc(ic, sizeof(uint32_t));
  if (!v || !idx) {
    free(v);
    free(idx);
    return NULL;
  }
  uint32_t vi = 0;
  for (int i = 0; i <= S; i++) {
    float phi = (float)M_PI * (float)i / (float)S;
    float sp = sinf(phi), cp = cosf(phi);
    for (int j = 0; j <= R; j++) {
      float th = 2.0f * (float)M_PI * (float)j / (float)R;
      MopVec3 n = {sp * cosf(th), cp, sp * sinf(th)};
      v[vi].position = n;
      v[vi].normal = n;
      v[vi].color = color;
      v[vi].u = (float)j / (float)R;
      v[vi].v = (float)i / (float)S;
      vi++;
    }
  }
  /* CCW winding viewed from outside. Quad vertices: a = (i,j) "top",
   * b = (i+1,j) "bottom", a+1 = (i,j+1), b+1 = (i+1,j+1).
   * Triangulate (a, a+1, b) and (a+1, b+1, b) so cross of edges aligns
   * with the outward vertex normal. */
  uint32_t ii = 0;
  for (int i = 0; i < S; i++) {
    for (int j = 0; j < R; j++) {
      uint32_t a = (uint32_t)(i * (R + 1) + j);
      uint32_t b = a + (uint32_t)(R + 1);
      idx[ii++] = a;
      idx[ii++] = a + 1;
      idx[ii++] = b;
      idx[ii++] = a + 1;
      idx[ii++] = b + 1;
      idx[ii++] = b;
    }
  }
  MopMesh *m = mop_viewport_add_mesh(vp, &(MopMeshDesc){
                                             .vertices = v,
                                             .vertex_count = vc,
                                             .indices = idx,
                                             .index_count = ic,
                                             .object_id = object_id,
                                         });
  free(v);
  free(idx);
  return m;
}

/* =========================================================================
 * Host-side animation thread — simulates a DCC's timeline playback or a
 * game's logic thread: independently mutates scene state while MOP
 * renders on the main thread. Demonstrates that mesh handles stay valid
 * and that scene_lock keeps the two threads coherent.
 * ========================================================================= */

typedef struct HostSim {
  MopViewport *vp;
  MopMesh *cube;
  MopMesh *sphere;
  _Atomic int current_frame;
  _Atomic int running;
} HostSim;

static void *host_animate(void *arg) {
  HostSim *s = (HostSim *)arg;
  while (atomic_load(&s->running)) {
    int f = atomic_load(&s->current_frame);
    float t = (float)f * (2.0f * (float)M_PI / (float)FRAME_COUNT);

    /* Scene mutation — bracketed by scene_lock so the render thread
     * never observes partial updates. Cheap when uncontended.
     * Host owns the camera (common DCC pattern): turntable orbit
     * around the origin, full revolution over FRAME_COUNT frames. */
    float cam_radius = 6.5f;
    float cam_angle = t;
    MopVec3 cam_eye = {cosf(cam_angle) * cam_radius, 2.5f,
                       sinf(cam_angle) * cam_radius};

    mop_viewport_scene_lock(s->vp);
    mop_viewport_set_camera(s->vp, cam_eye, (MopVec3){0, 0.3f, 0},
                            (MopVec3){0, 1, 0}, 45.0f, 0.1f, 100.0f);
    mop_mesh_set_rotation(s->cube, (MopVec3){0.25f, t, 0});
    mop_mesh_set_position(s->sphere, (MopVec3){sinf(t * 1.5f) * 2.5f,
                                               0.5f + cosf(t * 2.0f) * 0.3f,
                                               cosf(t * 1.5f) * 2.5f});
    mop_viewport_scene_unlock(s->vp);

    usleep(4000); /* ~250 Hz update cadence */
  }
  return NULL;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(int argc, char **argv) {
  const char *outdir = "/tmp/mop-showcase-out";
  MopBackendType backend = MOP_BACKEND_CPU;
  int width = DEFAULT_WIDTH;
  int height = DEFAULT_HEIGHT;

  /* Default HDRI — override with --hdri PATH. Host's HOME resolves ~/. */
  char default_hdri[1024] = {0};
  const char *home = getenv("HOME");
  if (home)
    snprintf(default_hdri, sizeof(default_hdri),
             "%s/Downloads/grasslands_sunset_4k.exr", home);
  const char *hdri_path = default_hdri[0] ? default_hdri : NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--vulkan") == 0)
      backend = MOP_BACKEND_VULKAN;
    else if (strcmp(argv[i], "--cpu") == 0)
      backend = MOP_BACKEND_CPU;
    else if (strcmp(argv[i], "--4k") == 0) {
      width = 3840;
      height = 2160;
    } else if (strcmp(argv[i], "--2k") == 0) {
      width = 2560;
      height = 1440;
    } else if (strcmp(argv[i], "--1080p") == 0) {
      width = 1920;
      height = 1080;
    } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
      width = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
      height = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--hdri") == 0 && i + 1 < argc) {
      hdri_path = argv[++i];
    } else if (strcmp(argv[i], "--no-hdri") == 0) {
      hdri_path = NULL;
    } else
      outdir = argv[i];
  }
  if (width <= 0 || height <= 0) {
    fprintf(stderr, "invalid resolution %dx%d\n", width, height);
    return 1;
  }
  mkdir(outdir, 0755);

  /* --- 1. Create a viewport ---------------------------------------------
   * The default ssaa_factor (2x) supersamples the internal framebuffer
   * for smooth edges; the RTT blit (step 5) downsamples on the way out
   * so the host texture remains presentation-sized (width × height). */
  MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
      .width = width,
      .height = height,
      .backend = backend,
  });
  if (!vp) {
    fprintf(stderr, "mop_viewport_create failed\n");
    return 1;
  }

  /* --- 2. Scene setup ---------------------------------------------------
   * Camera, lights, post-processing. A real host might instead stream
   * its own camera matrix each frame via mop_viewport_set_camera. */
  mop_viewport_set_camera(vp, (MopVec3){4.5f, 3.2f, 5.0f}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 45.0f, 0.1f, 100.0f);
  mop_viewport_set_clear_color(vp, (MopColor){0.05f, 0.06f, 0.09f, 1.0f});
  mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);
  mop_viewport_set_chrome(vp, false); /* no grid/gizmo for embedded use */
  mop_viewport_set_ambient(vp, 0.15f);
  mop_viewport_set_post_effects(vp, MOP_POST_GAMMA | MOP_POST_TONEMAP |
                                        MOP_POST_FXAA);

  MopLight key = {.type = MOP_LIGHT_DIRECTIONAL,
                  .direction = {-0.4f, -0.9f, -0.3f},
                  .color = {1.0f, 0.95f, 0.9f, 1.0f},
                  .intensity = 3.0f,
                  .active = true};
  MopLight rim = {.type = MOP_LIGHT_POINT,
                  .position = {-3, 2, -2},
                  .color = {0.4f, 0.6f, 1.0f, 1.0f},
                  .intensity = 30.0f,
                  .range = 12.0f,
                  .active = true};
  mop_viewport_add_light(vp, &key);
  mop_viewport_add_light(vp, &rim);

  /* --- HDRI environment (IBL + skybox) --------------------------------
   * Loading an equirectangular .hdr / .exr gives MOP the data to
   * precompute diffuse irradiance + prefiltered specular + BRDF LUT
   * (standard split-sum PBR IBL). Materials then pick up real-world
   * ambient lighting and reflections from the environment. */
  if (hdri_path) {
    bool env_ok = mop_viewport_set_environment(vp, &(MopEnvironmentDesc){
                                                       .type = MOP_ENV_HDRI,
                                                       .hdr_path = hdri_path,
                                                       .rotation = 0.0f,
                                                       .intensity = 1.0f,
                                                   });
    if (env_ok) {
      mop_viewport_set_environment_background(vp, true);
      fprintf(stderr, "showcase: loaded HDRI %s\n", hdri_path);
    } else {
      fprintf(stderr,
              "showcase: HDRI load failed (%s), falling back to "
              "gradient\n",
              hdri_path);
    }
  }

  MopMesh *cube = add_cube(vp, 1, (MopColor){0.95f, 0.55f, 0.2f, 1.0f});
  MopMesh *sphere = add_sphere(vp, 2, (MopColor){0.3f, 0.7f, 0.95f, 1.0f});
  if (!cube || !sphere) {
    fprintf(stderr, "mesh creation failed\n");
    mop_viewport_destroy(vp);
    return 1;
  }

  /* PBR materials — demonstrate the metal / non-metal contrast. */
  MopMaterial metal = mop_material_default();
  metal.metallic = 0.9f;
  metal.roughness = 0.25f;
  mop_mesh_set_material(cube, &metal);

  MopMaterial plastic = mop_material_default();
  plastic.metallic = 0.0f;
  plastic.roughness = 0.35f;
  mop_mesh_set_material(sphere, &plastic);

  /* --- 3. Host-owned output texture -------------------------------------
   * The host creates an RGBA8 texture sized to the viewport's internal
   * framebuffer at presentation size (width × height). MOP downsamples
   * from the internal SSAA buffer during present_to_texture. */
  size_t pixel_bytes = (size_t)width * (size_t)height * 4;
  uint8_t *seed_pixels = calloc(1, pixel_bytes);
  MopTexture *host_tex =
      mop_tex_create(vp, &(MopTextureDesc){
                             .width = width,
                             .height = height,
                             .format = MOP_TEX_FORMAT_RGBA8,
                             .data = seed_pixels,
                             .data_size = (uint32_t)pixel_bytes,
                         });
  free(seed_pixels);
  if (!host_tex) {
    fprintf(stderr, "host texture creation failed\n");
    mop_viewport_destroy(vp);
    return 1;
  }

  /* --- 4. Host worker thread --------------------------------------------
   * Animates the scene on a thread separate from the render loop.
   * mop_viewport_scene_lock/unlock keeps the two in lockstep. */
  HostSim sim = {.vp = vp, .cube = cube, .sphere = sphere};
  atomic_store(&sim.running, 1);
  atomic_store(&sim.current_frame, 0);
  pthread_t worker;
  pthread_create(&worker, NULL, host_animate, &sim);

  /* --- 5. Render loop ---------------------------------------------------
   * Each iteration:
   *   render()                — mop_viewport_render acquires scene_lock
   *                             internally, serializing with the worker.
   *   present_to_texture()    — blits the LDR color into host_tex.
   *   tex_read_rgba8()        — host-side readback (CPU backend only).
   *                             A real GPU host would skip this and use
   *                             host_tex directly in its compositor.
   *   export_png_buffer()     — persists the frame for this example. */
  uint8_t *readback = malloc(pixel_bytes);
  if (!readback) {
    mop_viewport_destroy(vp);
    return 1;
  }

  printf("showcase: rendering %d frames at %dx%d to %s/ (%s backend)\n",
         FRAME_COUNT, width, height, outdir,
         backend == MOP_BACKEND_VULKAN ? "vulkan" : "cpu");

  for (int i = 0; i < FRAME_COUNT; i++) {
    atomic_store(&sim.current_frame, i);

    if (mop_viewport_render(vp) != MOP_RENDER_OK) {
      fprintf(stderr, "render failed on frame %d: %s\n", i,
              mop_viewport_get_last_error(vp));
      break;
    }
    if (!mop_viewport_present_to_texture(vp, host_tex)) {
      fprintf(stderr, "present_to_texture failed on frame %d\n", i);
      break;
    }
    if (!mop_tex_read_rgba8(vp, host_tex, readback, pixel_bytes)) {
      /* GPU backend without readback support — skip file write. */
      if (i == 0) {
        fprintf(stderr, "texture readback unavailable on this backend; "
                        "frames are in the host texture but not dumped.\n");
      }
      continue;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%03d.png", outdir, i);
    mop_export_png_buffer(readback, width, height, path);

    if ((i + 1) % 10 == 0)
      printf("  %d/%d\n", i + 1, FRAME_COUNT);
  }

  /* --- 6. Cleanup -------------------------------------------------------
   * Signal the worker to exit, join, then destroy the viewport.  The
   * viewport owns the mesh pool and host texture (via mop_tex_create) —
   * one destroy call tears everything down. */
  atomic_store(&sim.running, 0);
  pthread_join(worker, NULL);

  free(readback);
  mop_viewport_destroy(vp);

  printf("showcase: done — %d frames in %s/\n", FRAME_COUNT, outdir);
  return 0;
}
