/*
 * Master of Puppets — Example: Physics Engine (Interactive)
 *
 * Real-time gravity simulation with AABB broad-phase collision detection.
 * Space=reset  P=pause  G=flip gravity  W=wireframe  Q/Esc=quit
 *
 * APIs: spatial.h (AABB, overlaps), query.h, scene.h
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include "geometry.h"
#include "sdl_harness.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define NUM_CUBES       5
#define GRAVITY_MAG     9.81f
#define FLOOR_Y         (-1.0f)
#define CUBE_HALF       0.5f
#define BOUNCE_DAMP     0.6f
#define MAX_DT          (1.0f / 30.0f)
#define COLL_LOG_PER_S  3

/* =========================================================================
 * Context
 * ========================================================================= */

typedef struct {
    MopMesh *cubes[5];
    MopMesh *floor_mesh;
    float    vy[5];
    float    start_y[5];
    bool     paused;
    int      collision_count;
    float    gravity_sign;
    float    coll_timer;
    int      coll_this_second;
} PhysicsCtx;

/* =========================================================================
 * Cube colors
 * ========================================================================= */

static const MopColor CUBE_COLORS[NUM_CUBES] = {
    { 0.9f, 0.2f, 0.2f, 1.0f },   /* red    */
    { 0.2f, 0.9f, 0.3f, 1.0f },   /* green  */
    { 0.2f, 0.4f, 0.9f, 1.0f },   /* blue   */
    { 0.9f, 0.8f, 0.1f, 1.0f },   /* yellow */
    { 0.8f, 0.2f, 0.9f, 1.0f },   /* purple */
};

/* =========================================================================
 * Callbacks
 * ========================================================================= */

static void physics_setup(MopViewport *vp, void *ctx)
{
    PhysicsCtx *pc = (PhysicsCtx *)ctx;

    /* Camera */
    mop_viewport_set_camera(vp,
        (MopVec3){ 6.0f, 5.0f, 10.0f },
        (MopVec3){ 0.0f, 0.0f,  0.0f },
        (MopVec3){ 0.0f, 1.0f,  0.0f },
        60.0f, 0.1f, 100.0f);

    /* Ambient lighting */
    mop_viewport_set_ambient(vp, 0.2f);

    /* Directional light */
    mop_viewport_add_light(vp, &(MopLight){
        .type      = MOP_LIGHT_DIRECTIONAL,
        .direction = { 0.4f, 1.0f, 0.3f },
        .color     = { 1.0f, 0.98f, 0.9f, 1.0f },
        .intensity = 1.2f,
        .active    = true,
    });

    /* Floor plane at y=-1 */
    pc->floor_mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){
        .vertices     = PLANE_VERTICES,
        .vertex_count = PLANE_VERTEX_COUNT,
        .indices      = PLANE_INDICES,
        .index_count  = PLANE_INDEX_COUNT,
        .object_id    = 100,
    });
    mop_mesh_set_position(pc->floor_mesh, (MopVec3){ 0.0f, FLOOR_Y, 0.0f });
    mop_mesh_set_material(pc->floor_mesh, &(MopMaterial){
        .base_color = { 0.4f, 0.4f, 0.45f, 1.0f },
        .metallic   = 0.0f,
        .roughness  = 0.9f,
    });

    /* 5 cubes at staggered heights */
    float start_heights[NUM_CUBES] = { 3.0f, 5.0f, 4.0f, 6.0f, 7.5f };
    float x_offsets[NUM_CUBES]     = { -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };

    for (int i = 0; i < NUM_CUBES; i++) {
        pc->cubes[i] = mop_viewport_add_mesh(vp, &(MopMeshDesc){
            .vertices     = CUBE_VERTICES,
            .vertex_count = CUBE_VERTEX_COUNT,
            .indices      = CUBE_INDICES,
            .index_count  = CUBE_INDEX_COUNT,
            .object_id    = (uint32_t)(i + 1),
        });

        MopVec3 pos = { x_offsets[i], start_heights[i], 0.0f };
        mop_mesh_set_position(pc->cubes[i], pos);
        mop_mesh_set_material(pc->cubes[i], &(MopMaterial){
            .base_color = CUBE_COLORS[i],
            .metallic   = 0.3f,
            .roughness  = 0.5f,
        });

        pc->vy[i]      = 0.0f;
        pc->start_y[i] = start_heights[i];
    }

    pc->paused           = false;
    pc->collision_count  = 0;
    pc->gravity_sign     = -1.0f;
    pc->coll_timer       = 0.0f;
    pc->coll_this_second = 0;

    printf("[physics] Scene ready: floor(id=100) + %d cubes (ids 1-%d)\n",
           NUM_CUBES, NUM_CUBES);
    printf("[physics] Space=reset  P=pause  G=flip gravity\n");
}

static void physics_update(MopViewport *vp, float dt, void *ctx)
{
    PhysicsCtx *pc = (PhysicsCtx *)ctx;
    if (pc->paused) return;

    /* Clamp dt */
    if (dt > MAX_DT) dt = MAX_DT;

    float gravity = pc->gravity_sign * GRAVITY_MAG;

    /* Reset per-second collision counter */
    pc->coll_timer += dt;
    if (pc->coll_timer >= 1.0f) {
        pc->coll_timer      -= 1.0f;
        pc->coll_this_second = 0;
    }

    /* Integrate velocity and position */
    for (int i = 0; i < NUM_CUBES; i++) {
        pc->vy[i] += gravity * dt;

        MopVec3 pos = mop_mesh_get_position(pc->cubes[i]);
        pos.y += pc->vy[i] * dt;

        /* Floor bounce */
        if (pos.y < FLOOR_Y + CUBE_HALF) {
            pos.y = FLOOR_Y + CUBE_HALF;
            pc->vy[i] = -pc->vy[i] * BOUNCE_DAMP;
        }

        mop_mesh_set_position(pc->cubes[i], pos);
    }

    /* AABB overlap check between cube pairs */
    for (int i = 0; i < NUM_CUBES; i++) {
        MopAABB ai = mop_mesh_get_aabb_world(pc->cubes[i], vp);

        for (int j = i + 1; j < NUM_CUBES; j++) {
            MopAABB aj = mop_mesh_get_aabb_world(pc->cubes[j], vp);

            if (mop_aabb_overlaps(ai, aj)) {
                pc->collision_count++;

                if (pc->coll_this_second < COLL_LOG_PER_S) {
                    printf("  [collision #%d] cube %d <-> cube %d\n",
                           pc->collision_count, i + 1, j + 1);
                    pc->coll_this_second++;
                }
            }
        }
    }
}

static bool physics_on_key(MopViewport *vp, SDL_Keycode key, void *ctx)
{
    PhysicsCtx *pc = (PhysicsCtx *)ctx;
    (void)vp;

    switch (key) {

    case SDLK_SPACE:
        /* Reset cubes to starting heights */
        for (int i = 0; i < NUM_CUBES; i++) {
            MopVec3 pos = mop_mesh_get_position(pc->cubes[i]);
            pos.y = pc->start_y[i];
            mop_mesh_set_position(pc->cubes[i], pos);
            pc->vy[i] = 0.0f;
        }
        pc->collision_count  = 0;
        pc->coll_this_second = 0;
        pc->coll_timer       = 0.0f;
        printf("[physics] Reset: cubes returned to start heights\n");
        return true;

    case SDLK_P:
        pc->paused = !pc->paused;
        printf("[physics] %s\n", pc->paused ? "Paused" : "Resumed");
        return true;

    case SDLK_G:
        pc->gravity_sign = -pc->gravity_sign;
        printf("[physics] Gravity flipped: %s\n",
               pc->gravity_sign < 0.0f ? "downward" : "upward");
        return true;

    default:
        return false;
    }
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    PhysicsCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    MopSDLApp app = {
        .title   = "MOP — Physics Engine",
        .width   = 800,
        .height  = 600,
        .setup   = physics_setup,
        .update  = physics_update,
        .on_key  = physics_on_key,
        .ctx     = &ctx,
    };

    return mop_sdl_run(&app);
}
