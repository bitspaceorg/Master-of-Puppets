/*
 * Master of Puppets — Example: Multi-Pass Pipeline (Interactive)
 *
 * Visualizes the 7-stage pipeline hook system in real-time.
 * 1-7=toggle hooks  V=verbose  S=stats  W=wireframe  Q/Esc=quit
 *
 * APIs: pipeline.h, snapshot.h, spatial.h, viewport.h
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include "geometry.h"
#include "sdl_harness.h"
#include <math.h>
#include <float.h>

/* =========================================================================
 * Pipeline stage names (indexed by MopPipelineStage)
 * ========================================================================= */

static const char *STAGE_NAMES[7] = {
    "PRE_RENDER", "POST_CLEAR", "PRE_SCENE", "POST_OPAQUE",
    "POST_SCENE", "POST_OVERLAY", "POST_RENDER"
};

/* =========================================================================
 * Context
 * ========================================================================= */

typedef struct {
    uint32_t handles[7];
    bool     active[7];
    int      counts[7];
    int      frame;
    uint32_t tri_count;
    uint32_t visible;
    float    brightness;
    bool     verbose;
} MultipassCtx;

/* =========================================================================
 * Per-hook data — small struct carrying stage index + context pointer
 * ========================================================================= */

typedef struct { MultipassCtx *ctx; int stage; } HookData;
static HookData g_hook_data[7];

/* =========================================================================
 * Generic hook — fires for most stages, increments counter
 * ========================================================================= */

static void generic_hook(MopViewport *vp, void *user_data)
{
    (void)vp;
    HookData *hd = (HookData *)user_data;
    MultipassCtx *ctx = hd->ctx;
    int s = hd->stage;

    ctx->counts[s]++;

    if (ctx->verbose) {
        printf("  [frame %d] %-14s  fired (call #%d)\n",
               ctx->frame, STAGE_NAMES[s], ctx->counts[s]);
    }
}

/* =========================================================================
 * POST_OPAQUE hook — snapshot + count triangles + visible meshes
 * ========================================================================= */

static void post_opaque_hook(MopViewport *vp, void *user_data)
{
    HookData *hd = (HookData *)user_data;
    MultipassCtx *ctx = hd->ctx;
    int s = hd->stage;

    ctx->counts[s]++;

    MopSceneSnapshot snap = mop_viewport_snapshot(vp);
    ctx->tri_count = mop_snapshot_triangle_count(&snap);
    ctx->visible   = mop_viewport_visible_mesh_count(vp);

    if (ctx->verbose) {
        printf("  [frame %d] %-14s  tris=%u  visible=%u\n",
               ctx->frame, STAGE_NAMES[s], ctx->tri_count, ctx->visible);
    }
}

/* =========================================================================
 * POST_OVERLAY hook — read framebuffer, compute average brightness
 * ========================================================================= */

static void post_overlay_hook(MopViewport *vp, void *user_data)
{
    HookData *hd = (HookData *)user_data;
    MultipassCtx *ctx = hd->ctx;
    int s = hd->stage;

    ctx->counts[s]++;

    int w, h;
    const uint8_t *px = mop_viewport_read_color(vp, &w, &h);

    float brightness = 0.0f;
    if (px && w > 0 && h > 0) {
        uint64_t sum = 0;
        int total = w * h;
        for (int i = 0; i < total; i++) {
            int idx = i * 4;
            /* Luminance approx: (2R + 3G + B) / 6 */
            sum += (uint64_t)px[idx] * 2
                 + (uint64_t)px[idx + 1] * 3
                 + (uint64_t)px[idx + 2];
        }
        brightness = (float)sum / (float)(total * 6) / 255.0f;
    }
    ctx->brightness = brightness;

    if (ctx->verbose) {
        printf("  [frame %d] %-14s  avg_brightness=%.4f  (%dx%d)\n",
               ctx->frame, STAGE_NAMES[s], brightness, w, h);
    }
}

/* =========================================================================
 * POST_RENDER hook — print frame summary if verbose
 * ========================================================================= */

static void post_render_hook(MopViewport *vp, void *user_data)
{
    (void)vp;
    HookData *hd = (HookData *)user_data;
    MultipassCtx *ctx = hd->ctx;
    int s = hd->stage;

    ctx->counts[s]++;

    if (ctx->verbose) {
        int active_count = 0;
        for (int i = 0; i < 7; i++)
            if (ctx->active[i]) active_count++;

        printf("  [frame %d] %-14s  === summary: tris=%u  visible=%u  "
               "brightness=%.4f  active_hooks=%d ===\n",
               ctx->frame, STAGE_NAMES[s],
               ctx->tri_count, ctx->visible,
               ctx->brightness, active_count);
    }
}

/* =========================================================================
 * Hook function dispatch table
 * ========================================================================= */

static MopPipelineHookFn hook_fns[7] = {
    generic_hook,        /* PRE_RENDER   */
    generic_hook,        /* POST_CLEAR   */
    generic_hook,        /* PRE_SCENE    */
    post_opaque_hook,    /* POST_OPAQUE  */
    generic_hook,        /* POST_SCENE   */
    post_overlay_hook,   /* POST_OVERLAY */
    post_render_hook,    /* POST_RENDER  */
};

/* =========================================================================
 * Frame callback — lightweight pre/post frame notification
 * ========================================================================= */

static void frame_callback(MopViewport *vp, bool is_pre_render, void *data)
{
    (void)vp;
    MultipassCtx *ctx = (MultipassCtx *)data;

    if (is_pre_render) {
        if (ctx->verbose) {
            printf("\n--- frame %d begin ---\n", ctx->frame + 1);
        }
    } else {
        if (ctx->verbose) {
            printf("--- frame %d end ---\n", ctx->frame);
        }
    }
}

/* =========================================================================
 * Harness callbacks
 * ========================================================================= */

static void setup(MopViewport *vp, void *user)
{
    MultipassCtx *ctx = (MultipassCtx *)user;

    /* Camera */
    mop_viewport_set_camera(vp,
        (MopVec3){ 3.0f, 3.0f, 5.0f },
        (MopVec3){ 0.0f, 0.0f, 0.0f },
        (MopVec3){ 0.0f, 1.0f, 0.0f },
        60.0f, 0.1f, 50.0f);

    mop_viewport_set_clear_color(vp, (MopColor){ 0.1f, 0.1f, 0.15f, 1.0f });
    mop_viewport_set_ambient(vp, 0.2f);

    /* 3 cubes at (0,0,0), (2,0,0), (-2,0,0) */
    MopVec3 positions[3] = {
        {  0.0f, 0.0f, 0.0f },
        {  2.0f, 0.0f, 0.0f },
        { -2.0f, 0.0f, 0.0f },
    };

    MopMaterial materials[3] = {
        { .base_color = { 0.9f, 0.2f, 0.2f, 1.0f }, .metallic = 0.1f, .roughness = 0.6f },
        { .base_color = { 0.2f, 0.8f, 0.3f, 1.0f }, .metallic = 0.5f, .roughness = 0.3f },
        { .base_color = { 0.2f, 0.3f, 0.9f, 1.0f }, .metallic = 0.8f, .roughness = 0.2f },
    };

    for (int i = 0; i < 3; i++) {
        MopMesh *m = mop_viewport_add_mesh(vp, &(MopMeshDesc){
            .vertices     = CUBE_VERTICES,
            .vertex_count = CUBE_VERTEX_COUNT,
            .indices      = CUBE_INDICES,
            .index_count  = CUBE_INDEX_COUNT,
            .object_id    = (uint32_t)(i + 1),
        });
        mop_mesh_set_position(m, positions[i]);
        mop_mesh_set_material(m, &materials[i]);
    }

    /* Directional light */
    mop_viewport_add_light(vp, &(MopLight){
        .type      = MOP_LIGHT_DIRECTIONAL,
        .direction = { 0.3f, 1.0f, 0.5f },
        .color     = { 1.0f, 1.0f, 0.95f, 1.0f },
        .intensity = 1.0f,
        .active    = true,
    });

    /* Initialize context */
    ctx->verbose = true;
    ctx->frame   = 0;

    /* Initialize hook data and register all 7 hooks */
    for (int i = 0; i < 7; i++) {
        g_hook_data[i].ctx   = ctx;
        g_hook_data[i].stage = i;

        ctx->handles[i] = mop_viewport_add_hook(
            vp, (MopPipelineStage)i, hook_fns[i], &g_hook_data[i]);
        ctx->active[i]  = true;
        ctx->counts[i]  = 0;
    }

    /* Frame callback */
    mop_viewport_set_frame_callback(vp, frame_callback, ctx);

    printf("[multipass] Setup complete: 3 cubes, 1 light, 7 hooks registered\n");
    printf("[multipass] Keys: 1-7=toggle hooks  V=verbose  S=stats  "
           "W=wireframe  Q/Esc=quit\n\n");
}

static void update(MopViewport *vp, float dt, void *user)
{
    (void)vp;
    (void)dt;
    MultipassCtx *ctx = (MultipassCtx *)user;
    ctx->frame++;
}

static bool on_key(MopViewport *vp, SDL_Keycode key, void *user)
{
    MultipassCtx *ctx = (MultipassCtx *)user;

    /* 1-7: toggle individual hooks on/off */
    if (key >= SDLK_1 && key <= SDLK_7) {
        int i = (int)(key - SDLK_1);

        if (ctx->active[i]) {
            /* Remove the hook */
            mop_viewport_remove_hook(vp, ctx->handles[i]);
            ctx->active[i] = false;
            printf("[multipass] Hook %d (%s) DISABLED\n",
                   i + 1, STAGE_NAMES[i]);
        } else {
            /* Re-register the hook */
            ctx->handles[i] = mop_viewport_add_hook(
                vp, (MopPipelineStage)i, hook_fns[i], &g_hook_data[i]);
            ctx->active[i] = true;
            printf("[multipass] Hook %d (%s) ENABLED  (handle=%u)\n",
                   i + 1, STAGE_NAMES[i], ctx->handles[i]);
        }
        return true;
    }

    /* V: toggle verbose output */
    if (key == SDLK_V) {
        ctx->verbose = !ctx->verbose;
        printf("[multipass] Verbose: %s\n",
               ctx->verbose ? "ON" : "OFF");
        return true;
    }

    /* S: print stats summary */
    if (key == SDLK_S) {
        printf("\n========== Pipeline Hook Stats (frame %d) ==========\n",
               ctx->frame);
        int total_calls = 0;
        int active_count = 0;
        for (int i = 0; i < 7; i++) {
            printf("  %d. %-14s  %s  calls=%d  handle=%u\n",
                   i + 1, STAGE_NAMES[i],
                   ctx->active[i] ? "ON " : "OFF",
                   ctx->counts[i],
                   ctx->active[i] ? ctx->handles[i] : 0);
            total_calls += ctx->counts[i];
            if (ctx->active[i]) active_count++;
        }
        printf("  --------------------------------------------------\n");
        printf("  Active hooks:     %d / 7\n", active_count);
        printf("  Total hook calls: %d\n", total_calls);
        printf("  Triangles:        %u\n", ctx->tri_count);
        printf("  Visible meshes:   %u\n", ctx->visible);
        printf("  Avg brightness:   %.4f\n", ctx->brightness);
        printf("  Frames rendered:  %d\n", ctx->frame);
        printf("====================================================\n\n");
        return true;
    }

    return false;
}

static void cleanup(void *user)
{
    (void)user;
    printf("[multipass] Shutdown.\n");
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    static MultipassCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    MopSDLApp app = {
        .title   = "MOP — Multi-Pass Pipeline",
        .width   = 800,
        .height  = 600,
        .setup   = setup,
        .update  = update,
        .on_key  = on_key,
        .cleanup = cleanup,
        .ctx     = &ctx,
    };

    return mop_sdl_run(&app);
}
