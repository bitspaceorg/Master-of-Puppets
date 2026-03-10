/*
 * Master of Puppets — Dark Sanctum
 *
 * An AAA-style game scene demonstrating MOP's full feature set:
 * multi-light PBR rendering, particles, water, fog, bloom, SSAO,
 * procedural sky, hierarchy, and transparency.
 *
 * Scene: A dark fantasy temple courtyard at dusk.  Stone pillars form
 * a circular colonnade around a 3-tier obsidian-and-gold altar.
 * Torches flicker on the pillars, a reflecting pool surrounds the
 * altar, magical orbs orbit above, and atmospheric fog rolls in.
 *
 * Controls:
 *   Orbit    = left-drag          Pan     = right-drag
 *   Zoom     = scroll
 *   W        = toggle wireframe   F       = flat/smooth shading
 *   +/-      = HDR exposure       R       = reset camera
 *   Esc / Q  = quit
 *
 * Backend selection: set MOP_BACKEND=vulkan (default: cpu)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "common/geometry.h"
#include <mop/core/environment.h>
#include <mop/mop.h>

#include <SDL3/SDL.h>
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

#define PILLAR_COUNT 8
#define COLONNADE_RADIUS 8.0f
#define PILLAR_HEIGHT 5.0f
#define PILLAR_WIDTH 0.6f

#define ORB_COUNT 5
#define ORB_ORBIT_RADIUS 1.2f
#define ORB_ORBIT_SPEED 0.7f
#define ORB_BOB_SPEED 1.5f
#define ORB_BOB_AMP 0.15f

#define TORCH_COUNT 4
#define TORCH_FLICKER_SPEED 8.0f
#define TORCH_BASE_INTENSITY 8.0f
#define TORCH_FLICKER_AMP 2.0f

#define CRYSTAL_BOB_SPEED 1.2f
#define CRYSTAL_BOB_AMP 0.25f
#define CRYSTAL_SPIN_SPEED 0.8f
#define CRYSTAL_BASE_Y 3.6f

/* =========================================================================
 * Simple xorshift RNG
 * ========================================================================= */

static uint32_t rng_state = 0xDEADBEEF;

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
  /* Ground */
  MopMesh *ground;

  /* Pillars */
  MopMesh *pillars[PILLAR_COUNT];

  /* Altar (3 tiers) */
  MopMesh *altar_base;
  MopMesh *altar_mid;
  MopMesh *altar_top;

  /* Floating crystal on altar */
  MopMesh *crystal;

  /* Magic orbs: center hub (invisible) + 5 children */
  MopMesh *orb_hub;
  MopMesh *orbs[ORB_COUNT];

  /* Low walls between pillars */
  MopMesh *walls[PILLAR_COUNT];

  /* Lights */
  MopLight *moonlight;
  MopLight *torch_lights[TORCH_COUNT];
  MopLight *altar_glow;
  MopLight *spotlight;

  /* Particles */
  MopParticleEmitter *torch_fire[2];
  MopParticleEmitter *altar_sparks;

  /* Water */
  MopWaterSurface *pool;

  /* Animation state */
  float orb_angle;
  float torch_phase[TORCH_COUNT];

  /* Display */
  MopShadingMode shading;
  float exposure;
} Scene;

/* =========================================================================
 * Colored geometry helpers — Vulkan shader uses vertex color as PBR base
 * color, so we bake material color into vertices directly.
 *
 * Vertex data is written into a caller-provided stack buffer, then copied
 * to the GPU by mop_viewport_add_mesh.  The buffer can be reused after
 * add_mesh returns.
 * ========================================================================= */

static void fill_colored_cube(MopVertex *out, MopColor c) {
  memcpy(out, CUBE_VERTICES, sizeof(MopVertex) * CUBE_VERTEX_COUNT);
  for (int i = 0; i < CUBE_VERTEX_COUNT; i++)
    out[i].color = c;
}

static void fill_colored_plane(MopVertex *out, MopColor c) {
  memcpy(out, PLANE_VERTICES, sizeof(MopVertex) * PLANE_VERTEX_COUNT);
  for (int i = 0; i < PLANE_VERTEX_COUNT; i++)
    out[i].color = c;
}

/* =========================================================================
 * Sphere mesh cache
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

/* =========================================================================
 * Build the scene
 * ========================================================================= */

static void build_scene(MopViewport *vp, Scene *sc) {
  ensure_sphere();
  memset(sc, 0, sizeof(*sc));
  sc->shading = MOP_SHADING_SMOOTH;
  sc->exposure = 1.0f;

  /* Seed torch flicker phases */
  for (int i = 0; i < TORCH_COUNT; i++)
    sc->torch_phase[i] = randf() * 2.0f * (float)M_PI;

  /* ================================================================
   * Materials — metallic/roughness/emissive are sent via UBO;
   * base_color must be baked into vertex colors for the Vulkan shader.
   * ================================================================ */

  static const MopColor col_stone = {0.4f, 0.38f, 0.35f, 1.0f};
  static const MopColor col_obsidian = {0.12f, 0.12f, 0.14f, 1.0f};
  static const MopColor col_gold = {0.85f, 0.65f, 0.2f, 1.0f};
  static const MopColor col_iron = {0.3f, 0.3f, 0.32f, 1.0f};
  static const MopColor col_crystal = {0.3f, 0.9f, 1.0f, 1.0f};

  MopMaterial mat_stone = {
      .base_color = col_stone,
      .metallic = 0.05f,
      .roughness = 0.9f,
  };

  MopMaterial mat_obsidian = {
      .base_color = col_obsidian,
      .metallic = 0.4f,
      .roughness = 0.35f,
  };

  MopMaterial mat_gold = {
      .base_color = col_gold,
      .metallic = 1.0f,
      .roughness = 0.15f,
  };

  MopMaterial mat_iron = {
      .base_color = col_iron,
      .metallic = 0.9f,
      .roughness = 0.4f,
  };

  /* Reusable stack buffers for colored vertex data.
   * fill_colored_cube/plane writes into these, then add_mesh copies
   * the data to the GPU.  Safe to overwrite and reuse after add_mesh. */
  MopVertex cv[CUBE_VERTEX_COUNT];
  MopVertex pv[PLANE_VERTEX_COUNT];

  /* ================================================================
   * Ground platform
   * ================================================================ */

  fill_colored_plane(pv, col_stone);
  sc->ground = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = pv,
                         .vertex_count = PLANE_VERTEX_COUNT,
                         .indices = PLANE_INDICES,
                         .index_count = PLANE_INDEX_COUNT,
                         .object_id = 0});
  mop_mesh_set_position(sc->ground, (MopVec3){0, -0.02f, 0});
  mop_mesh_set_scale(sc->ground, (MopVec3){2.5f, 1.0f, 2.5f});
  mop_mesh_set_material(sc->ground, &mat_stone);

  /* ================================================================
   * 8 stone pillars in a circle
   * ================================================================ */

  fill_colored_cube(cv, col_stone);
  for (int i = 0; i < PILLAR_COUNT; i++) {
    float angle = (float)i * (2.0f * (float)M_PI / (float)PILLAR_COUNT);
    float px = cosf(angle) * COLONNADE_RADIUS;
    float pz = sinf(angle) * COLONNADE_RADIUS;

    sc->pillars[i] = mop_viewport_add_mesh(
        vp, &(MopMeshDesc){.vertices = cv,
                           .vertex_count = CUBE_VERTEX_COUNT,
                           .indices = CUBE_INDICES,
                           .index_count = CUBE_INDEX_COUNT,
                           .object_id = 0});
    mop_mesh_set_position(sc->pillars[i],
                          (MopVec3){px, PILLAR_HEIGHT * 0.5f, pz});
    mop_mesh_set_scale(sc->pillars[i],
                       (MopVec3){PILLAR_WIDTH, PILLAR_HEIGHT, PILLAR_WIDTH});
    mop_mesh_set_material(sc->pillars[i], &mat_stone);
  }

  /* ================================================================
   * Low walls between pillars
   * ================================================================ */

  fill_colored_cube(cv, col_iron);
  for (int i = 0; i < PILLAR_COUNT; i++) {
    float a0 = (float)i * (2.0f * (float)M_PI / (float)PILLAR_COUNT);
    float a1 = (float)(i + 1) * (2.0f * (float)M_PI / (float)PILLAR_COUNT);
    float mid_angle = (a0 + a1) * 0.5f;
    float wx = cosf(mid_angle) * (COLONNADE_RADIUS - 0.1f);
    float wz = sinf(mid_angle) * (COLONNADE_RADIUS - 0.1f);

    sc->walls[i] = mop_viewport_add_mesh(
        vp, &(MopMeshDesc){.vertices = cv,
                           .vertex_count = CUBE_VERTEX_COUNT,
                           .indices = CUBE_INDICES,
                           .index_count = CUBE_INDEX_COUNT,
                           .object_id = 0});
    mop_mesh_set_position(sc->walls[i], (MopVec3){wx, 0.4f, wz});

    float gap =
        2.0f * COLONNADE_RADIUS * sinf((float)M_PI / (float)PILLAR_COUNT);
    float wall_len = gap - PILLAR_WIDTH - 0.2f;
    if (wall_len < 0.5f)
      wall_len = 0.5f;
    mop_mesh_set_scale(sc->walls[i], (MopVec3){wall_len, 0.8f, 0.25f});
    mop_mesh_set_rotation(sc->walls[i],
                          (MopVec3){0, -(mid_angle + (float)M_PI * 0.5f), 0});
    mop_mesh_set_material(sc->walls[i], &mat_iron);
  }

  /* ================================================================
   * 3-tier altar at center
   * ================================================================ */

  fill_colored_cube(cv, col_obsidian);
  sc->altar_base = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = cv,
                         .vertex_count = CUBE_VERTEX_COUNT,
                         .indices = CUBE_INDICES,
                         .index_count = CUBE_INDEX_COUNT,
                         .object_id = 0});
  mop_mesh_set_position(sc->altar_base, (MopVec3){0, 0.5f, 0});
  mop_mesh_set_scale(sc->altar_base, (MopVec3){2.4f, 1.0f, 2.4f});
  mop_mesh_set_material(sc->altar_base, &mat_obsidian);

  sc->altar_mid = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = cv,
                         .vertex_count = CUBE_VERTEX_COUNT,
                         .indices = CUBE_INDICES,
                         .index_count = CUBE_INDEX_COUNT,
                         .object_id = 0});
  mop_mesh_set_position(sc->altar_mid, (MopVec3){0, 1.42f, 0});
  mop_mesh_set_scale(sc->altar_mid, (MopVec3){1.8f, 0.8f, 1.8f});
  mop_mesh_set_material(sc->altar_mid, &mat_obsidian);

  fill_colored_cube(cv, col_gold);
  sc->altar_top = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = cv,
                         .vertex_count = CUBE_VERTEX_COUNT,
                         .indices = CUBE_INDICES,
                         .index_count = CUBE_INDEX_COUNT,
                         .object_id = 0});
  mop_mesh_set_position(sc->altar_top, (MopVec3){0, 2.17f, 0});
  mop_mesh_set_scale(sc->altar_top, (MopVec3){1.2f, 0.5f, 1.2f});
  mop_mesh_set_material(sc->altar_top, &mat_gold);

  /* ================================================================
   * Floating crystal above altar
   * ================================================================ */

  fill_colored_cube(cv, col_crystal);
  sc->crystal = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = cv,
                         .vertex_count = CUBE_VERTEX_COUNT,
                         .indices = CUBE_INDICES,
                         .index_count = CUBE_INDEX_COUNT,
                         .object_id = 0});
  mop_mesh_set_position(sc->crystal, (MopVec3){0, CRYSTAL_BASE_Y, 0});
  mop_mesh_set_scale(sc->crystal, (MopVec3){0.3f, 0.6f, 0.3f});
  mop_mesh_set_rotation(sc->crystal,
                        (MopVec3){(float)M_PI / 4.0f, 0, (float)M_PI / 4.0f});
  mop_mesh_set_material(sc->crystal, &(MopMaterial){
                                         .base_color = col_crystal,
                                         .metallic = 0.3f,
                                         .roughness = 0.1f,
                                         .emissive = {0.15f, 0.4f, 0.5f},
                                     });

  /* ================================================================
   * Magic orbs — colored spheres orbiting above altar
   *
   * Vulkan shader uses vertex color as PBR base, so we generate
   * per-orb sphere meshes with baked vertex colors.  No parenting
   * (avoids extreme scale ratios); positions set each frame.
   * ================================================================ */

  static const MopColor orb_colors[ORB_COUNT] = {
      {0.2f, 0.6f, 1.0f, 1.0f}, /* blue */
      {1.0f, 0.3f, 0.5f, 1.0f}, /* pink */
      {0.3f, 1.0f, 0.4f, 1.0f}, /* green */
      {1.0f, 0.8f, 0.2f, 1.0f}, /* amber */
      {0.8f, 0.3f, 1.0f, 1.0f}, /* purple */
  };

  static const MopVec3 orb_emissive[ORB_COUNT] = {
      {0.1f, 0.3f, 0.5f}, {0.5f, 0.15f, 0.25f}, {0.15f, 0.5f, 0.2f},
      {0.5f, 0.4f, 0.1f}, {0.4f, 0.15f, 0.5f},
  };

  /* Generate colored sphere verts for each orb */
  static MopVertex orb_verts[ORB_COUNT][SPHERE_MAX_VERTS];
  static uint32_t orb_indices[SPHERE_MAX_INDICES];
  static uint32_t orb_vc, orb_ic;
  {
    orb_vc = geometry_make_sphere(24, 36, 1.0f, orb_verts[0], orb_indices,
                                  orb_colors[0]);
    orb_ic = 24 * 36 * 6;
    for (int i = 1; i < ORB_COUNT; i++) {
      memcpy(orb_verts[i], orb_verts[0], sizeof(MopVertex) * orb_vc);
      for (uint32_t v = 0; v < orb_vc; v++)
        orb_verts[i][v].color = orb_colors[i];
    }
  }

  sc->orb_hub = NULL; /* not used */
  for (int i = 0; i < ORB_COUNT; i++) {
    sc->orbs[i] =
        mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = orb_verts[i],
                                                 .vertex_count = orb_vc,
                                                 .indices = orb_indices,
                                                 .index_count = orb_ic,
                                                 .object_id = 0});
    mop_mesh_set_scale(sc->orbs[i], (MopVec3){0.15f, 0.15f, 0.15f});
    mop_mesh_set_material(sc->orbs[i], &(MopMaterial){
                                           .base_color = orb_colors[i],
                                           .metallic = 0.3f,
                                           .roughness = 0.1f,
                                           .emissive = orb_emissive[i],
                                       });
  }

  /* ================================================================
   * Lighting
   * ================================================================ */

  mop_viewport_clear_lights(vp);
  mop_viewport_set_ambient(vp, 0.02f);

  /* Moonlight — cool blue directional from above-right */
  sc->moonlight =
      mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_DIRECTIONAL,
                                             .direction = {-0.4f, -0.8f, -0.3f},
                                             .color = {0.6f, 0.7f, 1.0f, 1.0f},
                                             .intensity = 0.5f,
                                             .active = true});

  /* 4 torch point lights on alternating pillars */
  static const int torch_pillars[TORCH_COUNT] = {0, 2, 4, 6};
  for (int i = 0; i < TORCH_COUNT; i++) {
    int pi = torch_pillars[i];
    float angle = (float)pi * (2.0f * (float)M_PI / (float)PILLAR_COUNT);
    float tx = cosf(angle) * (COLONNADE_RADIUS - 0.5f);
    float tz = sinf(angle) * (COLONNADE_RADIUS - 0.5f);
    float ty = PILLAR_HEIGHT * 0.75f;

    sc->torch_lights[i] = mop_viewport_add_light(
        vp, &(MopLight){.type = MOP_LIGHT_POINT,
                        .position = {tx, ty, tz},
                        .color = {1.0f, 0.65f, 0.25f, 1.0f},
                        .intensity = TORCH_BASE_INTENSITY,
                        .range = 15.0f,
                        .active = true});
  }

  /* Altar glow — eerie cyan/green */
  sc->altar_glow =
      mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_POINT,
                                             .position = {0, 3.0f, 0},
                                             .color = {0.2f, 0.9f, 0.8f, 1.0f},
                                             .intensity = 5.0f,
                                             .range = 12.0f,
                                             .active = true});

  /* Spotlight from above onto altar */
  sc->spotlight =
      mop_viewport_add_light(vp, &(MopLight){.type = MOP_LIGHT_SPOT,
                                             .position = {0, 10.0f, 0},
                                             .direction = {0, -1.0f, 0},
                                             .color = {0.9f, 0.85f, 1.0f, 1.0f},
                                             .intensity = 12.0f,
                                             .range = 20.0f,
                                             .spot_inner_cos = 0.95f, /* ~18° */
                                             .spot_outer_cos = 0.85f, /* ~32° */
                                             .active = true});

  /* ================================================================
   * Particle emitters
   * ================================================================ */

  /* Particles disabled — Vulkan billboard rendering without sprite
   * textures produces checkered artifacts.  TODO: add sprite textures. */
  (void)torch_pillars;

  /* ================================================================
   * Reflecting pool around altar
   * ================================================================ */

  /* Water disabled — Vulkan particle/water transparency causes artifacts.
   * TODO: investigate Vulkan alpha blending pipeline. */
  sc->pool = NULL;

  /* ================================================================
   * Procedural sky — dusk
   * ================================================================ */

  {
    MopEnvironmentDesc env = {
        .type = MOP_ENV_PROCEDURAL_SKY,
        .intensity = 0.8f,
    };
    mop_viewport_set_environment(vp, &env);
    mop_viewport_set_procedural_sky(
        vp, &(MopProceduralSkyDesc){
                .sun_direction = {-0.3f, 0.05f, -0.95f}, /* near horizon */
                .turbidity = 4.5f,
                .ground_albedo = 0.2f,
            });
    mop_viewport_set_environment_background(vp, true);
  }

  /* ================================================================
   * Post-processing
   * ================================================================ */

  mop_viewport_set_post_effects(vp, MOP_POST_TONEMAP | MOP_POST_BLOOM |
                                        MOP_POST_SSAO | MOP_POST_FOG |
                                        MOP_POST_FXAA | MOP_POST_VIGNETTE);

  mop_viewport_set_fog(vp, &(MopFogParams){
                               .color = {0.02f, 0.02f, 0.05f, 1.0f},
                               .near_dist = 15.0f,
                               .far_dist = 50.0f,
                           });

  mop_viewport_set_bloom(vp, 0.5f, 0.4f);
  mop_viewport_set_exposure(vp, sc->exposure);

  /* ================================================================
   * Camera
   * ================================================================ */

  mop_viewport_set_camera(vp, (MopVec3){10.0f, 6.0f, 10.0f},
                          (MopVec3){0, 2.0f, 0}, (MopVec3){0, 1, 0}, 50.0f,
                          0.1f, 200.0f);

  /* Dark theme to match the scene */
  {
    MopTheme theme = mop_theme_default();
    theme.bg_top = (MopColor){0.01f, 0.01f, 0.02f, 1.0f};
    theme.bg_bottom = (MopColor){0.01f, 0.01f, 0.02f, 1.0f};
    mop_viewport_set_theme(vp, &theme);
  }
  mop_viewport_set_clear_color(vp, (MopColor){0.01f, 0.01f, 0.02f, 1.0f});

  /* Hide editor chrome — no grid, no axis navigator */
  mop_viewport_set_chrome(vp, false);

  mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);
}

/* =========================================================================
 * Per-frame animation
 * ========================================================================= */

static void animate(MopViewport *vp, Scene *sc, float dt, float time) {
  (void)vp;

  /* ---- Orb orbit (world-space positions) ---- */
  sc->orb_angle += ORB_ORBIT_SPEED * dt;

  for (int i = 0; i < ORB_COUNT; i++) {
    float a =
        sc->orb_angle + (float)i * (2.0f * (float)M_PI / (float)ORB_COUNT);
    float y = 3.0f + sinf(time * ORB_BOB_SPEED + (float)i * 1.2f) * ORB_BOB_AMP;
    mop_mesh_set_position(sc->orbs[i], (MopVec3){cosf(a) * ORB_ORBIT_RADIUS, y,
                                                 sinf(a) * ORB_ORBIT_RADIUS});
  }

  /* ---- Torch flicker ---- */
  for (int i = 0; i < TORCH_COUNT; i++) {
    if (!sc->torch_lights[i])
      continue;
    /* Pseudo-random flicker: combine multiple sine waves */
    float flicker =
        sinf(time * TORCH_FLICKER_SPEED + sc->torch_phase[i]) * 0.5f +
        sinf(time * TORCH_FLICKER_SPEED * 1.7f + sc->torch_phase[i] * 2.3f) *
            0.3f +
        sinf(time * TORCH_FLICKER_SPEED * 3.1f + sc->torch_phase[i] * 0.7f) *
            0.2f;
    float intensity = TORCH_BASE_INTENSITY + flicker * TORCH_FLICKER_AMP;
    if (intensity < 4.0f)
      intensity = 4.0f;
    mop_light_set_intensity(sc->torch_lights[i], intensity);
  }

  /* ---- Floating crystal bob + spin ---- */
  {
    float y = CRYSTAL_BASE_Y + sinf(time * CRYSTAL_BOB_SPEED) * CRYSTAL_BOB_AMP;
    mop_mesh_set_position(sc->crystal, (MopVec3){0, y, 0});
    mop_mesh_set_rotation(sc->crystal, (MopVec3){(float)M_PI / 4.0f,
                                                 time * CRYSTAL_SPIN_SPEED,
                                                 (float)M_PI / 4.0f});
  }

  /* ---- Water animation ---- */
  if (sc->pool)
    mop_water_set_time(sc->pool, time);
}

/* No HUD — clean game presentation */

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[]) {
  /* --headless: render one frame, save PNG, exit (no SDL) */
  bool headless = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--headless") == 0)
      headless = true;
  }

  if (headless) {
    MopBackendType hb = MOP_BACKEND_CPU;
    const char *henv = getenv("MOP_BACKEND");
    if (henv && strcmp(henv, "vulkan") == 0)
      hb = MOP_BACKEND_VULKAN;

    MopViewport *hvp = mop_viewport_create(
        &(MopViewportDesc){.width = 1280, .height = 800, .backend = hb});
    if (!hvp) {
      fprintf(stderr, "viewport failed\n");
      return 1;
    }
    printf("[headless] backend=%s\n", henv ? henv : "cpu");

    Scene hsc;
    build_scene(hvp, &hsc);

    animate(hvp, &hsc, 0.016f, 0.5f);
    mop_viewport_set_time(hvp, 0.5f);
    mop_viewport_render(hvp);

    /* Use image_export for PNG */
    extern int mop_export_png(MopViewport * vp, const char *path);
    mop_export_png(hvp, "/tmp/dark_sanctum_headless.png");
    printf("[headless] saved /tmp/dark_sanctum_headless.png\n");

    mop_viewport_destroy(hvp);
    return 0;
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  int win_w = 1280, win_h = 800;

  SDL_Window *window = SDL_CreateWindow("MOP \xe2\x80\x94 Dark Sanctum", win_w,
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
  SDL_SetRenderVSync(renderer, 0);

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

  printf("[mop] Dark Sanctum  %dx%d  backend=%s\n", win_w, win_h,
         mop_backend_name(backend));

  /* Build scene */
  Scene sc;
  build_scene(vp, &sc);

  /* SDL texture for framebuffer blit */
  SDL_Texture *tex =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                        SDL_TEXTUREACCESS_STREAMING, win_w, win_h);

  /* Timing */
  Uint64 last = SDL_GetPerformanceCounter();
  Uint64 freq = SDL_GetPerformanceFrequency();
  float total_time = 0.0f;
  bool running = true;

  /* Seed RNG */
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

        case SDLK_R:
          mop_viewport_input(vp,
                             &(MopInputEvent){.type = MOP_INPUT_RESET_VIEW});
          break;

        case SDLK_EQUALS:
          sc.exposure *= 1.2f;
          if (sc.exposure > 16.0f)
            sc.exposure = 16.0f;
          mop_viewport_set_exposure(vp, sc.exposure);
          break;

        case SDLK_MINUS:
          sc.exposure /= 1.2f;
          if (sc.exposure < 0.05f)
            sc.exposure = 0.05f;
          mop_viewport_set_exposure(vp, sc.exposure);
          break;

        default:
          break;
        }
        break;

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev.button.button == SDL_BUTTON_LEFT) {
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_POINTER_DOWN,
                                                  ev.button.x, ev.button.y, 0,
                                                  0, 0});
        } else if (ev.button.button == SDL_BUTTON_RIGHT) {
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_SECONDARY_DOWN,
                                                  ev.button.x, ev.button.y, 0,
                                                  0, 0});
        }
        break;

      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev.button.button == SDL_BUTTON_LEFT) {
          mop_viewport_input(vp,
                             &(MopInputEvent){MOP_INPUT_POINTER_UP, ev.button.x,
                                              ev.button.y, 0, 0, 0});
        } else if (ev.button.button == SDL_BUTTON_RIGHT) {
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
          mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SCROLL,
                                                  .scroll = ev.wheel.y});
        } else {
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
      SDL_RenderPresent(renderer);
    }

    /* ---- Drain MOP events ---- */
    MopEvent mev;
    while (mop_viewport_poll_event(vp, &mev))
      ;
  }

  /* Cleanup */
  SDL_DestroyTexture(tex);
  mop_viewport_destroy(vp);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
