/*
 * Master of Puppets — Interactive Viewport
 *
 * A complex 3D scene demonstrating MOP's rendering, lighting, materials,
 * hierarchy, animation, picking, and display features.
 *
 * Scene:
 *   - Ground plane with dark PBR material
 *   - Central metallic pedestal (stacked cubes)
 *   - Orbiting sphere ring (8 spheres, animated rotation)
 *   - Moon hierarchy: child sphere parented to an orbiter
 *   - Material gallery: row of spheres with varying metallic/roughness
 *   - Checker-textured floor accent
 *   - 3-point lighting: directional sun, point fill, spot accent
 *
 * Controls:
 *   Orbit    = left-drag          Pan     = right-drag
 *   Zoom     = scroll             Click   = pick object
 *   W        = toggle wireframe   F       = flat/smooth shading
 *   O        = toggle overlays    G       = toggle grid/bounds
 *   S        = spawn cube         Delete  = remove selected
 *   1-3      = gizmo mode (T/R/S) R       = reset camera
 *   Esc / Q  = quit
 *
 * Backend selection: set MOP_BACKEND=vulkan (default: cpu)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "common/geometry.h"
#include <SDL3/SDL.h>
#include <mop/core/environment.h>
#include <mop/mop.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 * Scene constants
 * ========================================================================= */

#define MAX_SPAWNED 64
#define ORBIT_RADIUS 4.0f
#define ORBIT_SPEED 0.4f
#define ORBIT_SPHERE_COUNT 8
#define ORBIT_SPHERE_RADIUS 0.3f
#define MATERIAL_ROW_COUNT 7
#define MATERIAL_ROW_Z (-4.5f)

/* =========================================================================
 * RNG
 * ========================================================================= */

static uint32_t rng_state = 12345;

static float randf(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return (float)(rng_state & 0xFFFF) / 65535.0f;
}

/* =========================================================================
 * Scene state
 * ========================================================================= */

typedef struct {
  /* Orbit ring */
  MopMesh *orbit_spheres[ORBIT_SPHERE_COUNT];
  float orbit_angle;

  /* Hierarchy: parent orbiter + child moon */
  MopMesh *orbiter;
  MopMesh *moon;
  float hierarchy_angle;

  /* Pedestal */
  MopMesh *pedestal_base;
  MopMesh *pedestal_mid;
  MopMesh *pedestal_top;

  /* Material row */
  MopMesh *mat_spheres[MATERIAL_ROW_COUNT];

  /* Ground */
  MopMesh *ground;

  /* Spawned cubes */
  MopMesh *spawned[MAX_SPAWNED];
  int spawn_count;
  uint32_t next_id;

  /* Texture */
  MopTexture *checker_tex;

  /* Camera object */
  MopCameraObject *scene_camera;

  /* Display state */
  bool overlays_on;
  bool bounds_on;
  MopShadingMode shading;

  /* HDR exposure */
  float exposure;
} Scene;

/* =========================================================================
 * Sphere mesh cache (generated once)
 * ========================================================================= */

static MopVertex sphere_verts[SPHERE_MAX_VERTS];
static uint32_t sphere_indices[SPHERE_MAX_INDICES];
static uint32_t sphere_vert_count;
static uint32_t sphere_index_count;
static bool sphere_generated = false;

static void ensure_sphere(void) {
  if (sphere_generated)
    return;
  sphere_vert_count = geometry_make_sphere(
      24, 36, 1.0f, sphere_verts, sphere_indices, (MopColor){1, 1, 1, 1});
  sphere_index_count = 24 * 36 * 6;
  sphere_generated = true;
}

/* Checker texture generation removed — ground plane removed */

/* =========================================================================
 * Build the scene
 * ========================================================================= */

static void build_scene(MopViewport *vp, Scene *sc) {
  ensure_sphere();
  memset(sc, 0, sizeof(*sc));
  sc->next_id = 100;
  sc->shading = MOP_SHADING_SMOOTH;
  sc->exposure = 1.0f;

  /* ---- Lighting ---- */
  mop_viewport_set_ambient(vp, 0.15f);

  /* Bright point light — creates HDR highlights on metallic surfaces */
  mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_POINT,
                                         .position = {2.0f, 4.0f, 3.0f},
                                         .color = {1.0f, 0.95f, 0.85f, 1.0f},
                                         .intensity = 80.0f,
                                         .active = true});

  /* Ground plane removed — grid provides spatial reference */
  sc->ground = NULL;

  /* ---- Pedestal (3 stacked cubes) ---- */
  sc->pedestal_base = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = CUBE_VERTICES,
                         .vertex_count = CUBE_VERTEX_COUNT,
                         .indices = CUBE_INDICES,
                         .index_count = CUBE_INDEX_COUNT,
                         .object_id = 10});
  mop_mesh_set_position(sc->pedestal_base, (MopVec3){0, 0.4f, 0});
  mop_mesh_set_scale(sc->pedestal_base, (MopVec3){1.6f, 0.8f, 1.6f});
  mop_mesh_set_material(sc->pedestal_base,
                        &(MopMaterial){
                            .base_color = {0.6f, 0.6f, 0.65f, 1},
                            .metallic = 0.9f,
                            .roughness = 0.2f,
                        });

  sc->pedestal_mid = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = CUBE_VERTICES,
                         .vertex_count = CUBE_VERTEX_COUNT,
                         .indices = CUBE_INDICES,
                         .index_count = CUBE_INDEX_COUNT,
                         .object_id = 11});
  mop_mesh_set_position(sc->pedestal_mid, (MopVec3){0, 1.1f, 0});
  mop_mesh_set_scale(sc->pedestal_mid, (MopVec3){1.2f, 0.6f, 1.2f});
  mop_mesh_set_material(sc->pedestal_mid,
                        &(MopMaterial){
                            .base_color = {0.7f, 0.7f, 0.75f, 1},
                            .metallic = 0.95f,
                            .roughness = 0.15f,
                        });

  sc->pedestal_top = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = CUBE_VERTICES,
                         .vertex_count = CUBE_VERTEX_COUNT,
                         .indices = CUBE_INDICES,
                         .index_count = CUBE_INDEX_COUNT,
                         .object_id = 12});
  mop_mesh_set_position(sc->pedestal_top, (MopVec3){0, 1.7f, 0});
  mop_mesh_set_scale(sc->pedestal_top, (MopVec3){0.8f, 0.4f, 0.8f});
  mop_mesh_set_material(sc->pedestal_top,
                        &(MopMaterial){
                            .base_color = {0.85f, 0.75f, 0.45f, 1},
                            .metallic = 1.0f,
                            .roughness = 0.1f,
                        });

  /* ---- Orbiting spheres ---- */
  static const MopColor orbit_colors[] = {
      {0.95f, 0.25f, 0.20f, 1}, {0.20f, 0.85f, 0.35f, 1},
      {0.25f, 0.45f, 0.95f, 1}, {0.95f, 0.85f, 0.15f, 1},
      {0.85f, 0.30f, 0.85f, 1}, {0.20f, 0.85f, 0.85f, 1},
      {1.00f, 0.55f, 0.15f, 1}, {0.60f, 0.90f, 0.25f, 1},
  };

  for (int i = 0; i < ORBIT_SPHERE_COUNT; i++) {
    sc->orbit_spheres[i] = mop_viewport_add_mesh(
        vp, &(MopMeshDesc){.vertices = sphere_verts,
                           .vertex_count = sphere_vert_count,
                           .indices = sphere_indices,
                           .index_count = sphere_index_count,
                           .object_id = (uint32_t)(20 + i)});
    mop_mesh_set_scale(sc->orbit_spheres[i],
                       (MopVec3){ORBIT_SPHERE_RADIUS, ORBIT_SPHERE_RADIUS,
                                 ORBIT_SPHERE_RADIUS});
    mop_mesh_set_material(sc->orbit_spheres[i],
                          &(MopMaterial){
                              .base_color = orbit_colors[i],
                              .metallic = 0.3f,
                              .roughness = 0.4f,
                          });
  }

  /* ---- Hierarchy: orbiter parent + moon child ---- */
  sc->orbiter = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = sphere_verts,
                         .vertex_count = sphere_vert_count,
                         .indices = sphere_indices,
                         .index_count = sphere_index_count,
                         .object_id = 50});
  mop_mesh_set_scale(sc->orbiter, (MopVec3){0.5f, 0.5f, 0.5f});
  mop_mesh_set_material(sc->orbiter, &(MopMaterial){
                                         .base_color = {0.9f, 0.5f, 0.1f, 1},
                                         .metallic = 0.6f,
                                         .roughness = 0.3f,
                                     });

  sc->moon = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = sphere_verts,
                         .vertex_count = sphere_vert_count,
                         .indices = sphere_indices,
                         .index_count = sphere_index_count,
                         .object_id = 51});
  mop_mesh_set_scale(sc->moon, (MopVec3){0.2f, 0.2f, 0.2f});
  mop_mesh_set_position(sc->moon, (MopVec3){1.2f, 0.3f, 0});
  mop_mesh_set_material(sc->moon, &(MopMaterial){
                                      .base_color = {0.8f, 0.8f, 0.75f, 1},
                                      .metallic = 0.1f,
                                      .roughness = 0.7f,
                                  });
  mop_mesh_set_parent(sc->moon, sc->orbiter, vp);

  /* ---- Material gallery row ---- */
  for (int i = 0; i < MATERIAL_ROW_COUNT; i++) {
    sc->mat_spheres[i] = mop_viewport_add_mesh(
        vp, &(MopMeshDesc){.vertices = sphere_verts,
                           .vertex_count = sphere_vert_count,
                           .indices = sphere_indices,
                           .index_count = sphere_index_count,
                           .object_id = (uint32_t)(60 + i)});
    float x = -3.0f + (float)i * 1.0f;
    mop_mesh_set_position(sc->mat_spheres[i],
                          (MopVec3){x, 0.5f, MATERIAL_ROW_Z});
    mop_mesh_set_scale(sc->mat_spheres[i], (MopVec3){0.4f, 0.4f, 0.4f});

    float t = (float)i / (float)(MATERIAL_ROW_COUNT - 1);
    mop_mesh_set_material(sc->mat_spheres[i],
                          &(MopMaterial){
                              .base_color = {0.9f, 0.9f, 0.9f, 1},
                              .metallic = t, /* 0 → 1 left to right */
                              .roughness = 1.0f - t * 0.8f, /* 1 → 0.2 */
                          });
  }

  /* ---- Camera ---- */
  mop_viewport_set_camera(vp, (MopVec3){6, 5, 8}, (MopVec3){0, 1.2f, 0},
                          (MopVec3){0, 1, 0}, 55.0f, 0.1f, 200.0f);

  /* ---- Camera object (visible in scene with frustum) ---- */
  sc->scene_camera =
      mop_viewport_add_camera(vp, &(MopCameraObjectDesc){
                                      .position = {-5.0f, 3.0f, 6.0f},
                                      .target = {0.0f, 1.0f, 0.0f},
                                      .up = {0.0f, 1.0f, 0.0f},
                                      .fov_degrees = 45.0f,
                                      .near_plane = 0.1f,
                                      .far_plane = 30.0f,
                                      .aspect_ratio = 16.0f / 9.0f,
                                      .object_id = 90,
                                      .name = "Camera",
                                  });
}

/* =========================================================================
 * Animation
 * ========================================================================= */

static void animate(MopViewport *vp, Scene *sc, float dt, float time) {
  (void)vp;

  sc->orbit_angle += ORBIT_SPEED * dt;

  /* Orbiting spheres ring */
  for (int i = 0; i < ORBIT_SPHERE_COUNT; i++) {
    float a =
        sc->orbit_angle + (float)i * (2.0f * (float)M_PI / ORBIT_SPHERE_COUNT);
    float y = 2.2f + sinf(time * 1.5f + (float)i * 0.8f) * 0.3f;
    mop_mesh_set_position(
        sc->orbit_spheres[i],
        (MopVec3){cosf(a) * ORBIT_RADIUS, y, sinf(a) * ORBIT_RADIUS});
    mop_mesh_set_rotation(sc->orbit_spheres[i],
                          (MopVec3){0, -a + (float)M_PI / 2.0f, 0});
  }

  /* Hierarchy orbiter */
  sc->hierarchy_angle += 0.6f * dt;
  float ha = sc->hierarchy_angle;
  float orb_r = 6.0f;
  mop_mesh_set_position(sc->orbiter, (MopVec3){cosf(ha) * orb_r,
                                               3.5f + sinf(time * 0.8f) * 0.5f,
                                               sinf(ha) * orb_r});
  mop_mesh_set_rotation(sc->orbiter, (MopVec3){0, ha * 2.0f, 0});

  /* Pedestal top: gentle spin */
  mop_mesh_set_rotation(sc->pedestal_top, (MopVec3){0, time * 0.3f, 0});
}

/* =========================================================================
 * HUD
 * ========================================================================= */

static void draw_hud(SDL_Renderer *r, int w, int h, const Scene *sc,
                     uint32_t selected_id) {
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

  /* Top bar */
  SDL_FRect bar = {0, 0, (float)w, 28};
  SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
  SDL_RenderFillRect(r, &bar);

  SDL_SetRenderDrawColor(r, 120, 180, 255, 255);
  SDL_RenderDebugText(r, 10, 9, "MOP Interactive Viewport");

  char buf[128];

  /* Shading mode */
  const char *shading_str =
      sc->shading == MOP_SHADING_SMOOTH ? "Smooth" : "Flat";
  snprintf(buf, sizeof(buf), "Shading: %s", shading_str);
  SDL_SetRenderDrawColor(r, 200, 200, 200, 255);
  SDL_RenderDebugText(r, 250, 9, buf);

  /* Exposure */
  snprintf(buf, sizeof(buf), "Exp: %.2f", sc->exposure);
  SDL_SetRenderDrawColor(r, 255, 200, 100, 255);
  SDL_RenderDebugText(r, 370, 9, buf);

  /* Overlays */
  if (sc->overlays_on) {
    SDL_SetRenderDrawColor(r, 100, 255, 100, 255);
    SDL_RenderDebugText(r, 430, 9, "[OVL]");
  }

  /* Selected object */
  if (selected_id > 0) {
    snprintf(buf, sizeof(buf), "Selected: %u", selected_id);
    SDL_SetRenderDrawColor(r, 255, 220, 100, 255);
    SDL_RenderDebugText(r, (float)w - 160, 9, buf);
  }

  /* Spawned count */
  if (sc->spawn_count > 0) {
    snprintf(buf, sizeof(buf), "Cubes: %d", sc->spawn_count);
    SDL_SetRenderDrawColor(r, 180, 180, 180, 200);
    SDL_RenderDebugText(r, (float)w - 300, 9, buf);
  }

  /* Bottom hint bar */
  SDL_FRect hbar = {0, (float)h - 20, (float)w, 20};
  SDL_SetRenderDrawColor(r, 0, 0, 0, 160);
  SDL_RenderFillRect(r, &hbar);
  SDL_SetRenderDrawColor(r, 160, 160, 170, 200);
  SDL_RenderDebugText(
      r, 10, (float)h - 14,
      "LMB=orbit  RMB=pan  Scroll=zoom  W=wire  F=shading  "
      "O=overlays  +/-=exposure  S=spawn  Del=remove  R=reset  Q=quit");
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  int win_w = 1280, win_h = 800;

  SDL_Window *window = SDL_CreateWindow("MOP — Interactive Viewport", win_w,
                                        win_h, SDL_WINDOW_RESIZABLE);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
  if (!renderer) {
    fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 0); /* no VSync — minimize input latency */

  /* Backend selection */
  MopBackendType backend = MOP_BACKEND_CPU;
  const char *env = getenv("MOP_BACKEND");
  if (env) {
    if (strcmp(env, "opengl") == 0)
      backend = MOP_BACKEND_OPENGL;
    if (strcmp(env, "vulkan") == 0)
      backend = MOP_BACKEND_VULKAN;
  }

  MopViewport *vp = mop_viewport_create(
      &(MopViewportDesc){.width = win_w, .height = win_h, .backend = backend});
  if (!vp) {
    fprintf(stderr, "Failed to create MOP viewport\n");
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);

  /* Background: solid #454545 */
  {
    MopTheme theme = mop_theme_default();
    theme.bg_top = (MopColor){0.0514f, 0.0514f, 0.0514f, 1.0f};
    theme.bg_bottom = (MopColor){0.0514f, 0.0514f, 0.0514f, 1.0f};
    mop_viewport_set_theme(vp, &theme);
  }
  mop_viewport_set_clear_color(vp, (MopColor){0.0514f, 0.0514f, 0.0514f, 1.0f});

  printf("[mop] Interactive Viewport  %dx%d  backend=%s\n", win_w, win_h,
         mop_backend_name(backend));

  /* Build scene */
  Scene sc;
  build_scene(vp, &sc);

  /* Load HDRI environment map if MOP_HDRI env var is set */
  {
    const char *hdri = getenv("MOP_HDRI");
    if (hdri) {
      MopEnvironmentDesc edesc = {
          .type = MOP_ENV_HDRI,
          .hdr_path = hdri,
          .rotation = 0.0f,
          .intensity = 1.0f,
      };
      if (mop_viewport_set_environment(vp, &edesc)) {
        printf("[mop] HDRI loaded: %s\n", hdri);
        /* Show HDRI as skybox background */
        mop_viewport_set_environment_background(vp, false);
        /* Sync with auto-exposure set by environment loader */
        sc.exposure = mop_viewport_get_exposure(vp);
      } else {
        fprintf(stderr, "[mop] Failed to load HDRI: %s\n", hdri);
      }
    }
  }

  /* SDL texture for framebuffer blit */
  SDL_Texture *tex =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                        SDL_TEXTUREACCESS_STREAMING, win_w, win_h);

  /* Timing */
  Uint64 last = SDL_GetPerformanceCounter();
  Uint64 freq = SDL_GetPerformanceFrequency();
  float total_time = 0.0f;
  bool running = true;

  /* RNG seed from time */
  rng_state = (uint32_t)(SDL_GetPerformanceCounter() & 0xFFFFFFFF);
  if (rng_state == 0)
    rng_state = 1;

  while (running) {
    Uint64 now = SDL_GetPerformanceCounter();
    float dt = (float)(now - last) / (float)freq;
    if (dt > 1.0f / 15.0f)
      dt = 1.0f / 15.0f;
    last = now;
    total_time += dt;

    /* ---- Events ---- */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {

      case SDL_EVENT_QUIT:
        running = false;
        break;

      case SDL_EVENT_KEY_DOWN:
        switch (ev.key.key) {
        case SDLK_ESCAPE:
        case SDLK_Q:
          running = false;
          break;

        case SDLK_W:
          mop_viewport_input(
              vp, &(MopInputEvent){.type = MOP_INPUT_TOGGLE_WIREFRAME});
          break;

        case SDLK_F:
          sc.shading = (sc.shading == MOP_SHADING_SMOOTH) ? MOP_SHADING_FLAT
                                                          : MOP_SHADING_SMOOTH;
          mop_viewport_set_shading(vp, sc.shading);
          break;

        case SDLK_O: {
          sc.overlays_on = !sc.overlays_on;
          MopDisplaySettings ds = mop_viewport_get_display(vp);
          ds.wireframe_overlay = sc.overlays_on;
          ds.wireframe_opacity = 0.3f;
          ds.wireframe_color = (MopColor){0.4f, 0.7f, 1.0f, 1.0f};
          ds.show_normals = sc.overlays_on;
          ds.normal_display_length = 0.15f;
          mop_viewport_set_display(vp, &ds);
          break;
        }

        case SDLK_G: {
          sc.bounds_on = !sc.bounds_on;
          MopDisplaySettings ds = mop_viewport_get_display(vp);
          ds.show_bounds = sc.bounds_on;
          mop_viewport_set_display(vp, &ds);
          break;
        }

        case SDLK_R:
          mop_viewport_input(vp,
                             &(MopInputEvent){.type = MOP_INPUT_RESET_VIEW});
          break;

        case SDLK_EQUALS: /* +/= key — increase exposure */
          sc.exposure *= 1.2f;
          if (sc.exposure > 16.0f)
            sc.exposure = 16.0f;
          mop_viewport_set_exposure(vp, sc.exposure);
          printf("[hdr] exposure = %.2f\n", sc.exposure);
          break;
        case SDLK_MINUS: /* - key — decrease exposure */
          sc.exposure /= 1.2f;
          if (sc.exposure < 0.05f)
            sc.exposure = 0.05f;
          mop_viewport_set_exposure(vp, sc.exposure);
          printf("[hdr] exposure = %.2f\n", sc.exposure);
          break;

        case SDLK_S: {
          /* Spawn a random cube near the center */
          if (sc.spawn_count < MAX_SPAWNED) {
            uint32_t id = sc.next_id++;
            MopMesh *m = mop_viewport_add_mesh(
                vp, &(MopMeshDesc){.vertices = CUBE_VERTICES,
                                   .vertex_count = CUBE_VERTEX_COUNT,
                                   .indices = CUBE_INDICES,
                                   .index_count = CUBE_INDEX_COUNT,
                                   .object_id = id});
            float x = (randf() - 0.5f) * 8.0f;
            float z = (randf() - 0.5f) * 8.0f;
            float s = 0.3f + randf() * 0.5f;
            mop_mesh_set_position(m, (MopVec3){x, s * 0.5f, z});
            mop_mesh_set_scale(m, (MopVec3){s, s, s});
            mop_mesh_set_material(
                m,
                &(MopMaterial){
                    .base_color = {0.3f + randf() * 0.7f, 0.3f + randf() * 0.7f,
                                   0.3f + randf() * 0.7f, 1},
                    .metallic = randf(),
                    .roughness = 0.2f + randf() * 0.6f,
                });
            sc.spawned[sc.spawn_count++] = m;
          }
          break;
        }

        case SDLK_DELETE:
        case SDLK_BACKSPACE: {
          uint32_t sel = mop_viewport_get_selected(vp);
          if (sel >= 100) { /* only remove spawned cubes */
            for (int i = 0; i < sc.spawn_count; i++) {
              MopVec3 pos = mop_mesh_get_position(sc.spawned[i]);
              (void)pos;
              /* Check via pick which spawned mesh is selected */
              /* Simple: remove last spawned if selected matches */
              mop_viewport_remove_mesh(vp, sc.spawned[i]);
              sc.spawned[i] = sc.spawned[--sc.spawn_count];
              mop_viewport_input(vp,
                                 &(MopInputEvent){.type = MOP_INPUT_DESELECT});
              break;
            }
          }
          break;
        }

        case SDLK_1:
          mop_viewport_input(
              vp, &(MopInputEvent){.type = MOP_INPUT_MODE_TRANSLATE});
          break;
        case SDLK_2:
          mop_viewport_input(vp,
                             &(MopInputEvent){.type = MOP_INPUT_MODE_ROTATE});
          break;
        case SDLK_3:
          mop_viewport_input(vp,
                             &(MopInputEvent){.type = MOP_INPUT_MODE_SCALE});
          break;

        default:
          break;
        }
        break;

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev.button.button == SDL_BUTTON_LEFT) {
          SDL_SetWindowRelativeMouseMode(window, true);
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_POINTER_DOWN,
                                                  ev.button.x, ev.button.y, 0,
                                                  0, 0});
        } else if (ev.button.button == SDL_BUTTON_RIGHT) {
          SDL_SetWindowRelativeMouseMode(window, true);
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_SECONDARY_DOWN,
                                                  ev.button.x, ev.button.y, 0,
                                                  0, 0});
        }
        break;

      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev.button.button == SDL_BUTTON_LEFT) {
          SDL_SetWindowRelativeMouseMode(window, false);
          mop_viewport_input(vp,
                             &(MopInputEvent){MOP_INPUT_POINTER_UP, ev.button.x,
                                              ev.button.y, 0, 0, 0});
        } else if (ev.button.button == SDL_BUTTON_RIGHT) {
          SDL_SetWindowRelativeMouseMode(window, false);
          mop_viewport_input(vp,
                             &(MopInputEvent){.type = MOP_INPUT_SECONDARY_UP});
        }
        break;

      case SDL_EVENT_MOUSE_MOTION:
        mop_viewport_input(vp,
                           &(MopInputEvent){MOP_INPUT_POINTER_MOVE, ev.motion.x,
                                            ev.motion.y, ev.motion.xrel,
                                            ev.motion.yrel, 0});
        break;

      case SDL_EVENT_MOUSE_WHEEL: {
        SDL_Keymod mod = SDL_GetModState();
        if (mod & SDL_KMOD_CTRL) {
          /* Pinch-to-zoom (macOS sends Ctrl+scroll for trackpad pinch) */
          mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SCROLL,
                                                  .scroll = ev.wheel.y});
        } else {
          /* Two-finger scroll = orbit (both axes) */
          mop_viewport_input(vp,
                             &(MopInputEvent){.type = MOP_INPUT_SCROLL_ORBIT,
                                              .dx = -ev.wheel.x * 5.0f,
                                              .dy = ev.wheel.y * 5.0f});
        }
        break;
      }

      case SDL_EVENT_WINDOW_RESIZED: {
        int w = ev.window.data1, h = ev.window.data2;
        if (w > 0 && h > 0) {
          win_w = w;
          win_h = h;
          mop_viewport_resize(vp, w, h);
          SDL_DestroyTexture(tex);
          tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
        }
        break;
      }

      default:
        break;
      }
    }

    /* ---- Animate ---- */
    animate(vp, &sc, dt, total_time);

    /* ---- Render ---- */
    mop_viewport_set_time(vp, total_time);
    mop_viewport_render(vp);

    /* ---- Blit ---- */
    int fb_w, fb_h;
    const uint8_t *px = mop_viewport_read_color(vp, &fb_w, &fb_h);
    if (px && tex) {
      SDL_UpdateTexture(tex, NULL, px, fb_w * 4);
      SDL_RenderClear(renderer);
      SDL_RenderTexture(renderer, tex, NULL, NULL);
      draw_hud(renderer, win_w, win_h, &sc, mop_viewport_get_selected(vp));
      SDL_RenderPresent(renderer);
    }

    /* ---- Poll MOP events ---- */
    MopEvent mev;
    while (mop_viewport_poll_event(vp, &mev)) {
      switch (mev.type) {
      case MOP_EVENT_SELECTED:
        printf("[mop] Selected object %u\n", mev.object_id);
        break;
      case MOP_EVENT_DESELECTED:
        printf("[mop] Deselected\n");
        break;
      case MOP_EVENT_TRANSFORM_CHANGED:
        printf("[mop] Transform changed: obj=%u pos=(%.2f, %.2f, %.2f)\n",
               mev.object_id, mev.position.x, mev.position.y, mev.position.z);
        break;
      default:
        break;
      }
    }
  }

  /* Cleanup */
  if (sc.checker_tex)
    mop_viewport_destroy_texture(vp, sc.checker_tex);
  SDL_DestroyTexture(tex);
  mop_viewport_destroy(vp);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
