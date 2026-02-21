/*
 * Master of Puppets — Target Blaster
 *
 * A real 3D FPS built on MOP.  Two modes:
 *
 *   EDITOR  — orbit / pan / zoom camera to design the arena.
 *   PLAY    — first-person WASD + mouse look, click to shoot.
 *
 * Tab toggles between modes.  In Play mode the mouse is captured
 * and you move like a real FPS.  Shooting raycasts from the center
 * crosshair via MOP's spatial query engine.
 *
 * Controls (Play):
 *   WASD / arrows = move          Mouse = look
 *   Left-click    = shoot         Space = jump
 *   R             = restart       Tab   = editor mode
 *   Esc           = quit
 *
 * Controls (Editor):
 *   Left-drag  = orbit            Right-drag = pan
 *   Scroll     = zoom             Tab        = play mode
 *   W          = wireframe        Esc        = quit
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include <SDL3/SDL.h>
#include "geometry.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* =========================================================================
 * Tuning
 * ========================================================================= */

#define MAX_TARGETS      24
#define ARENA_HALF       5.0f
#define FLOOR_Y          (-1.0f)
#define TARGET_HALF      0.4f
#define BOB_AMP          0.35f
#define DRIFT_SPEED      0.7f
#define INITIAL_LIVES    5
#define TARGETS_PER_WAVE 3
#define WAVE_BONUS       50
#define MOVE_SPEED       5.0f
#define MOUSE_SENS       0.002f
#define GRAVITY          12.0f
#define JUMP_VEL         5.5f
#define EYE_HEIGHT       1.6f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 * Mode
 * ========================================================================= */

typedef enum { MODE_EDITOR, MODE_PLAY } Mode;

/* =========================================================================
 * FPS Camera
 * ========================================================================= */

typedef struct {
    MopVec3 pos;         /* feet position */
    float   yaw;         /* radians, 0 = looking along -Z */
    float   pitch;       /* radians, clamped ±89° */
    float   vy;          /* vertical velocity (jump/gravity) */
    bool    on_ground;
} FPSCamera;

static MopVec3 fps_forward(const FPSCamera *c)
{
    return (MopVec3){
        sinf(c->yaw),
        0.0f,
        -cosf(c->yaw),
    };
}

static MopVec3 fps_right(const FPSCamera *c)
{
    return (MopVec3){
        cosf(c->yaw),
        0.0f,
        sinf(c->yaw),
    };
}

static MopVec3 fps_eye(const FPSCamera *c)
{
    return (MopVec3){ c->pos.x, c->pos.y + EYE_HEIGHT, c->pos.z };
}

static MopVec3 fps_target(const FPSCamera *c)
{
    MopVec3 eye = fps_eye(c);
    return (MopVec3){
        eye.x + sinf(c->yaw) * cosf(c->pitch),
        eye.y + sinf(c->pitch),
        eye.z - cosf(c->yaw) * cosf(c->pitch),
    };
}

/* =========================================================================
 * Target
 * ========================================================================= */

typedef struct {
    MopMesh *mesh;
    bool     alive;
    MopVec3  base_pos;
    MopVec3  drift;
    float    bob_phase;
    float    bob_speed;
    uint32_t object_id;
} Target;

/* =========================================================================
 * Game state
 * ========================================================================= */

typedef struct {
    Target    targets[MAX_TARGETS];
    MopMesh  *floor_mesh;
    int       score;
    int       wave;
    int       lives;
    int       hits;
    int       shots;
    int       alive_count;
    bool      game_over;
    float     next_id;
    FPSCamera cam;
    Mode      mode;
} GameState;

/* =========================================================================
 * Colors
 * ========================================================================= */

static const MopColor TARGET_COLORS[] = {
    { 0.95f, 0.20f, 0.20f, 1 },
    { 0.20f, 0.90f, 0.30f, 1 },
    { 0.25f, 0.40f, 0.95f, 1 },
    { 0.95f, 0.85f, 0.10f, 1 },
    { 0.90f, 0.30f, 0.90f, 1 },
    { 0.20f, 0.90f, 0.90f, 1 },
    { 1.00f, 0.55f, 0.10f, 1 },
    { 0.50f, 0.90f, 0.20f, 1 },
};
#define NUM_COLORS (int)(sizeof(TARGET_COLORS) / sizeof(TARGET_COLORS[0]))

/* =========================================================================
 * RNG
 * ========================================================================= */

static uint32_t rng_state = 42;

static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static float randf(void) { return (float)(xorshift32() & 0xFFFF) / 65535.0f; }

static float randf_range(float lo, float hi)
{
    return lo + randf() * (hi - lo);
}

/* =========================================================================
 * Spawn
 * ========================================================================= */

static void spawn_target(MopViewport *vp, GameState *gs)
{
    int slot = -1;
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (!gs->targets[i].alive) { slot = i; break; }
    }
    if (slot < 0) return;

    Target *t = &gs->targets[slot];

    t->base_pos = (MopVec3){
        randf_range(-ARENA_HALF + 1, ARENA_HALF - 1),
        randf_range(0.5f, 3.5f),
        randf_range(-ARENA_HALF + 1, ARENA_HALF - 1),
    };

    float speed = DRIFT_SPEED * (1.0f + (float)gs->wave * 0.15f);
    float angle = randf_range(0, (float)(2.0 * M_PI));
    t->drift = (MopVec3){ cosf(angle) * speed, 0.0f, sinf(angle) * speed };
    t->bob_phase = randf_range(0, (float)(2.0 * M_PI));
    t->bob_speed = randf_range(2.0f, 4.5f);

    gs->next_id += 1.0f;
    t->object_id = (uint32_t)gs->next_id;

    t->mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){
        .vertices     = CUBE_VERTICES,
        .vertex_count = CUBE_VERTEX_COUNT,
        .indices      = CUBE_INDICES,
        .index_count  = CUBE_INDEX_COUNT,
        .object_id    = t->object_id,
    });

    float s = TARGET_HALF * 2.0f;
    mop_mesh_set_scale(t->mesh, (MopVec3){ s, s, s });
    mop_mesh_set_position(t->mesh, t->base_pos);

    mop_mesh_set_material(t->mesh, &(MopMaterial){
        .base_color = TARGET_COLORS[slot % NUM_COLORS],
        .metallic   = 0.2f,
        .roughness  = 0.5f,
    });

    t->alive = true;
    gs->alive_count++;
}

static void spawn_wave(MopViewport *vp, GameState *gs)
{
    gs->wave++;
    int count = TARGETS_PER_WAVE + (gs->wave - 1) * 2;
    if (count > MAX_TARGETS) count = MAX_TARGETS;

    printf("[game] === Wave %d ===  (%d targets)\n", gs->wave, count);

    for (int i = 0; i < count; i++)
        spawn_target(vp, gs);
}

/* =========================================================================
 * Reset
 * ========================================================================= */

static void game_reset(MopViewport *vp, GameState *gs)
{
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (gs->targets[i].alive && gs->targets[i].mesh)
            mop_viewport_remove_mesh(vp, gs->targets[i].mesh);
        gs->targets[i].alive = false;
        gs->targets[i].mesh  = NULL;
    }

    gs->score       = 0;
    gs->wave        = 0;
    gs->lives       = INITIAL_LIVES;
    gs->hits        = 0;
    gs->shots       = 0;
    gs->alive_count = 0;
    gs->game_over   = false;

    /* Reset player position */
    gs->cam.pos       = (MopVec3){ 0.0f, FLOOR_Y + 0.01f, 8.0f };
    gs->cam.yaw       = 0.0f;
    gs->cam.pitch     = 0.0f;
    gs->cam.vy        = 0.0f;
    gs->cam.on_ground = true;

    spawn_wave(vp, gs);
    printf("[game] Game started!  Lives: %d\n", gs->lives);
}

/* =========================================================================
 * Target update
 * ========================================================================= */

static void update_targets(MopViewport *vp, GameState *gs, float dt)
{
    if (gs->game_over) return;

    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &gs->targets[i];
        if (!t->alive) continue;

        t->bob_phase += t->bob_speed * dt;
        float bob_y = sinf(t->bob_phase) * BOB_AMP;

        t->base_pos.x += t->drift.x * dt;
        t->base_pos.z += t->drift.z * dt;

        if (t->base_pos.x < -ARENA_HALF || t->base_pos.x > ARENA_HALF)
            t->drift.x = -t->drift.x;
        if (t->base_pos.z < -ARENA_HALF || t->base_pos.z > ARENA_HALF)
            t->drift.z = -t->drift.z;

        if (t->base_pos.x < -ARENA_HALF) t->base_pos.x = -ARENA_HALF;
        if (t->base_pos.x >  ARENA_HALF) t->base_pos.x =  ARENA_HALF;
        if (t->base_pos.z < -ARENA_HALF) t->base_pos.z = -ARENA_HALF;
        if (t->base_pos.z >  ARENA_HALF) t->base_pos.z =  ARENA_HALF;

        MopVec3 pos = t->base_pos;
        pos.y += bob_y;
        mop_mesh_set_position(t->mesh, pos);
        mop_mesh_set_rotation(t->mesh, (MopVec3){ 0, t->bob_phase * 0.5f, 0 });
    }

    if (gs->alive_count <= 0) {
        gs->score += WAVE_BONUS;
        printf("[game] Wave %d cleared!  +%d bonus  (score: %d)\n",
               gs->wave, WAVE_BONUS, gs->score);
        spawn_wave(vp, gs);
    }
}

/* =========================================================================
 * FPS movement (Play mode)
 * ========================================================================= */

static void update_fps(GameState *gs, float dt)
{
    FPSCamera *c = &gs->cam;

    /* Gravity + jump */
    if (!c->on_ground) {
        c->vy -= GRAVITY * dt;
    }
    c->pos.y += c->vy * dt;

    if (c->pos.y <= FLOOR_Y + 0.01f) {
        c->pos.y = FLOOR_Y + 0.01f;
        c->vy = 0.0f;
        c->on_ground = true;
    }

    /* WASD movement via SDL scancode state (reliable held-key polling) */
    const bool *ks = SDL_GetKeyboardState(NULL);

    MopVec3 fwd   = fps_forward(c);
    MopVec3 right = fps_right(c);
    float speed = MOVE_SPEED * dt;

    if (ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP]) {
        c->pos.x += fwd.x * speed;
        c->pos.z += fwd.z * speed;
    }
    if (ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN]) {
        c->pos.x -= fwd.x * speed;
        c->pos.z -= fwd.z * speed;
    }
    if (ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_LEFT]) {
        c->pos.x -= right.x * speed;
        c->pos.z -= right.z * speed;
    }
    if (ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_RIGHT]) {
        c->pos.x += right.x * speed;
        c->pos.z += right.z * speed;
    }

    /* Clamp to arena */
    float limit = ARENA_HALF + 2.0f;
    if (c->pos.x < -limit) c->pos.x = -limit;
    if (c->pos.x >  limit) c->pos.x =  limit;
    if (c->pos.z < -limit) c->pos.z = -limit;
    if (c->pos.z >  limit) c->pos.z =  limit;
}

/* =========================================================================
 * Shoot — always from screen center in Play mode
 * ========================================================================= */

static void game_shoot(MopViewport *vp, GameState *gs, float px, float py)
{
    if (gs->game_over) return;

    gs->shots++;
    mop_viewport_render(vp);

    MopRayHit hit = mop_viewport_raycast(vp, px, py);

    if (hit.hit && hit.object_id != 999) {
        for (int i = 0; i < MAX_TARGETS; i++) {
            Target *t = &gs->targets[i];
            if (t->alive && t->object_id == hit.object_id) {
                gs->hits++;
                int points = 10 + gs->wave * 5;
                gs->score += points;

                printf("[game] HIT target %u  dist=%.1f  +%d  (score: %d)\n",
                       hit.object_id, hit.distance, points, gs->score);

                mop_viewport_remove_mesh(vp, t->mesh);
                t->mesh  = NULL;
                t->alive = false;
                gs->alive_count--;
                return;
            }
        }
    }

    gs->lives--;
    printf("[game] MISS  lives: %d\n", gs->lives);

    if (gs->lives <= 0) {
        gs->game_over = true;
        printf("\n[game] ====== GAME OVER ======\n");
        printf("[game] Score: %d   Waves: %d   Accuracy: %.0f%%\n",
               gs->score, gs->wave,
               gs->shots > 0 ? 100.0f * (float)gs->hits / (float)gs->shots : 0.0f);
        printf("[game] Press R to restart\n\n");
    }
}

/* =========================================================================
 * HUD
 * ========================================================================= */

static void hud_draw(SDL_Renderer *renderer, int win_w, int win_h,
                     GameState *gs)
{
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    char buf[128];

    float cx = (float)win_w / 2.0f;
    float cy = (float)win_h / 2.0f;

    /* ---- Top bar ---- */
    SDL_FRect bar = { 0, 0, (float)win_w, 30 };
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    SDL_RenderFillRect(renderer, &bar);

    /* Mode indicator */
    const char *mode_str = gs->mode == MODE_PLAY ? "[PLAY]" : "[EDITOR]";
    SDL_SetRenderDrawColor(renderer,
        gs->mode == MODE_PLAY ? 100 : 200,
        gs->mode == MODE_PLAY ? 220 : 200,
        gs->mode == MODE_PLAY ? 100 : 60,
        255);
    SDL_RenderDebugText(renderer, 10, 9, mode_str);

    snprintf(buf, sizeof(buf), "Score: %d", gs->score);
    SDL_SetRenderDrawColor(renderer, 255, 255, 100, 255);
    SDL_RenderDebugText(renderer, 90, 9, buf);

    snprintf(buf, sizeof(buf), "Wave: %d", gs->wave);
    SDL_SetRenderDrawColor(renderer, 200, 220, 255, 255);
    SDL_RenderDebugText(renderer, 230, 9, buf);

    /* Lives */
    SDL_SetRenderDrawColor(renderer, 255, 200, 200, 255);
    SDL_RenderDebugText(renderer, 340, 9, "Lives:");
    for (int i = 0; i < gs->lives; i++) {
        SDL_FRect dot = { 396.0f + (float)i * 14.0f, 11.0f, 8.0f, 8.0f };
        SDL_SetRenderDrawColor(renderer, 255, 60, 60, 255);
        SDL_RenderFillRect(renderer, &dot);
    }

    float acc = gs->shots > 0
        ? 100.0f * (float)gs->hits / (float)gs->shots : 0.0f;
    snprintf(buf, sizeof(buf), "Accuracy: %.0f%%", acc);
    SDL_SetRenderDrawColor(renderer, 180, 220, 180, 255);
    SDL_RenderDebugText(renderer, (float)win_w - 140.0f, 9, buf);

    /* ---- Crosshair (Play mode only) ---- */
    if (gs->mode == MODE_PLAY && !gs->game_over) {
        float cs = 12.0f;
        float gap = 4.0f;

        /* Shadow */
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 120);
        SDL_RenderLine(renderer, cx - cs, cy + 1, cx - gap, cy + 1);
        SDL_RenderLine(renderer, cx + gap, cy + 1, cx + cs, cy + 1);
        SDL_RenderLine(renderer, cx + 1, cy - cs, cx + 1, cy - gap);
        SDL_RenderLine(renderer, cx + 1, cy + gap, cx + 1, cy + cs);

        /* Crosshair lines (gap in center) */
        SDL_SetRenderDrawColor(renderer, 0, 255, 80, 220);
        SDL_RenderLine(renderer, cx - cs, cy, cx - gap, cy);
        SDL_RenderLine(renderer, cx + gap, cy, cx + cs, cy);
        SDL_RenderLine(renderer, cx, cy - cs, cx, cy - gap);
        SDL_RenderLine(renderer, cx, cy + gap, cx, cy + cs);

        /* Center dot */
        SDL_FRect cdot = { cx - 1, cy - 1, 2, 2 };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderFillRect(renderer, &cdot);
    }

    /* ---- Bottom hint ---- */
    if (gs->mode == MODE_PLAY && !gs->game_over) {
        const char *hint = "WASD=move  Mouse=look  Click=shoot  Space=jump  Tab=editor  R=restart";
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 120);
        SDL_FRect hbar = { 0, (float)win_h - 20, (float)win_w, 20 };
        SDL_RenderFillRect(renderer, &hbar);
        SDL_SetRenderDrawColor(renderer, 180, 180, 180, 200);
        SDL_RenderDebugText(renderer, 10, (float)win_h - 14, hint);
    } else if (gs->mode == MODE_EDITOR) {
        const char *hint = "Orbit=LMB  Pan=RMB  Zoom=scroll  Tab=play  W=wire";
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 120);
        SDL_FRect hbar = { 0, (float)win_h - 20, (float)win_w, 20 };
        SDL_RenderFillRect(renderer, &hbar);
        SDL_SetRenderDrawColor(renderer, 180, 180, 180, 200);
        SDL_RenderDebugText(renderer, 10, (float)win_h - 14, hint);
    }

    /* ---- Game over ---- */
    if (gs->game_over) {
        SDL_FRect overlay = { 0, 0, (float)win_w, (float)win_h };
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
        SDL_RenderFillRect(renderer, &overlay);

        SDL_SetRenderDrawColor(renderer, 255, 80, 80, 255);
        const char *go = "GAME OVER";
        float gx = cx - (float)(strlen(go) * 8) / 2.0f;
        SDL_RenderDebugText(renderer, gx, cy - 40, go);

        snprintf(buf, sizeof(buf), "Score: %d    Waves: %d    Accuracy: %.0f%%",
                 gs->score, gs->wave, acc);
        SDL_SetRenderDrawColor(renderer, 255, 255, 200, 255);
        float sx = cx - (float)(strlen(buf) * 8) / 2.0f;
        SDL_RenderDebugText(renderer, sx, cy - 10, buf);

        const char *r1 = "Press R to restart";
        SDL_SetRenderDrawColor(renderer, 200, 200, 255, 255);
        float rx = cx - (float)(strlen(r1) * 8) / 2.0f;
        SDL_RenderDebugText(renderer, rx, cy + 20, r1);
    }
}

/* =========================================================================
 * Mode switching
 * ========================================================================= */

static void enter_play_mode(SDL_Window *window, MopViewport *vp, GameState *gs)
{
    gs->mode = MODE_PLAY;
    SDL_SetWindowRelativeMouseMode(window, true);
    mop_viewport_set_chrome(vp, false);
    printf("[game] Entering PLAY mode (WASD + mouse look)\n");
}

static void enter_editor_mode(SDL_Window *window, MopViewport *vp, GameState *gs)
{
    gs->mode = MODE_EDITOR;
    SDL_SetWindowRelativeMouseMode(window, false);
    mop_viewport_set_chrome(vp, true);
    printf("[game] Entering EDITOR mode (orbit / pan / zoom)\n");
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    int win_w = 960, win_h = 720;

    SDL_Window *window = SDL_CreateWindow(
        "MOP — Target Blaster", win_w, win_h, SDL_WINDOW_RESIZABLE);
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

    /* Backend */
    MopBackendType backend = MOP_BACKEND_CPU;
    const char *env = getenv("MOP_BACKEND");
    if (env) {
        if (strcmp(env, "opengl") == 0)  backend = MOP_BACKEND_OPENGL;
        if (strcmp(env, "vulkan") == 0)  backend = MOP_BACKEND_VULKAN;
    }

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

    mop_viewport_set_clear_color(vp, (MopColor){ .06f, .06f, .12f, 1 });

    printf("[game] Target Blaster  %dx%d  backend=%s\n",
           win_w, win_h, mop_backend_name(backend));
    printf("[game] Tab = toggle Editor/Play mode\n\n");

    /* Lighting */
    mop_viewport_set_ambient(vp, 0.25f);

    mop_viewport_add_light(vp, &(MopLight){
        .type      = MOP_LIGHT_DIRECTIONAL,
        .direction = { 0.4f, 1.0f, 0.3f },
        .color     = { 1.0f, 0.95f, 0.85f, 1.0f },
        .intensity = 1.0f,
        .active    = true,
    });

    mop_viewport_add_light(vp, &(MopLight){
        .type      = MOP_LIGHT_POINT,
        .position  = { 0.0f, 6.0f, 0.0f },
        .color     = { 0.5f, 0.7f, 1.0f, 1.0f },
        .intensity = 2.5f,
        .range     = 25.0f,
        .active    = true,
    });

    /* Arena floor */
    MopMesh *floor_mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){
        .vertices     = PLANE_VERTICES,
        .vertex_count = PLANE_VERTEX_COUNT,
        .indices      = PLANE_INDICES,
        .index_count  = PLANE_INDEX_COUNT,
        .object_id    = 999,
    });
    mop_mesh_set_position(floor_mesh, (MopVec3){ 0, FLOOR_Y, 0 });
    mop_mesh_set_material(floor_mesh, &(MopMaterial){
        .base_color = { 0.25f, 0.25f, 0.30f, 1.0f },
        .metallic   = 0.0f,
        .roughness  = 0.9f,
    });

    /* SDL texture */
    SDL_Texture *tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING, win_w, win_h);

    /* Game state */
    GameState gs;
    memset(&gs, 0, sizeof(gs));

    rng_state = (uint32_t)(SDL_GetPerformanceCounter() & 0xFFFFFFFF);
    if (rng_state == 0) rng_state = 1;

    /* Start in editor mode so user can see the arena */
    gs.mode = MODE_EDITOR;

    /* Set initial editor camera */
    mop_viewport_set_camera(vp,
        (MopVec3){ 0, 5, 12 },
        (MopVec3){ 0, 1,  0 },
        (MopVec3){ 0, 1,  0 },
        55.0f, 0.1f, 100.0f);

    game_reset(vp, &gs);

    /* Main loop */
    Uint64 last = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    bool running = true;

    while (running) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / (float)freq;
        if (dt > 1.0f / 15.0f) dt = 1.0f / 15.0f;
        last = now;

        /* Events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_EVENT_QUIT:
                running = false;
                break;

            case SDL_EVENT_KEY_DOWN:
                switch (ev.key.key) {
                case SDLK_ESCAPE:
                    if (gs.mode == MODE_PLAY) {
                        enter_editor_mode(window, vp, &gs);
                    } else {
                        running = false;
                    }
                    break;

                case SDLK_TAB:
                    if (gs.mode == MODE_EDITOR)
                        enter_play_mode(window, vp, &gs);
                    else
                        enter_editor_mode(window, vp, &gs);
                    break;

                case SDLK_R:
                    game_reset(vp, &gs);
                    break;

                case SDLK_SPACE:
                    if (gs.mode == MODE_PLAY && gs.cam.on_ground) {
                        gs.cam.vy = JUMP_VEL;
                        gs.cam.on_ground = false;
                    }
                    break;

                case SDLK_F:
                    if (gs.mode == MODE_EDITOR) {
                        mop_viewport_input(vp, &(MopInputEvent){
                            .type = MOP_INPUT_TOGGLE_WIREFRAME });
                    }
                    break;

                default:
                    break;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (gs.mode == MODE_PLAY) {
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        /* Shoot from screen center */
                        game_shoot(vp, &gs,
                            (float)win_w / 2.0f, (float)win_h / 2.0f);
                    }
                } else {
                    /* Editor: forward to MOP for orbit/pan */
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        mop_viewport_input(vp, &(MopInputEvent){
                            MOP_INPUT_POINTER_DOWN,
                            ev.button.x, ev.button.y, 0, 0, 0 });
                    } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                        mop_viewport_input(vp, &(MopInputEvent){
                            MOP_INPUT_SECONDARY_DOWN,
                            ev.button.x, ev.button.y, 0, 0, 0 });
                    }
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (gs.mode == MODE_EDITOR) {
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        mop_viewport_input(vp, &(MopInputEvent){
                            MOP_INPUT_POINTER_UP,
                            ev.button.x, ev.button.y, 0, 0, 0 });
                    } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                        mop_viewport_input(vp, &(MopInputEvent){
                            .type = MOP_INPUT_SECONDARY_UP });
                    }
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                if (gs.mode == MODE_PLAY) {
                    /* FPS mouse look */
                    gs.cam.yaw   += ev.motion.xrel * MOUSE_SENS;
                    gs.cam.pitch -= ev.motion.yrel * MOUSE_SENS;

                    /* Clamp pitch to avoid gimbal flip */
                    float max_pitch = 89.0f * (float)M_PI / 180.0f;
                    if (gs.cam.pitch >  max_pitch) gs.cam.pitch =  max_pitch;
                    if (gs.cam.pitch < -max_pitch) gs.cam.pitch = -max_pitch;
                } else {
                    /* Editor: forward motion for orbit/pan */
                    mop_viewport_input(vp, &(MopInputEvent){
                        MOP_INPUT_POINTER_MOVE,
                        ev.motion.x, ev.motion.y,
                        ev.motion.xrel, ev.motion.yrel, 0 });
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                if (gs.mode == MODE_EDITOR) {
                    mop_viewport_input(vp, &(MopInputEvent){
                        .type = MOP_INPUT_SCROLL,
                        .scroll = ev.wheel.y });
                }
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

        /* Update targets */
        update_targets(vp, &gs, dt);

        /* FPS camera update */
        if (gs.mode == MODE_PLAY) {
            update_fps(&gs, dt);

            mop_viewport_set_camera(vp,
                fps_eye(&gs.cam),
                fps_target(&gs.cam),
                (MopVec3){ 0, 1, 0 },
                55.0f, 0.1f, 100.0f);
        }

        /* Render */
        mop_viewport_render(vp);

        /* Blit */
        int fb_w, fb_h;
        const uint8_t *px = mop_viewport_read_color(vp, &fb_w, &fb_h);
        if (px && tex) {
            SDL_UpdateTexture(tex, NULL, px, fb_w * 4);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, tex, NULL, NULL);
            hud_draw(renderer, win_w, win_h, &gs);
            SDL_RenderPresent(renderer);
        }
    }

    /* Cleanup */
    printf("[game] Final score: %d   Waves: %d\n", gs.score, gs.wave);
    SDL_DestroyTexture(tex);
    mop_viewport_destroy(vp);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
