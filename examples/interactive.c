/*
 * Master of Puppets — Interactive Showcase
 *
 * Companion to showcase.c: same scene, but driven by a live SDL2 window
 * instead of a frame-dumping worker thread. Demonstrates the embed path
 * for a real DCC or game-engine viewport:
 *
 *   1. Host owns the window (SDL2). MOP never touches the OS.
 *   2. Host translates platform events → MopInputEvent via
 *      mop_viewport_input(). MOP handles click-vs-drag, selection,
 *      gizmo manipulation, orbit/pan/zoom internally.
 *   3. Each frame, host calls mop_viewport_render() then
 *      mop_viewport_read_color() and blits the RGBA buffer to an
 *      SDL_Texture. That texture is the presentation surface.
 *   4. Host polls MopEvent via mop_viewport_poll_event() for
 *      selection / transform / shading state changes and reacts
 *      (here: prints to stderr).
 *
 * Mouse:
 *   Left drag           — orbit camera (or drag gizmo if one is selected)
 *   Ctrl + Left drag    — zoom (dolly)
 *   Right drag          — pan
 *   Two-finger scroll   — orbit camera
 *   Left click on mesh  — select
 *   Left click on void  — deselect
 *
 * Keyboard:
 *   W / E / R           — gizmo translate / rotate / scale
 *   1 / 2 / 3           — shading: flat / smooth / wireframe
 *   F                   — reset view
 *   Ctrl+Z / Ctrl+Y     — undo / redo
 *   Esc / Q             — quit
 *
 *   Usage:  nix run .#interactive
 *           ./build/examples/interactive [--hdri PATH] [--vulkan]
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>

#include <mop/core/theme.h>

#include <SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 * Procedural geometry — identical to showcase.c, kept local to avoid a
 * shared header. CCW winding throughout (MOP convention).
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
  const int S = 32, R = 48;
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
 * SDL → MOP translation
 * ========================================================================= */

static uint32_t sdl_modifiers(SDL_Keymod km) {
  uint32_t m = 0;
  if (km & KMOD_SHIFT)
    m |= MOP_MOD_SHIFT;
  if (km & KMOD_CTRL)
    m |= MOP_MOD_CTRL;
  if (km & KMOD_GUI) /* macOS: Cmd maps to Ctrl semantically */
    m |= MOP_MOD_CTRL;
  if (km & KMOD_ALT)
    m |= MOP_MOD_ALT;
  return m;
}

static const char *event_name(MopEventType t) {
  switch (t) {
  case MOP_EVENT_SELECTED:
    return "SELECTED";
  case MOP_EVENT_DESELECTED:
    return "DESELECTED";
  case MOP_EVENT_TRANSFORM_CHANGED:
    return "TRANSFORM_CHANGED";
  case MOP_EVENT_RENDER_MODE_CHANGED:
    return "RENDER_MODE_CHANGED";
  case MOP_EVENT_SHADING_CHANGED:
    return "SHADING_CHANGED";
  case MOP_EVENT_POST_EFFECTS_CHANGED:
    return "POST_EFFECTS_CHANGED";
  case MOP_EVENT_LIGHT_CHANGED:
    return "LIGHT_CHANGED";
  case MOP_EVENT_EDIT_MODE_CHANGED:
    return "EDIT_MODE_CHANGED";
  case MOP_EVENT_ELEMENT_SELECTED:
    return "ELEMENT_SELECTED";
  case MOP_EVENT_ELEMENT_DESELECTED:
    return "ELEMENT_DESELECTED";
  default:
    return "NONE";
  }
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char **argv) {
  int width = 1280, height = 720;
  /* Interactive defaults to Vulkan (GPU). The CPU backend is kept
   * available via --cpu for headless / no-GPU environments, but is too
   * slow for a live viewport at any reasonable resolution. */
  MopBackendType backend = MOP_BACKEND_VULKAN;

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
    else if (strcmp(argv[i], "--hdri") == 0 && i + 1 < argc)
      hdri_path = argv[++i];
    else if (strcmp(argv[i], "--no-hdri") == 0)
      hdri_path = NULL;
    else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
      width = atoi(argv[++i]);
    else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
      height = atoi(argv[++i]);
    else if (strcmp(argv[i], "--4k") == 0) {
      width = 3840;
      height = 2160;
    } else if (strcmp(argv[i], "--2k") == 0) {
      width = 2560;
      height = 1440;
    } else if (strcmp(argv[i], "--1080p") == 0) {
      width = 1920;
      height = 1080;
    }
  }

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  /* HIGHDPI: on Retina we want MOP to render at native drawable
   * resolution (2× the logical window), so the image maps 1:1 with
   * display pixels instead of being SDL-upscaled (which looks blurry
   * and costs quality for nothing). */
  SDL_Window *window =
      SDL_CreateWindow("Master of Puppets — Interactive Showcase",
                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width,
                       height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Renderer *renderer =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer)
    renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  /* Render at the actual drawable resolution — matches the window's
   * aspect 1:1 so the image never gets squished when the user
   * resizes.  HiDPI multipliers are honoured (Retina drawable is
   * 2× the logical window size).  `--4k` opens the window at 4K
   * logical, so the drawable lands at the device's max resolution
   * for the requested window size; on a smaller display the
   * window is OS-clamped and the drawable matches that.  Either
   * way, `draw_w × draw_h` is exactly what we render and what SDL
   * presents — no scaling, no squishing. */
  int draw_w = width, draw_h = height;
  SDL_GetRendererOutputSize(renderer, &draw_w, &draw_h);
  float dpi_x = (float)draw_w / (float)width;
  float dpi_y = (float)draw_h / (float)height;

  SDL_Texture *texture =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                        SDL_TEXTUREACCESS_STREAMING, draw_w, draw_h);
  if (!texture) {
    fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  /* ssaa_factor = 1: we're already rendering at native drawable
   * resolution, so supersampling would be 4×-over-native work with
   * diminishing returns. */
  MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
      .width = draw_w,
      .height = draw_h,
      .backend = backend,
      .ssaa_factor = 1,
  });
  if (!vp) {
    fprintf(stderr, "mop_viewport_create failed\n");
    return 1;
  }

  /* Theme tweaks: the outline painter reads theme.accent (not
   * selection_outline), so accent is the color to change for the
   * selected-object halo.  Highlight color #FF1493 (hot pink) —
   * vivid against dark navy, distinct from the red X-axis and
   * the orange cube material.  Overlay primitives composite
   * without gamma, so values are sRGB-fraction direct. */
  MopTheme theme = mop_theme_default();
  MopColor outline_color = {255.0f / 255.0f, 20.0f / 255.0f, 147.0f / 255.0f,
                            1.0f}; /* #FF1493 — hot pink */
  theme.accent = outline_color;
  theme.selection_outline = outline_color;
  theme.selection_outline_width = 2.0f;
  theme.gizmo_line_width = 10.0f;
  theme.gizmo_target_opacity =
      1.0f; /* keep selected mesh fully opaque when gizmo is active */
  theme.grid_line_width_axis = 6.0f;
  theme.grid_line_width_major = 2.0f;
  theme.grid_line_width_minor = 1.5f;
  theme.wireframe_line_width = 2.0f;
  theme.depth_bias = 0.01f;
  mop_viewport_set_theme(vp, &theme);

  mop_viewport_set_camera(vp, (MopVec3){4.5f, 3.2f, 5.0f}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 45.0f, 0.1f, 100.0f);
  /* Called AFTER set_theme because set_theme copies theme.bg_bottom into
   * the clear color — we want our darker navy, not the theme default. */
  mop_viewport_set_clear_color(vp, (MopColor){0.05f, 0.06f, 0.09f, 1.0f});
  mop_viewport_set_shading(vp, MOP_SHADING_SMOOTH);
  /* Chrome on: grid + origin axis gizmo + corner view navigator. This
   * is the DCC default — in an embedded host you would typically turn
   * it off (set_chrome(vp, false)) and draw your own UI around the
   * viewport instead. */
  mop_viewport_set_chrome(vp, true);
  mop_viewport_set_ambient(vp, 0.15f);
  mop_viewport_set_post_effects(vp, MOP_POST_GAMMA | MOP_POST_TONEMAP |
                                        MOP_POST_FXAA);

  /* Three-light colored rig — shows off the multi-light + linear
   * color path.  Warm amber sun overhead, magenta key-rim from the
   * right, electric-cyan fill from the left.  Matching MopLight
   * fields are already linear RGB; intensity is the brightness
   * multiplier (point lights use 1/d² attenuation so they need
   * larger numbers than directional). */
  MopLight sun = {.type = MOP_LIGHT_DIRECTIONAL,
                  .direction = {-0.3f, -0.9f, -0.2f},
                  .color = {1.0f, 0.72f, 0.32f, 1.0f}, /* warm amber */
                  .intensity = 2.5f,
                  .active = true};
  MopLight magenta = {.type = MOP_LIGHT_POINT,
                      .position = {3.5f, 1.6f, 1.5f},
                      .color = {1.0f, 0.10f, 0.65f, 1.0f}, /* hot magenta */
                      .intensity = 35.0f,
                      .range = 14.0f,
                      .active = true};
  MopLight cyan = {.type = MOP_LIGHT_POINT,
                   .position = {-3.5f, 1.4f, -1.5f},
                   .color = {0.10f, 0.78f, 1.0f, 1.0f}, /* electric cyan */
                   .intensity = 35.0f,
                   .range = 14.0f,
                   .active = true};
  mop_viewport_add_light(vp, &sun);
  mop_viewport_add_light(vp, &magenta);
  mop_viewport_add_light(vp, &cyan);

  if (hdri_path) {
    bool env_ok = mop_viewport_set_environment(vp, &(MopEnvironmentDesc){
                                                       .type = MOP_ENV_HDRI,
                                                       .hdr_path = hdri_path,
                                                       .rotation = 0.0f,
                                                       .intensity = 1.0f,
                                                   });
    if (env_ok) {
      mop_viewport_set_environment_background(vp, true);
      fprintf(stderr, "interactive: loaded HDRI %s\n", hdri_path);
    } else {
      fprintf(stderr,
              "interactive: HDRI load failed (%s), falling back to "
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
  mop_mesh_set_position(sphere, (MopVec3){2.2f, 0.5f, 0.0f});

  MopMaterial metal = mop_material_default();
  metal.metallic = 0.9f;
  metal.roughness = 0.25f;
  mop_mesh_set_material(cube, &metal);

  MopMaterial plastic = mop_material_default();
  plastic.metallic = 0.0f;
  plastic.roughness = 0.35f;
  mop_mesh_set_material(sphere, &plastic);

  fprintf(stderr, "interactive: ready. Left=orbit/select, Ctrl+Left=zoom, "
                  "Right=pan, Two-finger=orbit, W/E/R=gizmo, 1/2/3=shading, "
                  "F=reset, Esc=quit\n");

  /* MOP design-language text — embedded HUD font when libmop was
   * built with `make fonts && make`, otherwise we fall back to
   * loading the .mfa from disk.  Both code paths produce the same
   * MopFont*; hud_font is NULL only when neither is available, in
   * which case we silently skip every text submission. */
  const MopFont *hud_font = mop_font_hud();
  MopFont *loaded_font = NULL;
  if (!hud_font) {
    loaded_font = mop_font_load("build/fonts/jbm_regular.mfa");
    if (!loaded_font)
      loaded_font = mop_font_load("../build/fonts/jbm_regular.mfa");
    if (loaded_font)
      hud_font = loaded_font;
  }

  /* Resolution-adaptive sizing — 12 px @ 720p reads the same as
   * 36 px @ 4K.  px_size is in framebuffer (drawable) pixels. */
  float ui_scale = (float)draw_h / 720.0f;
  if (ui_scale < 1.0f)
    ui_scale = 1.0f;

  bool running = true;
  uint64_t last_ticks = SDL_GetPerformanceCounter();
  double ticks_per_ms = (double)SDL_GetPerformanceFrequency() / 1000.0;
  int frames = 0;
  double accum_ms = 0;

  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      MopInputEvent me = {0};
      me.modifiers = sdl_modifiers(SDL_GetModState());

      switch (ev.type) {
      case SDL_QUIT:
        running = false;
        break;

      case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
          /* Re-aspect: render at the new drawable resolution so the
           * scene never gets squished.  Update mouse scaling, MOP
           * viewport, and the SDL streaming texture together. */
          int nw = ev.window.data1, nh = ev.window.data2;
          if (nw > 0 && nh > 0 && (nw != width || nh != height)) {
            width = nw;
            height = nh;
            SDL_GetRendererOutputSize(renderer, &draw_w, &draw_h);
            dpi_x = (float)draw_w / (float)width;
            dpi_y = (float)draw_h / (float)height;
            mop_viewport_resize(vp, draw_w, draw_h);
            SDL_DestroyTexture(texture);
            texture =
                SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_STREAMING, draw_w, draw_h);
          }
        }
        break;

      case SDL_MOUSEBUTTONDOWN:
        me.x = (float)ev.button.x * dpi_x;
        me.y = (float)ev.button.y * dpi_y;
        if (ev.button.button == SDL_BUTTON_LEFT)
          me.type = MOP_INPUT_POINTER_DOWN;
        else if (ev.button.button == SDL_BUTTON_RIGHT)
          me.type = MOP_INPUT_SECONDARY_DOWN;
        else
          break;
        mop_viewport_input(vp, &me);
        break;

      case SDL_MOUSEBUTTONUP:
        me.x = (float)ev.button.x * dpi_x;
        me.y = (float)ev.button.y * dpi_y;
        if (ev.button.button == SDL_BUTTON_LEFT)
          me.type = MOP_INPUT_POINTER_UP;
        else if (ev.button.button == SDL_BUTTON_RIGHT)
          me.type = MOP_INPUT_SECONDARY_UP;
        else
          break;
        mop_viewport_input(vp, &me);
        break;

      case SDL_MOUSEMOTION: {
        bool ctrl_held = (me.modifiers & MOP_MOD_CTRL) != 0;
        bool lmb_down = (ev.motion.state & SDL_BUTTON_LMASK) != 0;
        if (lmb_down && ctrl_held) {
          /* Ctrl + left-drag = dolly zoom. Vertical motion drives
           * zoom; positive scroll zooms in, so drag up = zoom in. */
          me.type = MOP_INPUT_SCROLL;
          me.scroll = -(float)ev.motion.yrel * dpi_y * 0.02f;
          mop_viewport_input(vp, &me);
        } else {
          me.type = MOP_INPUT_POINTER_MOVE;
          me.x = (float)ev.motion.x * dpi_x;
          me.y = (float)ev.motion.y * dpi_y;
          me.dx = (float)ev.motion.xrel * dpi_x;
          me.dy = (float)ev.motion.yrel * dpi_y;
          mop_viewport_input(vp, &me);
        }
      } break;

      case SDL_MOUSEWHEEL:
        /* Two-finger trackpad swipe = orbit. Horizontal is inverted on
         * macOS natural scroll; vertical is NOT (swipe down = look
         * down feels correct with the MOP orbit convention). */
        me.type = MOP_INPUT_SCROLL_ORBIT;
        me.dx = -ev.wheel.preciseX * 8.0f;
        me.dy = ev.wheel.preciseY * 8.0f;
        mop_viewport_input(vp, &me);
        break;

      case SDL_KEYDOWN: {
        SDL_Keycode k = ev.key.keysym.sym;
        bool ctrl = (me.modifiers & MOP_MOD_CTRL) != 0;
        if (k == SDLK_ESCAPE || k == SDLK_q) {
          running = false;
        } else if (k == SDLK_w) {
          me.type = MOP_INPUT_MODE_TRANSLATE;
          mop_viewport_input(vp, &me);
        } else if (k == SDLK_e) {
          me.type = MOP_INPUT_MODE_ROTATE;
          mop_viewport_input(vp, &me);
        } else if (k == SDLK_r && !ctrl) {
          me.type = MOP_INPUT_MODE_SCALE;
          mop_viewport_input(vp, &me);
        } else if (k == SDLK_f) {
          me.type = MOP_INPUT_RESET_VIEW;
          mop_viewport_input(vp, &me);
        } else if (k == SDLK_1) {
          me.type = MOP_INPUT_SET_SHADING;
          me.value = MOP_SHADING_FLAT;
          mop_viewport_input(vp, &me);
        } else if (k == SDLK_2) {
          me.type = MOP_INPUT_SET_SHADING;
          me.value = MOP_SHADING_SMOOTH;
          mop_viewport_input(vp, &me);
        } else if (k == SDLK_3) {
          me.type = MOP_INPUT_TOGGLE_WIREFRAME;
          mop_viewport_input(vp, &me);
        } else if (k == SDLK_z && ctrl) {
          me.type = MOP_INPUT_UNDO;
          mop_viewport_input(vp, &me);
        } else if (k == SDLK_y && ctrl) {
          me.type = MOP_INPUT_REDO;
          mop_viewport_input(vp, &me);
        } else if (k == SDLK_ESCAPE) {
          me.type = MOP_INPUT_DESELECT;
          mop_viewport_input(vp, &me);
        }
      } break;
      }
    }

    /* Drain MOP output events — a real host would update its UI panel
     * (transform fields, selection highlight, etc.) here. */
    MopEvent oe;
    while (mop_viewport_poll_event(vp, &oe)) {
      if (oe.type == MOP_EVENT_SELECTED || oe.type == MOP_EVENT_DESELECTED)
        fprintf(stderr, "event: %s object=%u\n", event_name(oe.type),
                oe.object_id);
    }

    /* MOP design-language overlay — submitted before render so the
     * rasterizer picks it up during readback compositing.  Three
     * pieces:
     *   1. Top-left navigator breadcrumb
     *   2. Frame stats (current FPS bucket)
     *   3. Selection callout label tracking whichever mesh the user
     *      most recently clicked — gel-red so the brand accent
     *      doubles as the selection signal. */
    if (hud_font) {
      char hud_line[96];
      snprintf(
          hud_line, sizeof(hud_line),
          "SCENE \xe2\x80\xba INTERACTIVE \xe2\x80\xba %dx%d \xe2\x80\xba %s",
          draw_w, draw_h, backend == MOP_BACKEND_VULKAN ? "VULKAN" : "CPU");
      mop_text_draw_2d(vp, hud_font, hud_line, 16.0f * ui_scale,
                       12.0f * ui_scale,
                       (MopTextStyle){
                           .color = MOP_COLOR_BONE,
                           .px_size = 12.0f * ui_scale,
                           .weight = 0.18f,
                       });

      char stats[96];
      double fps = (frames > 0 && accum_ms > 0)
                       ? 1000.0 * (double)frames / accum_ms
                       : 0.0;
      snprintf(stats, sizeof(stats), "FRAME %.1f MS \xc2\xb7 %.0f FPS",
               (frames > 0) ? accum_ms / (double)frames : 0.0, fps);
      mop_text_draw_2d(vp, hud_font, stats, 16.0f * ui_scale, 30.0f * ui_scale,
                       (MopTextStyle){
                           .color = MOP_COLOR_BONE_DIM,
                           .px_size = 11.0f * ui_scale,
                           .weight = 0.15f,
                       });

      /* Selection callout — black-on-#FF1493 pill, ALL CAPS.  The
       * pill matches the selection outline color so the callout
       * visually ties to the highlight.  Note: the text rasterizer
       * gamma-encodes (sqrt) at composite, so we pass *linear*
       * values that map to sRGB (255,20,147) on the framebuffer:
       * linear = (byte/255)^2 ≈ (1.0, 0.0062, 0.332).              */
      const MopColor pink = {1.0f, 0.0062f, 0.332f, 1.0f};
      const MopColor black = {0.0f, 0.0f, 0.0f, 1.0f};
      uint32_t selected_id = mop_viewport_get_selected(vp);
      if (selected_id == 1)
        mop_text_draw_label(vp, hud_font, cube, "CUBE", MOP_LABEL_TOP_CENTER,
                            MOP_LABEL_ALWAYS_ON_TOP,
                            (MopTextStyle){
                                .color = black,
                                .px_size = 13.0f * ui_scale,
                                .weight = 0.22f,
                                .bg_color = pink,
                                .bg_padding = 6.0f * ui_scale,
                            });
      else if (selected_id == 2)
        mop_text_draw_label(vp, hud_font, sphere, "SPHERE",
                            MOP_LABEL_TOP_CENTER, MOP_LABEL_ALWAYS_ON_TOP,
                            (MopTextStyle){
                                .color = black,
                                .px_size = 13.0f * ui_scale,
                                .weight = 0.22f,
                                .bg_color = pink,
                                .bg_padding = 6.0f * ui_scale,
                            });
    }

    mop_viewport_render(vp);

    int rw = 0, rh = 0;
    const uint8_t *pixels = mop_viewport_read_color(vp, &rw, &rh);
    if (pixels && rw > 0 && rh > 0) {
      SDL_UpdateTexture(texture, NULL, pixels, rw * 4);
      SDL_RenderClear(renderer);
      SDL_RenderCopy(renderer, texture, NULL, NULL);
      SDL_RenderPresent(renderer);
    }

    uint64_t now = SDL_GetPerformanceCounter();
    double dt_ms = (double)(now - last_ticks) / ticks_per_ms;
    last_ticks = now;
    accum_ms += dt_ms;
    if (++frames == 60) {
      fprintf(stderr, "interactive: %5.1f fps (%.2f ms/frame)\n",
              60000.0 / accum_ms, accum_ms / 60.0);
      frames = 0;
      accum_ms = 0;
    }
  }

  mop_viewport_destroy(vp);
  if (loaded_font)
    mop_font_free(loaded_font);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
