/*
 * Master of Puppets — Example: CPU Raytracer (Interactive)
 *
 * Side-by-side comparison: MOP rasterizer vs CPU raytracer.
 * Orbit the camera, press R to raytrace the current view.
 * R=raytrace  M=MOP view  W=wireframe  Q/Esc=quit
 *
 * APIs: snapshot.h, camera_query.h, spatial.h, query.h, light.h
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include <SDL3/SDL.h>
#include "geometry.h"
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define MAX_TRIS 4096

/* =========================================================================
 * Context
 * ========================================================================= */

typedef struct {
    MopTriangle tris[MAX_TRIS];
    uint32_t    tri_count;
    uint8_t    *rt_fb;        /* raytraced framebuffer (RGBA8, same size as viewport) */
    int         rt_w, rt_h;
    bool        show_rt;      /* true=show raytraced, false=show rasterized */
    bool        rt_valid;     /* true if rt_fb has valid data */
    bool        btn_hover;    /* mouse is over the button */
} RaytracerCtx;

/* =========================================================================
 * Button geometry — top-right corner
 * ========================================================================= */

#define BTN_W       120
#define BTN_H       32
#define BTN_MARGIN  12

static SDL_FRect btn_rect(int win_w)
{
    return (SDL_FRect){
        .x = (float)(win_w - BTN_W - BTN_MARGIN),
        .y = (float)BTN_MARGIN,
        .w = (float)BTN_W,
        .h = (float)BTN_H,
    };
}

static bool btn_contains(SDL_FRect r, float x, float y)
{
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

static void btn_draw(SDL_Renderer *renderer, int win_w, bool show_rt, bool hover)
{
    SDL_FRect r = btn_rect(win_w);

    /* Background */
    if (hover) {
        SDL_SetRenderDrawColor(renderer, 80, 140, 220, 230);
    } else {
        SDL_SetRenderDrawColor(renderer, 50, 100, 180, 200);
    }
    SDL_RenderFillRect(renderer, &r);

    /* Border */
    SDL_SetRenderDrawColor(renderer, 180, 200, 240, 255);
    SDL_RenderRect(renderer, &r);

    /* Label — centered using 8x8 debug font */
    const char *label = show_rt ? "Rasterize" : "Raytrace";
    int label_len = (int)strlen(label);
    float text_w = (float)(label_len * 8);
    float tx = r.x + (r.w - text_w) / 2.0f;
    float ty = r.y + (r.h - 8.0f) / 2.0f;
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, tx, ty, label);
}

/* =========================================================================
 * Utility
 * ========================================================================= */

static inline float clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* =========================================================================
 * Shading — diffuse Lambert with multi-light support
 * ========================================================================= */

static MopColor shade_hit(MopTriangle *tri, float u, float v, MopVec3 hit_pos,
                           MopRay ray, const MopLight *lights, uint32_t light_count,
                           float ambient)
{
    (void)ray;

    /* Interpolate normal: n = (1-u-v)*n0 + u*n1 + v*n2 */
    float w0 = 1.0f - u - v;
    float w1 = u;
    float w2 = v;

    MopVec3 normal = mop_vec3_normalize(
        mop_vec3_add(
            mop_vec3_add(
                mop_vec3_scale(tri->n[0], w0),
                mop_vec3_scale(tri->n[1], w1)),
            mop_vec3_scale(tri->n[2], w2)));

    /* Base color from material */
    float base_r = tri->material.base_color.r;
    float base_g = tri->material.base_color.g;
    float base_b = tri->material.base_color.b;

    /* Start with ambient contribution */
    float diff_r = 0.0f;
    float diff_g = 0.0f;
    float diff_b = 0.0f;

    /* Evaluate each light */
    for (uint32_t i = 0; i < light_count; i++) {
        const MopLight *light = &lights[i];
        if (!light->active) continue;

        MopVec3 L;
        float attenuation = 1.0f;

        if (light->type == MOP_LIGHT_DIRECTIONAL) {
            /* L points toward the light source */
            L = mop_vec3_normalize(light->direction);
        } else if (light->type == MOP_LIGHT_POINT) {
            MopVec3 to_light = mop_vec3_sub(light->position, hit_pos);
            float dist_sq = mop_vec3_dot(to_light, to_light);
            float dist = sqrtf(dist_sq);
            L = mop_vec3_scale(to_light, 1.0f / (dist + 1e-8f));

            float range = light->range > 0.0f ? light->range : 1.0f;
            attenuation = 1.0f / (1.0f + dist_sq / (range * range));
        } else {
            continue;
        }

        float NdotL = mop_vec3_dot(normal, L);
        if (NdotL <= 0.0f) continue;

        float intensity = light->intensity * attenuation * NdotL;
        diff_r += intensity * light->color.r;
        diff_g += intensity * light->color.g;
        diff_b += intensity * light->color.b;
    }

    /* Final color: (ambient + diffuse) * base_color */
    float r = (ambient + diff_r) * base_r;
    float g = (ambient + diff_g) * base_g;
    float b = (ambient + diff_b) * base_b;

    /* Clamp to [0,1] */
    r = clampf(r, 0.0f, 1.0f);
    g = clampf(g, 0.0f, 1.0f);
    b = clampf(b, 0.0f, 1.0f);

    /* Gamma correction: linear -> sRGB */
    float inv_gamma = 1.0f / 2.2f;
    r = powf(r, inv_gamma);
    g = powf(g, inv_gamma);
    b = powf(b, inv_gamma);

    return (MopColor){ r, g, b, 1.0f };
}

/* =========================================================================
 * Raytrace the current scene
 * ========================================================================= */

static void raytrace_scene(MopViewport *vp, RaytracerCtx *ctx)
{
    printf("[raytracer] Extracting scene...\n");

    /* 1. Extract scene: snapshot + triangle iteration */
    MopSceneSnapshot snap = mop_viewport_snapshot(vp);
    MopTriangleIter iter = mop_triangle_iter_begin(vp);
    MopTriangle tri;

    ctx->tri_count = 0;
    while (mop_triangle_iter_next(&iter, &tri) && ctx->tri_count < MAX_TRIS) {
        ctx->tris[ctx->tri_count++] = tri;
    }

    /* 2. Get camera state for stats */
    MopCameraState cam = mop_viewport_get_camera_state(vp);

    /* 3. Determine framebuffer dimensions from the rasterizer */
    int fb_w, fb_h;
    const uint8_t *raster_px = mop_viewport_read_color(vp, &fb_w, &fb_h);
    (void)raster_px;

    if (fb_w <= 0 || fb_h <= 0) {
        printf("[raytracer] ERROR: invalid framebuffer size %dx%d\n", fb_w, fb_h);
        return;
    }

    /* Reallocate if dimensions changed */
    if (ctx->rt_w != fb_w || ctx->rt_h != fb_h) {
        free(ctx->rt_fb);
        ctx->rt_fb = (uint8_t *)malloc((size_t)(fb_w * fb_h) * 4);
        ctx->rt_w  = fb_w;
        ctx->rt_h  = fb_h;
    }

    if (!ctx->rt_fb) {
        printf("[raytracer] ERROR: allocation failed\n");
        return;
    }

    printf("[raytracer] Tracing %dx%d  (%u triangles, %u lights)\n",
           fb_w, fb_h, ctx->tri_count, snap.light_count);
    printf("[raytracer] Camera: eye=(%.2f,%.2f,%.2f) fov=%.1f deg\n",
           cam.eye.x, cam.eye.y, cam.eye.z,
           cam.fov_radians * 180.0f / 3.14159265f);

    float ambient = 0.15f;
    uint32_t hits = 0;

    /* 4. For each pixel, cast a ray and find the closest hit */
    for (int y = 0; y < fb_h; y++) {
        for (int x = 0; x < fb_w; x++) {
            MopRay ray = mop_viewport_pixel_to_ray(vp,
                (float)x + 0.5f, (float)y + 0.5f);

            float  best_t  = FLT_MAX;
            int    best_id = -1;
            float  best_u  = 0.0f;
            float  best_v  = 0.0f;

            for (uint32_t i = 0; i < ctx->tri_count; i++) {
                float t, u, v;
                bool hit = mop_ray_intersect_triangle(
                    ray,
                    ctx->tris[i].p[0],
                    ctx->tris[i].p[1],
                    ctx->tris[i].p[2],
                    &t, &u, &v);

                if (hit && t > 0.0f && t < best_t) {
                    best_t  = t;
                    best_id = (int)i;
                    best_u  = u;
                    best_v  = v;
                }
            }

            int idx = (y * fb_w + x) * 4;

            if (best_id >= 0) {
                hits++;
                MopVec3 hit_pos = mop_vec3_add(
                    ray.origin, mop_vec3_scale(ray.direction, best_t));

                MopColor c = shade_hit(
                    &ctx->tris[best_id], best_u, best_v, hit_pos,
                    ray, snap.lights, snap.light_count, ambient);

                /* Store as RGBA8 bytes (MOP's ABGR8888 pixel format
                 * is byte-order R, G, B, A on little-endian) */
                ctx->rt_fb[idx + 0] = (uint8_t)(c.r * 255.0f + 0.5f);
                ctx->rt_fb[idx + 1] = (uint8_t)(c.g * 255.0f + 0.5f);
                ctx->rt_fb[idx + 2] = (uint8_t)(c.b * 255.0f + 0.5f);
                ctx->rt_fb[idx + 3] = 255;
            } else {
                /* Background — dark blue clear color, gamma corrected */
                float inv_gamma = 1.0f / 2.2f;
                ctx->rt_fb[idx + 0] = (uint8_t)(powf(0.05f, inv_gamma) * 255.0f + 0.5f);
                ctx->rt_fb[idx + 1] = (uint8_t)(powf(0.05f, inv_gamma) * 255.0f + 0.5f);
                ctx->rt_fb[idx + 2] = (uint8_t)(powf(0.15f, inv_gamma) * 255.0f + 0.5f);
                ctx->rt_fb[idx + 3] = 255;
            }
        }

        /* Progress every 10% */
        if ((y + 1) % (fb_h / 10 + 1) == 0 || y == fb_h - 1) {
            printf("  row %d / %d (%.0f%%)\n",
                   y + 1, fb_h, 100.0f * (float)(y + 1) / (float)fb_h);
        }
    }

    ctx->show_rt  = true;
    ctx->rt_valid = true;

    /* 5. Print stats */
    uint32_t total_rays  = (uint32_t)(fb_w * fb_h);
    uint32_t total_tests = total_rays * ctx->tri_count;

    printf("\n[raytracer] Done.\n");
    printf("  Rays cast:          %u\n", total_rays);
    printf("  Triangles in scene: %u\n", ctx->tri_count);
    printf("  Ray-tri tests:      %u\n", total_tests);
    printf("  Hits:               %u (%.1f%%)\n",
           hits, 100.0f * (float)hits / (float)total_rays);
    printf("  Press M to return to MOP rasterizer view.\n\n");
}

/* =========================================================================
 * Main — standalone SDL3 event loop (no harness, custom blit logic)
 * ========================================================================= */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* ---- SDL3 init ---- */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    int win_w = 800, win_h = 600;

    SDL_Window *window = SDL_CreateWindow(
        "MOP — CPU Raytracer", win_w, win_h, SDL_WINDOW_RESIZABLE);
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

    printf("[raytracer] %dx%d  backend=%s\n",
           win_w, win_h, mop_backend_name(backend));

    /* ---- Camera ---- */
    mop_viewport_set_camera(vp,
        (MopVec3){ 3.0f, 2.0f, 4.0f },
        (MopVec3){ 0.0f, 0.4f, 0.0f },
        (MopVec3){ 0.0f, 1.0f, 0.0f },
        60.0f, 0.1f, 100.0f);

    mop_viewport_set_clear_color(vp,
        (MopColor){ 0.05f, 0.05f, 0.15f, 1.0f });

    mop_viewport_set_ambient(vp, 0.15f);

    /* ---- Scene: 3 cubes + floor plane ---- */

    /* Red cube at (-1.5, 0, 0) */
    MopMesh *cube_r = mop_viewport_add_mesh(vp, &(MopMeshDesc){
        .vertices     = CUBE_VERTICES,
        .vertex_count = CUBE_VERTEX_COUNT,
        .indices      = CUBE_INDICES,
        .index_count  = CUBE_INDEX_COUNT,
        .object_id    = 1,
    });
    mop_mesh_set_position(cube_r, (MopVec3){ -1.5f, 0.0f, 0.0f });
    mop_mesh_set_material(cube_r, &(MopMaterial){
        .base_color = { 0.9f, 0.15f, 0.15f, 1.0f },
        .metallic   = 0.0f,
        .roughness  = 0.6f,
    });

    /* Green cube at (1.5, 0, 0) */
    MopMesh *cube_g = mop_viewport_add_mesh(vp, &(MopMeshDesc){
        .vertices     = CUBE_VERTICES,
        .vertex_count = CUBE_VERTEX_COUNT,
        .indices      = CUBE_INDICES,
        .index_count  = CUBE_INDEX_COUNT,
        .object_id    = 2,
    });
    mop_mesh_set_position(cube_g, (MopVec3){ 1.5f, 0.0f, 0.0f });
    mop_mesh_set_material(cube_g, &(MopMaterial){
        .base_color = { 0.15f, 0.85f, 0.2f, 1.0f },
        .metallic   = 0.1f,
        .roughness  = 0.5f,
    });

    /* Blue cube at (0, 1.2, 0) */
    MopMesh *cube_b = mop_viewport_add_mesh(vp, &(MopMeshDesc){
        .vertices     = CUBE_VERTICES,
        .vertex_count = CUBE_VERTEX_COUNT,
        .indices      = CUBE_INDICES,
        .index_count  = CUBE_INDEX_COUNT,
        .object_id    = 3,
    });
    mop_mesh_set_position(cube_b, (MopVec3){ 0.0f, 1.2f, 0.0f });
    mop_mesh_set_material(cube_b, &(MopMaterial){
        .base_color = { 0.15f, 0.2f, 0.9f, 1.0f },
        .metallic   = 0.2f,
        .roughness  = 0.4f,
    });

    /* Gray floor plane at y = -0.5 */
    MopMesh *floor_m = mop_viewport_add_mesh(vp, &(MopMeshDesc){
        .vertices     = PLANE_VERTICES,
        .vertex_count = PLANE_VERTEX_COUNT,
        .indices      = PLANE_INDICES,
        .index_count  = PLANE_INDEX_COUNT,
        .object_id    = 4,
    });
    mop_mesh_set_position(floor_m, (MopVec3){ 0.0f, -0.5f, 0.0f });
    mop_mesh_set_material(floor_m, &(MopMaterial){
        .base_color = { 0.6f, 0.6f, 0.6f, 1.0f },
        .metallic   = 0.0f,
        .roughness  = 0.9f,
    });

    /* ---- 2 lights ---- */

    /* Directional: warm key light */
    mop_viewport_add_light(vp, &(MopLight){
        .type      = MOP_LIGHT_DIRECTIONAL,
        .direction = { 0.5f, 1.0f, 0.3f },
        .color     = { 1.0f, 0.95f, 0.85f, 1.0f },
        .intensity = 1.0f,
        .active    = true,
    });

    /* Point: white fill light */
    mop_viewport_add_light(vp, &(MopLight){
        .type      = MOP_LIGHT_POINT,
        .position  = { 2.0f, 3.0f, 2.0f },
        .color     = { 1.0f, 1.0f, 1.0f, 1.0f },
        .intensity = 1.2f,
        .range     = 20.0f,
        .active    = true,
    });

    printf("[raytracer] Scene: 3 cubes + floor, 2 lights (directional + point)\n");
    printf("[raytracer] Controls: R=raytrace  M=MOP view  W=wireframe  Q/Esc=quit\n\n");

    /* ---- SDL texture for framebuffer blit ---- */
    SDL_Texture *tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING, win_w, win_h);

    /* ---- Raytracer context ---- */
    RaytracerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* ---- Main loop ---- */
    Uint64 last = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    bool running = true;

    while (running) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / (float)freq;
        (void)dt;
        last = now;

        /* ---- Events ---- */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_EVENT_QUIT:
                running = false;
                break;

            case SDL_EVENT_KEY_DOWN:
                switch (ev.key.key) {
                case SDLK_Q:
                case SDLK_ESCAPE:
                    running = false;
                    break;

                case SDLK_R:
                    /* Render one frame first to ensure transforms are up to date */
                    mop_viewport_render(vp);
                    raytrace_scene(vp, &ctx);
                    break;

                case SDLK_M:
                    ctx.show_rt = false;
                    printf("[raytracer] Switched to MOP rasterizer view.\n");
                    break;

                case SDLK_W:
                    mop_viewport_input(vp, &(MopInputEvent){
                        .type = MOP_INPUT_TOGGLE_WIREFRAME });
                    /* Invalidate RT view since wireframe changes the rasterized output */
                    if (ctx.show_rt) {
                        printf("[raytracer] Wireframe toggled. "
                               "Press R to re-raytrace.\n");
                    }
                    break;

                default:
                    break;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    /* Check button click first */
                    SDL_FRect br = btn_rect(win_w);
                    if (btn_contains(br, ev.button.x, ev.button.y)) {
                        if (ctx.show_rt) {
                            ctx.show_rt = false;
                            printf("[raytracer] Switched to MOP rasterizer view.\n");
                        } else {
                            mop_viewport_render(vp);
                            raytrace_scene(vp, &ctx);
                        }
                    } else {
                        mop_viewport_input(vp, &(MopInputEvent){
                            MOP_INPUT_POINTER_DOWN,
                            ev.button.x, ev.button.y, 0, 0, 0 });
                    }
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    mop_viewport_input(vp, &(MopInputEvent){
                        MOP_INPUT_SECONDARY_DOWN,
                        ev.button.x, ev.button.y, 0, 0, 0 });
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    mop_viewport_input(vp, &(MopInputEvent){
                        MOP_INPUT_POINTER_UP,
                        ev.button.x, ev.button.y, 0, 0, 0 });
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    mop_viewport_input(vp, &(MopInputEvent){
                        .type = MOP_INPUT_SECONDARY_UP });
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                /* Track hover state for button */
                ctx.btn_hover = btn_contains(
                    btn_rect(win_w), ev.motion.x, ev.motion.y);
                mop_viewport_input(vp, &(MopInputEvent){
                    MOP_INPUT_POINTER_MOVE,
                    ev.motion.x, ev.motion.y,
                    ev.motion.xrel, ev.motion.yrel, 0 });
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                mop_viewport_input(vp, &(MopInputEvent){
                    .type = MOP_INPUT_SCROLL,
                    .scroll = ev.wheel.y });
                break;

            case SDL_EVENT_WINDOW_RESIZED: {
                int w = ev.window.data1, h = ev.window.data2;
                if (w > 0 && h > 0) {
                    win_w = w;
                    win_h = h;
                    mop_viewport_resize(vp, w, h);
                    SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(renderer,
                        SDL_PIXELFORMAT_ABGR8888,
                        SDL_TEXTUREACCESS_STREAMING, w, h);

                    /* Invalidate RT framebuffer on resize */
                    ctx.rt_valid = false;
                    ctx.show_rt  = false;
                    free(ctx.rt_fb);
                    ctx.rt_fb = NULL;
                    ctx.rt_w  = 0;
                    ctx.rt_h  = 0;
                }
                break;
            }

            default:
                break;
            }
        }

        /* ---- Render MOP rasterizer every frame ---- */
        mop_viewport_render(vp);

        /* ---- Blit: either raytraced or rasterized ---- */
        if (ctx.show_rt && ctx.rt_valid && ctx.rt_fb
            && ctx.rt_w == win_w && ctx.rt_h == win_h) {
            SDL_UpdateTexture(tex, NULL, ctx.rt_fb, ctx.rt_w * 4);
        } else {
            int fb_w, fb_h;
            const uint8_t *px = mop_viewport_read_color(vp, &fb_w, &fb_h);
            if (px) {
                SDL_UpdateTexture(tex, NULL, px, fb_w * 4);
            }
        }

        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, tex, NULL, NULL);

        /* Draw button overlay */
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        btn_draw(renderer, win_w, ctx.show_rt, ctx.btn_hover);

        SDL_RenderPresent(renderer);
    }

    /* ---- Cleanup ---- */
    printf("[raytracer] Shutting down...\n");
    free(ctx.rt_fb);
    SDL_DestroyTexture(tex);
    mop_viewport_destroy(vp);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("[raytracer] Clean shutdown.\n");
    return 0;
}
