/*
 * Master of Puppets — Phase 1 Showcase Demo (SDL3)
 *
 * Exercises every Phase 1 feature in a single interactive scene:
 *   - Multi-light shading (directional + 2 orbiting points + 1 spot)
 *   - Wireframe-on-shaded overlay
 *   - Vertex normals overlay
 *   - Bounding box overlay
 *   - Selection highlight overlay
 *   - Flexible vertex format (mop_viewport_add_mesh_ex + CUSTOM0)
 *   - Display settings (live toolbar control)
 *
 * Scene: 3 meshes (cube, UV sphere, flex heat-plane) lit by 4 lights.
 * Left sidebar toolbar toggles every overlay and light in real time.
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
 * Constants
 * ========================================================================= */

#define WINDOW_W 960
#define WINDOW_H 720

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 * Cube geometry — X-macro generated (24 verts, 36 indices)
 * ========================================================================= */

#define CUBE_FACES(F)                                                          \
  /*    nx  ny  nz    r     g     b       v0            v1            v2 v3 */ \
  F(0, 0, 1, 0.9f, 0.2f, 0.2f, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1,            \
    1) /* front  */                                                            \
  F(0, 0, -1, 0.2f, 0.9f, 0.2f, 1, -1, -1, -1, -1, -1, -1, 1, -1, 1, 1,        \
    -1) /* back   */                                                           \
  F(0, 1, 0, 0.2f, 0.2f, 0.9f, -1, 1, 1, 1, 1, 1, 1, 1, -1, -1, 1,             \
    -1) /* top    */                                                           \
  F(0, -1, 0, 0.9f, 0.9f, 0.2f, -1, -1, -1, 1, -1, -1, 1, -1, 1, -1, -1,       \
    1) /* bottom */                                                            \
  F(1, 0, 0, 0.2f, 0.9f, 0.9f, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1, 1,            \
    1) /* right  */                                                            \
  F(-1, 0, 0, 0.9f, 0.2f, 0.9f, -1, -1, -1, -1, -1, 1, -1, 1, 1, -1, 1,        \
    -1) /* left   */

#define FACE_VERTS(nx, ny, nz, cr, cg, cb, x0, y0, z0, x1, y1, z1, x2, y2, z2, \
                   x3, y3, z3)                                                 \
  {{.5f * (x0), .5f * (y0), .5f * (z0)}, {nx, ny, nz}, {cr, cg, cb, 1}, 0, 0}, \
      {{.5f * (x1), .5f * (y1), .5f * (z1)},                                   \
       {nx, ny, nz},                                                           \
       {cr, cg, cb, 1},                                                        \
       0,                                                                      \
       0},                                                                     \
      {{.5f * (x2), .5f * (y2), .5f * (z2)},                                   \
       {nx, ny, nz},                                                           \
       {cr, cg, cb, 1},                                                        \
       0,                                                                      \
       0},                                                                     \
      {{.5f * (x3), .5f * (y3), .5f * (z3)},                                   \
       {nx, ny, nz},                                                           \
       {cr, cg, cb, 1},                                                        \
       0,                                                                      \
       0},

static const MopVertex CUBE_VERTS[] = {CUBE_FACES(FACE_VERTS)};

static const uint32_t CUBE_INDICES[] = {
    0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
    12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20,
};

#define CUBE_VERT_COUNT (sizeof(CUBE_VERTS) / sizeof(CUBE_VERTS[0]))
#define CUBE_INDEX_COUNT (sizeof(CUBE_INDICES) / sizeof(CUBE_INDICES[0]))

/* =========================================================================
 * UV Sphere generation
 * ========================================================================= */

#define SPHERE_LAT 20
#define SPHERE_LON 32
#define SPHERE_VERT_COUNT ((SPHERE_LAT + 1) * (SPHERE_LON + 1))
#define SPHERE_INDEX_COUNT (SPHERE_LAT * SPHERE_LON * 6)

static MopVertex g_sphere_verts[SPHERE_VERT_COUNT];
static uint32_t g_sphere_indices[SPHERE_INDEX_COUNT];

static void generate_sphere(float radius) {
  int vi = 0;
  for (int lat = 0; lat <= SPHERE_LAT; lat++) {
    float theta = (float)lat / (float)SPHERE_LAT * (float)M_PI;
    float sin_t = sinf(theta);
    float cos_t = cosf(theta);

    for (int lon = 0; lon <= SPHERE_LON; lon++) {
      float phi = (float)lon / (float)SPHERE_LON * 2.0f * (float)M_PI;
      float sin_p = sinf(phi);
      float cos_p = cosf(phi);

      float nx = sin_t * cos_p;
      float ny = cos_t;
      float nz = sin_t * sin_p;

      g_sphere_verts[vi] = (MopVertex){
          .position = {radius * nx, radius * ny, radius * nz},
          .normal = {nx, ny, nz},
          .color = {0.7f, 0.7f, 0.8f, 1.0f},
          .u = (float)lon / (float)SPHERE_LON,
          .v = (float)lat / (float)SPHERE_LAT,
      };
      vi++;
    }
  }

  int ii = 0;
  for (int lat = 0; lat < SPHERE_LAT; lat++) {
    for (int lon = 0; lon < SPHERE_LON; lon++) {
      int a = lat * (SPHERE_LON + 1) + lon;
      int b = a + (SPHERE_LON + 1);

      g_sphere_indices[ii++] = (uint32_t)a;
      g_sphere_indices[ii++] = (uint32_t)b;
      g_sphere_indices[ii++] = (uint32_t)(a + 1);

      g_sphere_indices[ii++] = (uint32_t)(a + 1);
      g_sphere_indices[ii++] = (uint32_t)b;
      g_sphere_indices[ii++] = (uint32_t)(b + 1);
    }
  }
}

/* =========================================================================
 * Flex plane — custom vertex format with CUSTOM0 heat channel
 * ========================================================================= */

typedef struct {
  float px, py, pz;     /* POSITION  (float3, offset 0)  */
  float nx, ny, nz;     /* NORMAL    (float3, offset 12) */
  float cr, cg, cb, ca; /* COLOR     (float4, offset 24) */
  float u, v;           /* TEXCOORD0 (float2, offset 40) */
  float h0, h1, h2, h3; /* CUSTOM0   (float4, offset 48) */
} FlexVertex;           /* stride = 64 bytes */

#define PLANE_RES 16
#define PLANE_VERT_COUNT ((PLANE_RES + 1) * (PLANE_RES + 1))
#define PLANE_INDEX_COUNT (PLANE_RES * PLANE_RES * 6)

static FlexVertex g_plane_verts[PLANE_VERT_COUNT];
static uint32_t g_plane_indices[PLANE_INDEX_COUNT];

static void generate_flex_plane(float extent) {
  int vi = 0;
  float half = extent * 0.5f;

  for (int row = 0; row <= PLANE_RES; row++) {
    for (int col = 0; col <= PLANE_RES; col++) {
      float u = (float)col / (float)PLANE_RES;
      float v = (float)row / (float)PLANE_RES;

      float x = -half + u * extent;
      float z = -half + v * extent;

      /* Radial heat: 1.0 at center, 0.0 at edges */
      float dx = u - 0.5f;
      float dz = v - 0.5f;
      float dist = sqrtf(dx * dx + dz * dz) * 2.0f; /* 0..~1.4 */
      float heat = 1.0f - dist;
      if (heat < 0.0f)
        heat = 0.0f;

      /* Color from heat: blue → red */
      float cr = heat;
      float cb = 1.0f - heat;

      g_plane_verts[vi] = (FlexVertex){
          .px = x,
          .py = 0.0f,
          .pz = z,
          .nx = 0.0f,
          .ny = 1.0f,
          .nz = 0.0f,
          .cr = cr,
          .cg = 0.2f,
          .cb = cb,
          .ca = 1.0f,
          .u = u,
          .v = v,
          .h0 = heat,
          .h1 = 0.0f,
          .h2 = 0.0f,
          .h3 = 0.0f,
      };
      vi++;
    }
  }

  int ii = 0;
  for (int row = 0; row < PLANE_RES; row++) {
    for (int col = 0; col < PLANE_RES; col++) {
      int a = row * (PLANE_RES + 1) + col;
      int b = a + (PLANE_RES + 1);

      g_plane_indices[ii++] = (uint32_t)a;
      g_plane_indices[ii++] = (uint32_t)b;
      g_plane_indices[ii++] = (uint32_t)(a + 1);

      g_plane_indices[ii++] = (uint32_t)(a + 1);
      g_plane_indices[ii++] = (uint32_t)b;
      g_plane_indices[ii++] = (uint32_t)(b + 1);
    }
  }
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  /* ---- SDL3 init ---- */
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window *window = SDL_CreateWindow("MOP — Phase 1 Showcase", WINDOW_W,
                                        WINDOW_H, SDL_WINDOW_RESIZABLE);
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
  SDL_SetRenderVSync(renderer, 1);

  /* ---- MOP viewport ---- */
  MopBackendType backend = MOP_BACKEND_CPU;
  const char *backend_env = getenv("MOP_BACKEND");
  if (backend_env) {
    if (strcmp(backend_env, "vulkan") == 0)
      backend = MOP_BACKEND_VULKAN;
    else if (strcmp(backend_env, "opengl") == 0)
      backend = MOP_BACKEND_OPENGL;
  }
  MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
      .width = WINDOW_W, .height = WINDOW_H, .backend = backend});
  if (!vp) {
    fprintf(stderr, "Failed to create MOP viewport\n");
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  mop_viewport_set_clear_color(vp, (MopColor){.12f, .12f, .16f, 1});

  /* ---- Camera ---- */
  mop_viewport_set_camera(vp, (MopVec3){0, 3, 8}, /* eye */
                          (MopVec3){0, 0.5f, 0},  /* target */
                          (MopVec3){0, 1, 0},     /* up */
                          45.0f, 0.1f, 100.0f);

  /* ---- Generate geometry ---- */
  generate_sphere(0.6f);
  generate_flex_plane(2.0f);

  /* ---- Add meshes ---- */

  /* Cube (object_id=1): standard MopVertex, per-face colors */
  MopMesh *cube = mop_viewport_add_mesh(vp, &(MopMeshDesc){
                                                .vertices = CUBE_VERTS,
                                                .vertex_count = CUBE_VERT_COUNT,
                                                .indices = CUBE_INDICES,
                                                .index_count = CUBE_INDEX_COUNT,
                                                .object_id = 1,
                                            });
  mop_mesh_set_position(cube, (MopVec3){-2.0f, 0.5f, 0.0f});

  /* Sphere (object_id=2): standard MopVertex, smooth normals */
  MopMesh *sphere =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){
                                    .vertices = g_sphere_verts,
                                    .vertex_count = SPHERE_VERT_COUNT,
                                    .indices = g_sphere_indices,
                                    .index_count = SPHERE_INDEX_COUNT,
                                    .object_id = 2,
                                });
  mop_mesh_set_position(sphere, (MopVec3){0.0f, 0.6f, 0.0f});

  /* Flex plane (object_id=3): custom vertex format with CUSTOM0 heat */
  MopVertexFormat flex_fmt = {
      .attrib_count = 6,
      .stride = sizeof(FlexVertex),
      .attribs =
          {
              {MOP_ATTRIB_POSITION, MOP_FORMAT_FLOAT3,
               offsetof(FlexVertex, px)},
              {MOP_ATTRIB_NORMAL, MOP_FORMAT_FLOAT3, offsetof(FlexVertex, nx)},
              {MOP_ATTRIB_COLOR, MOP_FORMAT_FLOAT4, offsetof(FlexVertex, cr)},
              {MOP_ATTRIB_TEXCOORD0, MOP_FORMAT_FLOAT2,
               offsetof(FlexVertex, u)},
              {MOP_ATTRIB_CUSTOM0, MOP_FORMAT_FLOAT4, offsetof(FlexVertex, h0)},
              {0}, /* sentinel */
          },
  };

  MopMesh *plane =
      mop_viewport_add_mesh_ex(vp, &(MopMeshDescEx){
                                       .vertex_data = g_plane_verts,
                                       .vertex_count = PLANE_VERT_COUNT,
                                       .indices = g_plane_indices,
                                       .index_count = PLANE_INDEX_COUNT,
                                       .object_id = 3,
                                       .vertex_format = &flex_fmt,
                                   });
  mop_mesh_set_position(plane, (MopVec3){2.5f, 0.0f, 0.0f});

  /* ---- Multi-light setup ---- */

  /* Light 0: directional key light (synced with legacy API) */
  mop_viewport_set_light_dir(vp, (MopVec3){0.3f, 1.0f, 0.5f});
  mop_viewport_set_ambient(vp, 0.15f);

  MopLight dir_light = {
      .type = MOP_LIGHT_DIRECTIONAL,
      .direction = {0.3f, 1.0f, 0.5f},
      .color = {1.0f, 1.0f, 0.95f, 1.0f},
      .intensity = 1.0f,
      .active = true,
  };
  MopLight *light_dir = mop_viewport_add_light(vp, &dir_light);

  /* Point A: warm orbiting light */
  MopLight pt_a_desc = {
      .type = MOP_LIGHT_POINT,
      .position = {3.0f, 2.0f, 0.0f},
      .color = {1.0f, 0.6f, 0.2f, 1.0f},
      .intensity = 1.5f,
      .range = 15.0f,
      .active = true,
  };
  MopLight *light_pt_a = mop_viewport_add_light(vp, &pt_a_desc);

  /* Point B: cool orbiting light */
  MopLight pt_b_desc = {
      .type = MOP_LIGHT_POINT,
      .position = {-3.0f, 2.0f, 0.0f},
      .color = {0.2f, 0.5f, 1.0f, 1.0f},
      .intensity = 1.5f,
      .range = 15.0f,
      .active = true,
  };
  MopLight *light_pt_b = mop_viewport_add_light(vp, &pt_b_desc);

  /* Spot: downward */
  MopLight spot_desc = {
      .type = MOP_LIGHT_SPOT,
      .position = {0.0f, 5.0f, 0.0f},
      .direction = {0.0f, -1.0f, 0.0f},
      .color = {1.0f, 1.0f, 0.8f, 1.0f},
      .intensity = 2.0f,
      .range = 20.0f,
      .spot_inner_cos = 0.95f, /* ~18 degrees */
      .spot_outer_cos = 0.85f, /* ~32 degrees */
      .active = true,
  };
  MopLight *light_spot = mop_viewport_add_light(vp, &spot_desc);

  /* ---- Overlay defaults ---- */
  mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_WIREFRAME, false);
  mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_NORMALS, false);
  mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_BOUNDS, false);
  mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_SELECTION, true);

  /* ---- Toolbar ---- */
  UiToolbar tb;
  ui_toolbar_init(&tb);

  /* LIGHTING section */
  ui_toolbar_section(&tb, "LIGHTING");
  int btn_key_light =
      ui_toolbar_button(&tb, "Key Light", UI_BTN_TOGGLE, 0, true);
  int btn_point_a = ui_toolbar_button(&tb, "Point A", UI_BTN_TOGGLE, 0, true);
  int btn_point_b = ui_toolbar_button(&tb, "Point B", UI_BTN_TOGGLE, 0, true);
  int btn_spot = ui_toolbar_button(&tb, "Spot Light", UI_BTN_TOGGLE, 0, true);

  /* OVERLAYS section */
  ui_toolbar_section(&tb, "OVERLAYS");
  int btn_wireframe =
      ui_toolbar_button(&tb, "Wireframe", UI_BTN_TOGGLE, 0, false);
  int btn_normals = ui_toolbar_button(&tb, "Normals", UI_BTN_TOGGLE, 0, false);
  int btn_bounds = ui_toolbar_button(&tb, "Bounds", UI_BTN_TOGGLE, 0, false);
  int btn_selection =
      ui_toolbar_button(&tb, "Selection", UI_BTN_TOGGLE, 0, true);

  /* SHADING section — radio group 1 */
  ui_toolbar_section(&tb, "SHADING");
  /* btn_flat only exists to be the default radio selection; we check btn_smooth
   */
  (void)ui_toolbar_button(&tb, "Flat", UI_BTN_RADIO, 1, true);
  int btn_smooth = ui_toolbar_button(&tb, "Smooth", UI_BTN_RADIO, 1, false);

  /* SCENE section */
  ui_toolbar_section(&tb, "SCENE");
  int btn_auto_rotate =
      ui_toolbar_button(&tb, "Auto-Rotate", UI_BTN_TOGGLE, 0, true);

  ui_toolbar_layout(&tb);

  /* ---- SDL texture for CPU framebuffer blit ---- */
  int win_w = WINDOW_W, win_h = WINDOW_H;
  SDL_Texture *tex =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                        SDL_TEXTUREACCESS_STREAMING, win_w, win_h);

  /* ---- Timing ---- */
  Uint64 last = SDL_GetPerformanceCounter();
  Uint64 freq = SDL_GetPerformanceFrequency();
  float time_accum = 0.0f;

  bool running = true;
  bool animate_pt_a = true; /* stop animation when user drags a light */
  bool animate_pt_b = true;

  printf("MOP — Phase 1 Showcase Demo\n");
  printf("  3 meshes  |  4 lights  |  all overlays  |  flex vertex format\n");
  printf("  Left-drag: orbit  |  Right-drag: pan  |  Scroll: zoom\n");
  printf("  Click meshes to select  |  Toolbar on the left\n");

  /* ---- Event loop ---- */
  while (running) {
    Uint64 now = SDL_GetPerformanceCounter();
    float dt = (float)(now - last) / (float)freq;
    last = now;
    time_accum += dt;

    /* ---- Process SDL events ---- */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      /* Quit */
      if (ev.type == SDL_EVENT_QUIT) {
        running = false;
        continue;
      }

      /* Toolbar consumes mouse events in sidebar area */
      if (ui_toolbar_event(&tb, &ev))
        continue;

      /* Forward to MOP */
      switch (ev.type) {
      case SDL_EVENT_KEY_DOWN:
        switch (ev.key.key) {
        case SDLK_Q:
        case SDLK_ESCAPE:
          running = false;
          break;
        default:
          break;
        }
        break;

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev.button.button == SDL_BUTTON_LEFT)
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_POINTER_DOWN,
                                                  ev.button.x, ev.button.y, 0,
                                                  0, 0});
        else if (ev.button.button == SDL_BUTTON_RIGHT)
          mop_viewport_input(vp, &(MopInputEvent){MOP_INPUT_SECONDARY_DOWN,
                                                  ev.button.x, ev.button.y, 0,
                                                  0, 0});
        break;

      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev.button.button == SDL_BUTTON_LEFT)
          mop_viewport_input(vp,
                             &(MopInputEvent){MOP_INPUT_POINTER_UP, ev.button.x,
                                              ev.button.y, 0, 0, 0});
        else if (ev.button.button == SDL_BUTTON_RIGHT)
          mop_viewport_input(vp,
                             &(MopInputEvent){.type = MOP_INPUT_SECONDARY_UP});
        break;

      case SDL_EVENT_MOUSE_MOTION:
        mop_viewport_input(vp,
                           &(MopInputEvent){MOP_INPUT_POINTER_MOVE, ev.motion.x,
                                            ev.motion.y, ev.motion.xrel,
                                            ev.motion.yrel, 0});
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
          tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
        }
        break;
      }

      default:
        break;
      }
    }

    /* ---- Poll MOP events ---- */
    MopEvent mev;
    while (mop_viewport_poll_event(vp, &mev)) {
      if (mev.type == MOP_EVENT_SELECTED)
        printf("Selected object %u\n", mev.object_id);
      if (mev.type == MOP_EVENT_DESELECTED)
        printf("Deselected\n");
      if (mev.type == MOP_EVENT_LIGHT_CHANGED) {
        /* Stop animating lights that the user has moved.
         * Light indices: 0=directional, 1=pt_a, 2=pt_b, 3=spot */
        uint32_t li = mev.object_id - 0xFFFE0000u;
        if (li == 1)
          animate_pt_a = false;
        if (li == 2)
          animate_pt_b = false;
        printf("Light %u changed\n", li);
      }
    }

    /* ---- Sync toolbar → lights ---- */
    light_dir->active = ui_toolbar_is_on(&tb, btn_key_light);
    light_pt_a->active = ui_toolbar_is_on(&tb, btn_point_a);
    light_pt_b->active = ui_toolbar_is_on(&tb, btn_point_b);
    light_spot->active = ui_toolbar_is_on(&tb, btn_spot);

    /* ---- Sync toolbar → overlays + display settings ---- */
    {
      bool wire = ui_toolbar_is_on(&tb, btn_wireframe);
      bool norm = ui_toolbar_is_on(&tb, btn_normals);
      bool bnds = ui_toolbar_is_on(&tb, btn_bounds);
      bool sel = ui_toolbar_is_on(&tb, btn_selection);

      MopDisplaySettings ds = mop_viewport_get_display(vp);
      ds.wireframe_overlay = wire;
      ds.show_normals = norm;
      ds.show_bounds = bnds;
      mop_viewport_set_display(vp, &ds);

      mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_WIREFRAME, wire);
      mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_NORMALS, norm);
      mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_BOUNDS, bnds);
      mop_viewport_set_overlay_enabled(vp, MOP_OVERLAY_SELECTION, sel);
    }

    /* ---- Sync toolbar → shading mode ---- */
    if (ui_toolbar_is_on(&tb, btn_smooth))
      mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);
    else
      mop_viewport_set_shading(vp, MOP_SHADING_FLAT);

    /* ---- Animate orbiting point lights ---- */
    {
      float t = time_accum;

      /* Point A: CW orbit (stops when user drags the light) */
      if (animate_pt_a) {
        mop_light_set_position(light_pt_a,
                               (MopVec3){3.0f * cosf(t), 2.0f, 3.0f * sinf(t)});
      }

      /* Point B: CCW orbit, different speed */
      if (animate_pt_b) {
        mop_light_set_position(
            light_pt_b,
            (MopVec3){3.0f * cosf(-t * 0.7f), 2.0f, 3.0f * sinf(-t * 0.7f)});
      }
    }

    /* ---- Auto-rotate cube ---- */
    if (ui_toolbar_is_on(&tb, btn_auto_rotate)) {
      MopVec3 r = mop_mesh_get_rotation(cube);
      r.y += dt * 0.8f;
      mop_mesh_set_rotation(cube, r);
    }

    /* ---- Render ---- */
    mop_viewport_set_time(vp, time_accum);
    mop_viewport_render(vp);

    /* ---- Blit CPU framebuffer → SDL texture → window ---- */
    int fb_w, fb_h;
    const uint8_t *px = mop_viewport_read_color(vp, &fb_w, &fb_h);
    if (px && tex) {
      SDL_UpdateTexture(tex, NULL, px, fb_w * 4);
      SDL_RenderClear(renderer);
      SDL_RenderTexture(renderer, tex, NULL, NULL);

      /* Draw toolbar over the MOP framebuffer */
      ui_toolbar_render(&tb, renderer, win_h);

      SDL_RenderPresent(renderer);
    }
  }

  /* ---- Cleanup ---- */
  printf("Shutting down...\n");
  SDL_DestroyTexture(tex);
  mop_viewport_destroy(vp);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("Clean shutdown.\n");
  return 0;
}
