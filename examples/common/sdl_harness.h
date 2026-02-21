/*
 * Master of Puppets — Examples: SDL3 Harness
 *
 * Header-only SDL3 application harness for data pipeline examples.
 * Provides: window, renderer, texture, event loop with built-in camera
 * controls (orbit/pan/zoom), backend selection via MOP_BACKEND env var,
 * window resize, and framebuffer blitting.
 *
 * Each example provides callbacks for setup, per-frame update, key
 * handling, mouse click, and cleanup.
 *
 * Usage:
 *   MopSDLApp app = { .title = "My Demo", .width = 800, .height = 600,
 *                      .setup = my_setup, .update = my_update, ... };
 *   return mop_sdl_run(&app);
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_SDL_HARNESS_H
#define MOP_SDL_HARNESS_H

#include <mop/mop.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Application descriptor
 * ------------------------------------------------------------------------- */

typedef struct MopSDLApp {
    const char *title;
    int width, height;

    /* Called once after viewport is created.  Build your scene here. */
    void (*setup)(MopViewport *vp, void *ctx);

    /* Called every frame before mop_viewport_render.  dt is in seconds. */
    void (*update)(MopViewport *vp, float dt, void *ctx);

    /* Called on key-down.  Return true to suppress default handling. */
    bool (*on_key)(MopViewport *vp, SDL_Keycode key, void *ctx);

    /* Called on left-click (after MOP processes it for selection/gizmo).
     * (x, y) are pixel coordinates (top-left origin). */
    void (*on_click)(MopViewport *vp, float x, float y, void *ctx);

    /* Called once before shutdown. */
    void (*cleanup)(void *ctx);

    /* Opaque user context pointer passed to all callbacks. */
    void *ctx;
} MopSDLApp;

/* -------------------------------------------------------------------------
 * Run the application.  Returns 0 on success, 1 on failure.
 * This function owns the entire lifecycle: init → loop → shutdown.
 * ------------------------------------------------------------------------- */

static inline int mop_sdl_run(MopSDLApp *app)
{
    /* ---- SDL3 init ---- */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    int win_w = app->width  > 0 ? app->width  : 800;
    int win_h = app->height > 0 ? app->height : 600;

    SDL_Window *window = SDL_CreateWindow(
        app->title ? app->title : "MOP Example",
        win_w, win_h, SDL_WINDOW_RESIZABLE);
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

    /* ---- Backend selection via env ---- */
    MopBackendType backend = MOP_BACKEND_CPU;
    const char *env = getenv("MOP_BACKEND");
    if (env) {
        if (strcmp(env, "opengl") == 0)  backend = MOP_BACKEND_OPENGL;
        if (strcmp(env, "vulkan") == 0)  backend = MOP_BACKEND_VULKAN;
    }

    /* ---- MOP viewport ---- */
    MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
        .width = win_w, .height = win_h, .backend = backend
    });
    if (!vp) {
        fprintf(stderr, "Failed to create MOP viewport\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    mop_viewport_set_clear_color(vp, (MopColor){ .12f, .12f, .16f, 1 });

    printf("[%s] %dx%d  backend=%s\n",
           app->title ? app->title : "MOP",
           win_w, win_h, mop_backend_name(backend));

    /* ---- SDL texture for framebuffer blit ---- */
    SDL_Texture *tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING, win_w, win_h);

    /* ---- Setup callback ---- */
    if (app->setup) app->setup(vp, app->ctx);

    /* ---- Main loop ---- */
    Uint64 last = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    bool running = true;

    while (running) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / (float)freq;
        last = now;

        /* ---- Events ---- */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_EVENT_QUIT:
                running = false;
                break;

            case SDL_EVENT_KEY_DOWN: {
                /* Let app handle first */
                bool handled = false;
                if (app->on_key)
                    handled = app->on_key(vp, ev.key.key, app->ctx);

                if (!handled) {
                    switch (ev.key.key) {
                    case SDLK_Q:
                    case SDLK_ESCAPE:
                        running = false;
                        break;
                    case SDLK_W:
                        mop_viewport_input(vp, &(MopInputEvent){
                            .type = MOP_INPUT_TOGGLE_WIREFRAME});
                        break;
                    default: break;
                    }
                }
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    mop_viewport_input(vp, &(MopInputEvent){
                        MOP_INPUT_POINTER_DOWN,
                        ev.button.x, ev.button.y, 0, 0, 0});
                    if (app->on_click)
                        app->on_click(vp, ev.button.x, ev.button.y, app->ctx);
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    mop_viewport_input(vp, &(MopInputEvent){
                        MOP_INPUT_SECONDARY_DOWN,
                        ev.button.x, ev.button.y, 0, 0, 0});
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    mop_viewport_input(vp, &(MopInputEvent){
                        MOP_INPUT_POINTER_UP,
                        ev.button.x, ev.button.y, 0, 0, 0});
                else if (ev.button.button == SDL_BUTTON_RIGHT)
                    mop_viewport_input(vp, &(MopInputEvent){
                        .type = MOP_INPUT_SECONDARY_UP});
                break;

            case SDL_EVENT_MOUSE_MOTION:
                mop_viewport_input(vp, &(MopInputEvent){
                    MOP_INPUT_POINTER_MOVE,
                    ev.motion.x, ev.motion.y,
                    ev.motion.xrel, ev.motion.yrel, 0});
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                mop_viewport_input(vp, &(MopInputEvent){
                    .type = MOP_INPUT_SCROLL,
                    .scroll = ev.wheel.y});
                break;

            case SDL_EVENT_WINDOW_RESIZED: {
                int w = ev.window.data1, h = ev.window.data2;
                if (w > 0 && h > 0) {
                    win_w = w; win_h = h;
                    mop_viewport_resize(vp, w, h);
                    SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(renderer,
                        SDL_PIXELFORMAT_ABGR8888,
                        SDL_TEXTUREACCESS_STREAMING, w, h);
                }
                break;
            }

            default: break;
            }
        }

        /* ---- Per-frame update ---- */
        if (app->update) app->update(vp, dt, app->ctx);

        /* ---- Render ---- */
        mop_viewport_render(vp);

        /* ---- Blit framebuffer ---- */
        int fb_w, fb_h;
        const uint8_t *px = mop_viewport_read_color(vp, &fb_w, &fb_h);
        if (px && tex) {
            SDL_UpdateTexture(tex, NULL, px, fb_w * 4);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, tex, NULL, NULL);
            SDL_RenderPresent(renderer);
        }
    }

    /* ---- Cleanup ---- */
    if (app->cleanup) app->cleanup(app->ctx);
    SDL_DestroyTexture(tex);
    mop_viewport_destroy(vp);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#endif /* MOP_SDL_HARNESS_H */
