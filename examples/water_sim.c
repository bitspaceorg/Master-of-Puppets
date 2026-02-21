/*
 * Master of Puppets — Interactive Water Simulation (SDL3)
 *
 * Architecture:
 *   The APPLICATION owns all simulation logic:
 *     - Water grid generation (vertex positions)
 *     - Sine-wave displacement computation
 *     - Normal recomputation from displaced geometry
 *     - Parameter adjustment (amplitude, frequency, speed)
 *     - Fire/smoke particle emitters
 *
 *   MOP owns only:
 *     - Rendering submitted geometry
 *     - Camera / input handling (orbit, pan, zoom)
 *     - Selection / gizmo system
 *     - Post-processing
 *
 *   Controls are via a left sidebar (ui_toolbar.h).  Emitters are
 *   repositioned via MOP's gizmo system: click the yellow octahedron
 *   marker, drag the translate gizmo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_toolbar.h"
#include <SDL3/SDL.h>
#include <mop/mop.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * APP-OWNED SIMULATION — water grid solver
 * ========================================================================= */

typedef struct {
  MopVertex *verts;
  uint32_t *indices;
  int resolution;
  float extent;

  float *rest_x;
  float *rest_z;

  float wave_amplitude;
  float wave_frequency;
  float wave_speed;
  float opacity;
  float color_r, color_g, color_b;
} WaterSim;

static void water_sim_create(WaterSim *ws, int resolution, float extent) {
  int n = resolution;
  ws->resolution = n;
  ws->extent = extent;

  ws->verts = calloc((size_t)(n * n), sizeof(MopVertex));
  ws->rest_x = calloc((size_t)(n * n), sizeof(float));
  ws->rest_z = calloc((size_t)(n * n), sizeof(float));
  ws->indices = calloc((size_t)((n - 1) * (n - 1) * 6), sizeof(uint32_t));

  for (int z = 0; z < n; z++) {
    for (int x = 0; x < n; x++) {
      int i = z * n + x;
      ws->rest_x[i] = -extent + 2.0f * extent * (float)x / (float)(n - 1);
      ws->rest_z[i] = -extent + 2.0f * extent * (float)z / (float)(n - 1);
    }
  }

  uint32_t ii = 0;
  for (int z = 0; z < n - 1; z++) {
    for (int x = 0; x < n - 1; x++) {
      uint32_t tl = (uint32_t)(z * n + x);
      uint32_t tr = tl + 1;
      uint32_t bl = tl + (uint32_t)n;
      uint32_t br = bl + 1;
      ws->indices[ii++] = tl;
      ws->indices[ii++] = bl;
      ws->indices[ii++] = tr;
      ws->indices[ii++] = tr;
      ws->indices[ii++] = bl;
      ws->indices[ii++] = br;
    }
  }

  ws->wave_amplitude = 0.12f;
  ws->wave_frequency = 2.0f;
  ws->wave_speed = 1.5f;
  ws->opacity = 0.65f;
  ws->color_r = 0.08f;
  ws->color_g = 0.25f;
  ws->color_b = 0.55f;
}

static void water_sim_free(WaterSim *ws) {
  free(ws->verts);
  free(ws->rest_x);
  free(ws->rest_z);
  free(ws->indices);
  memset(ws, 0, sizeof(*ws));
}

static void water_sim_update(WaterSim *ws, float t) {
  int n = ws->resolution;
  float amp = ws->wave_amplitude;
  float freq = ws->wave_frequency;
  float spd = ws->wave_speed;

  MopColor c = {ws->color_r, ws->color_g, ws->color_b, ws->opacity};

  for (int i = 0; i < n * n; i++) {
    float x = ws->rest_x[i];
    float z = ws->rest_z[i];
    float y =
        amp * sinf(freq * (x + t * spd)) * sinf(freq * (z + t * spd * 0.7f));

    ws->verts[i].position = (MopVec3){x, y, z};
    ws->verts[i].color = c;
    ws->verts[i].u = (x + ws->extent) / (2.0f * ws->extent);
    ws->verts[i].v = (z + ws->extent) / (2.0f * ws->extent);
  }

  float eps = 0.01f;
  for (int i = 0; i < n * n; i++) {
    float x = ws->rest_x[i];
    float z = ws->rest_z[i];

    float y_px = amp * sinf(freq * (x + eps + t * spd)) *
                 sinf(freq * (z + t * spd * 0.7f));
    float y_mx = amp * sinf(freq * (x - eps + t * spd)) *
                 sinf(freq * (z + t * spd * 0.7f));
    float y_pz = amp * sinf(freq * (x + t * spd)) *
                 sinf(freq * (z + eps + t * spd * 0.7f));
    float y_mz = amp * sinf(freq * (x + t * spd)) *
                 sinf(freq * (z - eps + t * spd * 0.7f));

    float dx = (y_px - y_mx) / (2.0f * eps);
    float dz = (y_pz - y_mz) / (2.0f * eps);

    float nx = -dx, ny = 1.0f, nz = -dz;
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if (len > 1e-6f) {
      nx /= len;
      ny /= len;
      nz /= len;
    }
    ws->verts[i].normal = (MopVec3){nx, ny, nz};
  }
}

static uint32_t water_vertex_count(const WaterSim *ws) {
  return (uint32_t)(ws->resolution * ws->resolution);
}

static uint32_t water_index_count(const WaterSim *ws) {
  int n = ws->resolution;
  return (uint32_t)((n - 1) * (n - 1) * 6);
}

/* =========================================================================
 * APP-OWNED PARTICLE SIM (fire/smoke for the island scene)
 * ========================================================================= */

typedef struct {
  float x, y, z, vx, vy, vz, life, maxlife, size, r, g, b, a;
  bool alive;
} Ptcl;

typedef struct {
  Ptcl *pool;
  uint32_t max_p;
  float rate, accum;
  float px, py, pz;
  bool active;
  uint32_t rng;
  MopVertex *verts;
  uint32_t *indices;
  uint32_t vc, ic;
} FireSim;

static float frand(uint32_t *s) {
  uint32_t x = *s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *s = x;
  return (float)(x & 0xFFFFFF) / (float)0xFFFFFF;
}

static float frand_r(uint32_t *s, float lo, float hi) {
  return lo + frand(s) * (hi - lo);
}

static void fire_create(FireSim *f, uint32_t max_p) {
  f->pool = calloc(max_p, sizeof(Ptcl));
  f->verts = calloc(max_p * 4, sizeof(MopVertex));
  f->indices = calloc(max_p * 6, sizeof(uint32_t));
  f->max_p = max_p;
  f->rate = 60;
  f->active = true;
  f->rng = 0xCAFE1234;
}

static void fire_free(FireSim *f) {
  free(f->pool);
  free(f->verts);
  free(f->indices);
}

static void fire_update(FireSim *f, float dt, MopVec3 cam_right,
                        MopVec3 cam_up) {
  for (uint32_t i = 0; i < f->max_p; i++) {
    Ptcl *p = &f->pool[i];
    if (!p->alive)
      continue;
    p->life += dt;
    if (p->life >= p->maxlife) {
      p->alive = false;
      continue;
    }
    p->vy += 1.0f * dt;
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->z += p->vz * dt;
    float t = p->life / p->maxlife;
    p->size = 0.5f * (1.0f - t) + 0.1f * t;
    p->r = 1.0f;
    p->g = 0.8f * (1.0f - t) + 0.1f * t;
    p->b = 0.2f * (1.0f - t);
    p->a = 1.0f - t;
  }
  if (f->active) {
    f->accum += f->rate * dt;
    while (f->accum >= 1.0f) {
      f->accum -= 1.0f;
      for (uint32_t i = 0; i < f->max_p; i++) {
        if (f->pool[i].alive)
          continue;
        Ptcl *p = &f->pool[i];
        p->alive = true;
        p->x = f->px;
        p->y = f->py;
        p->z = f->pz;
        p->vx = frand_r(&f->rng, -0.3f, 0.3f);
        p->vy = frand_r(&f->rng, 1.5f, 3.0f);
        p->vz = frand_r(&f->rng, -0.3f, 0.3f);
        p->life = 0;
        p->maxlife = frand_r(&f->rng, 0.5f, 1.5f);
        p->size = 0.5f;
        p->r = 1;
        p->g = 0.8f;
        p->b = 0.2f;
        p->a = 1;
        break;
      }
    }
  }

  /* Generate camera-facing billboard quads */
  uint32_t vi = 0, ii = 0;
  for (uint32_t i = 0; i < f->max_p; i++) {
    Ptcl *p = &f->pool[i];
    if (!p->alive)
      continue;
    float hs = p->size * 0.5f;
    MopColor c = {p->r, p->g, p->b, p->a};
    MopVec3 n = {0, 0, 1};

    float rx = cam_right.x * hs, ry = cam_right.y * hs, rz = cam_right.z * hs;
    float ux = cam_up.x * hs, uy = cam_up.y * hs, uz = cam_up.z * hs;

    f->verts[vi + 0] = (MopVertex){
        .position = {p->x - rx - ux, p->y - ry - uy, p->z - rz - uz},
        .normal = n,
        .color = c};
    f->verts[vi + 1] = (MopVertex){
        .position = {p->x + rx - ux, p->y + ry - uy, p->z + rz - uz},
        .normal = n,
        .color = c};
    f->verts[vi + 2] = (MopVertex){
        .position = {p->x + rx + ux, p->y + ry + uy, p->z + rz + uz},
        .normal = n,
        .color = c};
    f->verts[vi + 3] = (MopVertex){
        .position = {p->x - rx + ux, p->y - ry + uy, p->z - rz + uz},
        .normal = n,
        .color = c};

    f->indices[ii] = vi;
    f->indices[ii + 1] = vi + 1;
    f->indices[ii + 2] = vi + 2;
    f->indices[ii + 3] = vi + 2;
    f->indices[ii + 4] = vi + 3;
    f->indices[ii + 5] = vi;
    vi += 4;
    ii += 6;
  }
  f->vc = vi;
  f->ic = ii;
}

/* =========================================================================
 * ISLAND GEOMETRY
 * ========================================================================= */

static const MopVertex ISLAND_VERTS[] = {
    {.position = {-0.8f, 0.6f, 0.8f},
     .normal = {0, 1, 0},
     .color = {0.22f, 0.55f, 0.15f, 1}},
    {.position = {0.8f, 0.6f, 0.8f},
     .normal = {0, 1, 0},
     .color = {0.22f, 0.55f, 0.15f, 1}},
    {.position = {0.8f, 0.6f, -0.8f},
     .normal = {0, 1, 0},
     .color = {0.22f, 0.55f, 0.15f, 1}},
    {.position = {-0.8f, 0.6f, -0.8f},
     .normal = {0, 1, 0},
     .color = {0.22f, 0.55f, 0.15f, 1}},
    {.position = {-0.8f, -0.4f, 0.8f},
     .normal = {0, 0, 1},
     .color = {0.45f, 0.32f, 0.18f, 1}},
    {.position = {0.8f, -0.4f, 0.8f},
     .normal = {0, 0, 1},
     .color = {0.45f, 0.32f, 0.18f, 1}},
    {.position = {0.8f, 0.6f, 0.8f},
     .normal = {0, 0, 1},
     .color = {0.45f, 0.32f, 0.18f, 1}},
    {.position = {-0.8f, 0.6f, 0.8f},
     .normal = {0, 0, 1},
     .color = {0.45f, 0.32f, 0.18f, 1}},
    {.position = {0.8f, -0.4f, -0.8f},
     .normal = {0, 0, -1},
     .color = {0.42f, 0.30f, 0.16f, 1}},
    {.position = {-0.8f, -0.4f, -0.8f},
     .normal = {0, 0, -1},
     .color = {0.42f, 0.30f, 0.16f, 1}},
    {.position = {-0.8f, 0.6f, -0.8f},
     .normal = {0, 0, -1},
     .color = {0.42f, 0.30f, 0.16f, 1}},
    {.position = {0.8f, 0.6f, -0.8f},
     .normal = {0, 0, -1},
     .color = {0.42f, 0.30f, 0.16f, 1}},
    {.position = {0.8f, -0.4f, 0.8f},
     .normal = {1, 0, 0},
     .color = {0.44f, 0.31f, 0.17f, 1}},
    {.position = {0.8f, -0.4f, -0.8f},
     .normal = {1, 0, 0},
     .color = {0.44f, 0.31f, 0.17f, 1}},
    {.position = {0.8f, 0.6f, -0.8f},
     .normal = {1, 0, 0},
     .color = {0.44f, 0.31f, 0.17f, 1}},
    {.position = {0.8f, 0.6f, 0.8f},
     .normal = {1, 0, 0},
     .color = {0.44f, 0.31f, 0.17f, 1}},
    {.position = {-0.8f, -0.4f, -0.8f},
     .normal = {-1, 0, 0},
     .color = {0.44f, 0.31f, 0.17f, 1}},
    {.position = {-0.8f, -0.4f, 0.8f},
     .normal = {-1, 0, 0},
     .color = {0.44f, 0.31f, 0.17f, 1}},
    {.position = {-0.8f, 0.6f, 0.8f},
     .normal = {-1, 0, 0},
     .color = {0.44f, 0.31f, 0.17f, 1}},
    {.position = {-0.8f, 0.6f, -0.8f},
     .normal = {-1, 0, 0},
     .color = {0.44f, 0.31f, 0.17f, 1}},
    {.position = {-0.8f, -0.4f, -0.8f},
     .normal = {0, -1, 0},
     .color = {0.35f, 0.25f, 0.12f, 1}},
    {.position = {0.8f, -0.4f, -0.8f},
     .normal = {0, -1, 0},
     .color = {0.35f, 0.25f, 0.12f, 1}},
    {.position = {0.8f, -0.4f, 0.8f},
     .normal = {0, -1, 0},
     .color = {0.35f, 0.25f, 0.12f, 1}},
    {.position = {-0.8f, -0.4f, 0.8f},
     .normal = {0, -1, 0},
     .color = {0.35f, 0.25f, 0.12f, 1}},
};
static const uint32_t ISLAND_IDX[] = {
    0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
    12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20,
};

/* -------------------------------------------------------------------------
 * Octahedron marker mesh
 * ------------------------------------------------------------------------- */

#define MARKER_SCALE 0.15f

static void make_octahedron_marker(MopVertex *verts, uint32_t *indices,
                                   MopColor color) {
  float s = MARKER_SCALE;
  MopVec3 positions[6] = {{0, s, 0},  {0, -s, 0}, {s, 0, 0},
                          {-s, 0, 0}, {0, 0, s},  {0, 0, -s}};
  uint32_t faces[24] = {0, 2, 4, 0, 4, 3, 0, 3, 5, 0, 5, 2,
                        1, 4, 2, 1, 3, 4, 1, 5, 3, 1, 2, 5};
  for (int i = 0; i < 6; i++) {
    verts[i] = (MopVertex){.position = positions[i],
                           .normal = mop_vec3_normalize(positions[i]),
                           .color = color};
  }
  memcpy(indices, faces, 24 * sizeof(uint32_t));
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

  SDL_Window *window = SDL_CreateWindow("MOP — Water Simulation", 960, 720,
                                        SDL_WINDOW_RESIZABLE);
  if (!window) {
    SDL_Quit();
    return 1;
  }

  SDL_Renderer *sdl_renderer = SDL_CreateRenderer(window, NULL);
  if (!sdl_renderer) {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(sdl_renderer, 1);

  /* ---- MOP: viewport ---- */
  int win_w = 960, win_h = 720;
  MopBackendType backend = MOP_BACKEND_CPU;
  const char *backend_env = getenv("MOP_BACKEND");
  if (backend_env) {
    if (strcmp(backend_env, "vulkan") == 0)
      backend = MOP_BACKEND_VULKAN;
    else if (strcmp(backend_env, "opengl") == 0)
      backend = MOP_BACKEND_OPENGL;
  }
  MopViewport *vp = mop_viewport_create(
      &(MopViewportDesc){.width = win_w, .height = win_h, .backend = backend});
  if (!vp) {
    fprintf(stderr, "Failed to create viewport\n");
    return 1;
  }

  mop_viewport_set_clear_color(vp, (MopColor){0.35f, 0.55f, 0.80f, 1});
  mop_viewport_set_camera(vp, (MopVec3){5, 4, 7}, (MopVec3){0, 0.3f, 0},
                          (MopVec3){0, 1, 0}, 55.0f, 0.1f, 100.0f);
  mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_SHADING,
                                          .value = MOP_SHADING_SMOOTH});
  mop_viewport_set_light_dir(vp, (MopVec3){0.4f, 1.0f, 0.3f});
  mop_viewport_set_ambient(vp, 0.25f);

  /* Static island mesh */
  mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = ISLAND_VERTS,
                                           .vertex_count = 24,
                                           .indices = ISLAND_IDX,
                                           .index_count = 36,
                                           .object_id = 1});

  uint32_t post_effects = MOP_POST_GAMMA | MOP_POST_FOG;
  mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_POST_EFFECTS,
                                          .value = post_effects});
  mop_viewport_set_fog(vp, &(MopFogParams){.color = {0.35f, 0.55f, 0.80f, 1},
                                           .near_dist = 12.0f,
                                           .far_dist = 50.0f});

  /* ---- APP: create simulations ---- */
  WaterSim water;
  water_sim_create(&water, 48, 8.0f);

  FireSim fire;
  fire_create(&fire, 512);
  fire.px = 0;
  fire.py = 0.6f;
  fire.pz = 0;

  FireSim smoke;
  fire_create(&smoke, 256);
  smoke.px = 0;
  smoke.py = 1.2f;
  smoke.pz = 0;
  smoke.rate = 30;

  /* ---- Emitter markers (octahedrons for gizmo picking) ---- */
  MopColor marker_color = {1.0f, 0.85f, 0.1f, 1.0f};
  MopVertex marker_verts[6];
  uint32_t marker_indices[24];
  make_octahedron_marker(marker_verts, marker_indices, marker_color);

#define MARKER_BASE_ID 100
  MopMesh *marker_fire =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = marker_verts,
                                               .vertex_count = 6,
                                               .indices = marker_indices,
                                               .index_count = 24,
                                               .object_id = MARKER_BASE_ID});
  mop_mesh_set_position(marker_fire, (MopVec3){fire.px, fire.py, fire.pz});

  MopMesh *marker_smoke = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = marker_verts,
                         .vertex_count = 6,
                         .indices = marker_indices,
                         .index_count = 24,
                         .object_id = MARKER_BASE_ID + 1});
  mop_mesh_set_position(marker_smoke, (MopVec3){smoke.px, smoke.py, smoke.pz});

  /* MOP mesh handles for dynamic geometry */
  MopMesh *water_mesh = NULL;
  MopMesh *fire_mesh = NULL;
  MopMesh *smoke_mesh = NULL;

  /* ---- Sidebar ---- */
  UiToolbar toolbar;
  ui_toolbar_init(&toolbar);

  ui_toolbar_section(&toolbar, "WAVE PARAMS");
  int btn_amp_up =
      ui_toolbar_button(&toolbar, "Amplitude +", UI_BTN_MOMENTARY, 0, false);
  int btn_amp_dn =
      ui_toolbar_button(&toolbar, "Amplitude -", UI_BTN_MOMENTARY, 0, false);
  int btn_freq_up =
      ui_toolbar_button(&toolbar, "Frequency +", UI_BTN_MOMENTARY, 0, false);
  int btn_freq_dn =
      ui_toolbar_button(&toolbar, "Frequency -", UI_BTN_MOMENTARY, 0, false);
  int btn_spd_up =
      ui_toolbar_button(&toolbar, "Speed +", UI_BTN_MOMENTARY, 0, false);
  int btn_spd_dn =
      ui_toolbar_button(&toolbar, "Speed -", UI_BTN_MOMENTARY, 0, false);
  int btn_opac_up =
      ui_toolbar_button(&toolbar, "Opacity +", UI_BTN_MOMENTARY, 0, false);
  int btn_opac_dn =
      ui_toolbar_button(&toolbar, "Opacity -", UI_BTN_MOMENTARY, 0, false);

  ui_toolbar_section(&toolbar, "EMITTERS");
  int btn_fire = ui_toolbar_button(&toolbar, "Fire", UI_BTN_TOGGLE, 0, true);
  int btn_smoke = ui_toolbar_button(&toolbar, "Smoke", UI_BTN_TOGGLE, 0, true);

  ui_toolbar_section(&toolbar, "POST FX");
  int btn_gamma = ui_toolbar_button(&toolbar, "Gamma", UI_BTN_TOGGLE, 0, true);
  int btn_tonemap =
      ui_toolbar_button(&toolbar, "Tonemap", UI_BTN_TOGGLE, 0, false);
  int btn_vignette =
      ui_toolbar_button(&toolbar, "Vignette", UI_BTN_TOGGLE, 0, false);
  int btn_fog = ui_toolbar_button(&toolbar, "Fog", UI_BTN_TOGGLE, 0, true);

  ui_toolbar_section(&toolbar, "SIM");
  int btn_pause = ui_toolbar_button(&toolbar, "Pause", UI_BTN_TOGGLE, 0, false);
  int btn_reset =
      ui_toolbar_button(&toolbar, "Reset", UI_BTN_MOMENTARY, 0, false);

  ui_toolbar_layout(&toolbar);

  /* ---- State ---- */
  bool paused = false;
  float sim_time = 0;

  SDL_Texture *tex =
      SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ABGR8888,
                        SDL_TEXTUREACCESS_STREAMING, win_w, win_h);

  Uint64 last = SDL_GetPerformanceCounter();
  Uint64 freq_ = SDL_GetPerformanceFrequency();
  bool running = true;

  while (running) {
    Uint64 now = SDL_GetPerformanceCounter();
    float dt = (float)(now - last) / (float)freq_;
    last = now;
    if (dt > 0.1f)
      dt = 0.1f;

    /* ---- Events ---- */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_EVENT_QUIT) {
        running = false;
        continue;
      }

      /* Keyboard shortcuts — toggle toolbar buttons so UI stays in sync */
      if (ev.type == SDL_EVENT_KEY_DOWN) {
        switch (ev.key.key) {
        case SDLK_ESCAPE:
          running = false;
          continue;

        /* Wave params — fire the momentary buttons */
        case SDLK_D:
          ui_toolbar_toggle(&toolbar, btn_amp_up);
          break;
        case SDLK_A:
          ui_toolbar_toggle(&toolbar, btn_amp_dn);
          break;
        case SDLK_W:
          ui_toolbar_toggle(&toolbar, btn_freq_up);
          break;
        case SDLK_S:
          ui_toolbar_toggle(&toolbar, btn_freq_dn);
          break;
        case SDLK_E:
          ui_toolbar_toggle(&toolbar, btn_spd_up);
          break;
        case SDLK_Q:
          ui_toolbar_toggle(&toolbar, btn_spd_dn);
          break;
        case SDLK_P:
          ui_toolbar_toggle(&toolbar, btn_opac_up);
          break;
        case SDLK_O:
          ui_toolbar_toggle(&toolbar, btn_opac_dn);
          break;

        /* Emitter toggles */
        case SDLK_1:
          ui_toolbar_toggle(&toolbar, btn_fire);
          break;
        case SDLK_2:
          ui_toolbar_toggle(&toolbar, btn_smoke);
          break;

        /* Post-processing toggles */
        case SDLK_G:
          ui_toolbar_toggle(&toolbar, btn_gamma);
          break;
        case SDLK_T:
          ui_toolbar_toggle(&toolbar, btn_tonemap);
          break;
        case SDLK_V:
          ui_toolbar_toggle(&toolbar, btn_vignette);
          break;
        case SDLK_F:
          ui_toolbar_toggle(&toolbar, btn_fog);
          break;

        /* Sim controls */
        case SDLK_SPACE:
          ui_toolbar_toggle(&toolbar, btn_pause);
          break;
        case SDLK_R:
          ui_toolbar_toggle(&toolbar, btn_reset);
          break;

        default:
          break;
        }
        continue;
      }

      /* Sidebar consumes mouse events in its area */
      if (ui_toolbar_event(&toolbar, &ev))
        continue;

      /* Forward to MOP */
      switch (ev.type) {
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev.button.button == SDL_BUTTON_LEFT)
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_POINTER_DOWN,
                                                  ev.button.x, ev.button.y});
        else if (ev.button.button == SDL_BUTTON_RIGHT)
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_SECONDARY_DOWN,
                                                  ev.button.x, ev.button.y});
        break;
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev.button.button == SDL_BUTTON_LEFT)
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_POINTER_UP,
                                                  ev.button.x, ev.button.y});
        else if (ev.button.button == SDL_BUTTON_RIGHT)
          mop_viewport_input(vp,
                             &(MopInputEvent){.type = MOP_INPUT_SECONDARY_UP});
        break;
      case SDL_EVENT_MOUSE_MOTION:
        mop_viewport_input(
            vp, &(MopInputEvent){MOP_INPUT_POINTER_MOVE, ev.motion.x,
                                 ev.motion.y, ev.motion.xrel, ev.motion.yrel});
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SCROLL,
                                                .scroll = ev.wheel.y});
        break;
      case SDL_EVENT_WINDOW_RESIZED: {
        int w = ev.window.data1, h = ev.window.data2;
        if (w > 0 && h > 0) {
          win_w = w;
          win_h = h;
          mop_viewport_resize(vp, w, h);
          SDL_DestroyTexture(tex);
          tex = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
        }
        break;
      }
      default:
        break;
      }
    }

    /* ---- Poll MOP events (gizmo transforms) ---- */
    MopEvent mop_ev;
    while (mop_viewport_poll_event(vp, &mop_ev)) {
      if (mop_ev.type == MOP_EVENT_TRANSFORM_CHANGED) {
        if (mop_ev.object_id == MARKER_BASE_ID) {
          fire.px = mop_ev.position.x;
          fire.py = mop_ev.position.y;
          fire.pz = mop_ev.position.z;
        } else if (mop_ev.object_id == MARKER_BASE_ID + 1) {
          smoke.px = mop_ev.position.x;
          smoke.py = mop_ev.position.y;
          smoke.pz = mop_ev.position.z;
        }
      }
    }

    /* ---- Sync sidebar state ---- */

    /* Wave parameter buttons (momentary) */
    if (ui_toolbar_fired(&toolbar, btn_amp_up)) {
      water.wave_amplitude += 0.02f;
      if (water.wave_amplitude > 1.0f)
        water.wave_amplitude = 1.0f;
    }
    if (ui_toolbar_fired(&toolbar, btn_amp_dn)) {
      water.wave_amplitude -= 0.02f;
      if (water.wave_amplitude < 0.01f)
        water.wave_amplitude = 0.01f;
    }
    if (ui_toolbar_fired(&toolbar, btn_freq_up)) {
      water.wave_frequency += 0.2f;
      if (water.wave_frequency > 10.0f)
        water.wave_frequency = 10.0f;
    }
    if (ui_toolbar_fired(&toolbar, btn_freq_dn)) {
      water.wave_frequency -= 0.2f;
      if (water.wave_frequency < 0.2f)
        water.wave_frequency = 0.2f;
    }
    if (ui_toolbar_fired(&toolbar, btn_spd_up)) {
      water.wave_speed += 0.2f;
      if (water.wave_speed > 5.0f)
        water.wave_speed = 5.0f;
    }
    if (ui_toolbar_fired(&toolbar, btn_spd_dn)) {
      water.wave_speed -= 0.2f;
      if (water.wave_speed < 0.1f)
        water.wave_speed = 0.1f;
    }
    if (ui_toolbar_fired(&toolbar, btn_opac_up)) {
      water.opacity += 0.05f;
      if (water.opacity > 1.0f)
        water.opacity = 1.0f;
    }
    if (ui_toolbar_fired(&toolbar, btn_opac_dn)) {
      water.opacity -= 0.05f;
      if (water.opacity < 0.05f)
        water.opacity = 0.05f;
    }

    fire.active = ui_toolbar_is_on(&toolbar, btn_fire);
    smoke.active = ui_toolbar_is_on(&toolbar, btn_smoke);
    paused = ui_toolbar_is_on(&toolbar, btn_pause);

    if (ui_toolbar_fired(&toolbar, btn_reset)) {
      sim_time = 0;
    }

    /* Post-processing — send via MOP event */
    post_effects = 0;
    if (ui_toolbar_is_on(&toolbar, btn_gamma))
      post_effects |= MOP_POST_GAMMA;
    if (ui_toolbar_is_on(&toolbar, btn_tonemap))
      post_effects |= MOP_POST_TONEMAP;
    if (ui_toolbar_is_on(&toolbar, btn_vignette))
      post_effects |= MOP_POST_VIGNETTE;
    if (ui_toolbar_is_on(&toolbar, btn_fog))
      post_effects |= MOP_POST_FOG;
    mop_viewport_input(vp, &(MopInputEvent){.type = MOP_INPUT_SET_POST_EFFECTS,
                                            .value = post_effects});

    /* ---- Update marker positions ---- */
    mop_mesh_set_position(marker_fire, (MopVec3){fire.px, fire.py, fire.pz});
    mop_mesh_set_position(marker_smoke,
                          (MopVec3){smoke.px, smoke.py, smoke.pz});

    /* ================================================================
     * APP SIMULATION STEP
     * ================================================================ */

    /* Camera basis for billboard orientation */
    MopVec3 cam_eye = mop_viewport_get_camera_eye(vp);
    MopVec3 cam_target = mop_viewport_get_camera_target(vp);
    MopVec3 fwd = mop_vec3_normalize(mop_vec3_sub(cam_target, cam_eye));
    MopVec3 world_up = {0, 1, 0};
    MopVec3 cam_right = mop_vec3_normalize(mop_vec3_cross(fwd, world_up));
    MopVec3 cam_up = mop_vec3_cross(cam_right, fwd);

    if (!paused) {
      sim_time += dt;
      water_sim_update(&water, sim_time);
      fire_update(&fire, dt, cam_right, cam_up);
      fire_update(&smoke, dt, cam_right, cam_up);
    }

    /* ---- Submit to MOP ---- */

    /* Water mesh */
    if (!water_mesh) {
      water_mesh = mop_viewport_add_mesh(
          vp, &(MopMeshDesc){.vertices = water.verts,
                             .vertex_count = water_vertex_count(&water),
                             .indices = water.indices,
                             .index_count = water_index_count(&water),
                             .object_id = 2});
      if (water_mesh)
        mop_mesh_set_blend_mode(water_mesh, MOP_BLEND_ALPHA);
    } else {
      mop_mesh_update_geometry(water_mesh, vp, water.verts,
                               water_vertex_count(&water), water.indices,
                               water_index_count(&water));
    }
    if (water_mesh)
      mop_mesh_set_opacity(water_mesh, water.opacity);

    /* Fire mesh */
    if (fire.vc > 0) {
      if (!fire_mesh) {
        fire_mesh =
            mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = fire.verts,
                                                     .vertex_count = fire.vc,
                                                     .indices = fire.indices,
                                                     .index_count = fire.ic,
                                                     .object_id = 3});
        if (fire_mesh)
          mop_mesh_set_blend_mode(fire_mesh, MOP_BLEND_ADDITIVE);
      } else {
        mop_mesh_update_geometry(fire_mesh, vp, fire.verts, fire.vc,
                                 fire.indices, fire.ic);
      }
    } else if (fire_mesh) {
      mop_viewport_remove_mesh(vp, fire_mesh);
      fire_mesh = NULL;
    }

    /* Smoke mesh */
    if (smoke.vc > 0) {
      if (!smoke_mesh) {
        smoke_mesh =
            mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = smoke.verts,
                                                     .vertex_count = smoke.vc,
                                                     .indices = smoke.indices,
                                                     .index_count = smoke.ic,
                                                     .object_id = 4});
        if (smoke_mesh) {
          mop_mesh_set_blend_mode(smoke_mesh, MOP_BLEND_ALPHA);
          mop_mesh_set_opacity(smoke_mesh, 0.6f);
        }
      } else {
        mop_mesh_update_geometry(smoke_mesh, vp, smoke.verts, smoke.vc,
                                 smoke.indices, smoke.ic);
      }
    } else if (smoke_mesh) {
      mop_viewport_remove_mesh(vp, smoke_mesh);
      smoke_mesh = NULL;
    }

    /* ---- MOP: render ---- */
    mop_viewport_render(vp);

    /* ---- Blit ---- */
    int fb_w, fb_h;
    const uint8_t *px = mop_viewport_read_color(vp, &fb_w, &fb_h);
    if (px && tex) {
      SDL_UpdateTexture(tex, NULL, px, fb_w * 4);
      SDL_RenderClear(sdl_renderer);
      SDL_RenderTexture(sdl_renderer, tex, NULL, NULL);

      /* Draw sidebar on top */
      ui_toolbar_render(&toolbar, sdl_renderer, win_h);

      SDL_RenderPresent(sdl_renderer);
    }
  }

  /* ---- Cleanup ---- */
  if (water_mesh)
    mop_viewport_remove_mesh(vp, water_mesh);
  if (fire_mesh)
    mop_viewport_remove_mesh(vp, fire_mesh);
  if (smoke_mesh)
    mop_viewport_remove_mesh(vp, smoke_mesh);
  mop_viewport_remove_mesh(vp, marker_fire);
  mop_viewport_remove_mesh(vp, marker_smoke);
  water_sim_free(&water);
  fire_free(&fire);
  fire_free(&smoke);
  SDL_DestroyTexture(tex);
  mop_viewport_destroy(vp);
  SDL_DestroyRenderer(sdl_renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
