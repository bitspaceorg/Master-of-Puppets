/*
 * Master of Puppets — Water Surface
 * water.c — Configurable grid mesh with sine-wave vertex animation
 *
 * Generates a resolution x resolution vertex grid, indexed as
 * (res-1)*(res-1)*2 triangles.  Per-frame update displaces Y via sine
 * waves and recomputes normals from finite differences.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include "rhi/rhi.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* MopWaterSurface struct is defined in viewport_internal.h */

/* Forward declarations for vtable */
static void water_subsys_update(MopSubsystem *self, MopViewport *vp, float dt, float t);
static void water_subsys_destroy(MopSubsystem *self, MopViewport *vp);

static const MopSubsystemVTable water_vtable = {
    .name    = "water",
    .phase   = MOP_SUBSYS_PHASE_SIMULATE,
    .update  = water_subsys_update,
    .destroy = water_subsys_destroy,
};

/* -------------------------------------------------------------------------
 * Grid generation helpers
 * ------------------------------------------------------------------------- */

static void water_generate_grid(MopWaterSurface *ws) {
    int res = ws->resolution;
    float ext = ws->extent;

    ws->vertex_count = (uint32_t)(res * res);
    ws->index_count  = (uint32_t)((res - 1) * (res - 1) * 6);

    ws->vertices = malloc(ws->vertex_count * sizeof(MopVertex));
    ws->indices  = malloc(ws->index_count  * sizeof(uint32_t));
    if (!ws->vertices || !ws->indices) return;

    /* Generate vertices: evenly spaced on xz plane, y=0 */
    for (int z = 0; z < res; z++) {
        for (int x = 0; x < res; x++) {
            uint32_t idx = (uint32_t)(z * res + x);
            float fx = -ext + 2.0f * ext * (float)x / (float)(res - 1);
            float fz = -ext + 2.0f * ext * (float)z / (float)(res - 1);

            ws->vertices[idx].position = (MopVec3){ fx, 0.0f, fz };
            ws->vertices[idx].normal   = (MopVec3){ 0.0f, 1.0f, 0.0f };
            ws->vertices[idx].color    = ws->color;
            ws->vertices[idx].u = (float)x / (float)(res - 1);
            ws->vertices[idx].v = (float)z / (float)(res - 1);
        }
    }

    /* Generate indices: two triangles per quad */
    uint32_t ii = 0;
    for (int z = 0; z < res - 1; z++) {
        for (int x = 0; x < res - 1; x++) {
            uint32_t tl = (uint32_t)(z * res + x);
            uint32_t tr = tl + 1;
            uint32_t bl = (uint32_t)((z + 1) * res + x);
            uint32_t br = bl + 1;

            /* Triangle 1 */
            ws->indices[ii++] = tl;
            ws->indices[ii++] = bl;
            ws->indices[ii++] = tr;

            /* Triangle 2 */
            ws->indices[ii++] = tr;
            ws->indices[ii++] = bl;
            ws->indices[ii++] = br;
        }
    }
}

/* -------------------------------------------------------------------------
 * Sine-wave displacement and normal recomputation
 * ------------------------------------------------------------------------- */

static float water_height(float x, float z, float t,
                           float speed, float amplitude, float frequency) {
    return amplitude * sinf(frequency * (x + t * speed))
                     * sinf(frequency * (z + t * speed * 0.7f));
}

void mop_water_update(MopWaterSurface *ws, float t) {
    if (!ws || !ws->vertices) return;
    if (ws->resolution < 2) return;

    int res = ws->resolution;
    float spd = ws->wave_speed;
    float amp = ws->wave_amplitude;
    float freq = ws->wave_frequency;
    float ext = ws->extent;

    ws->time = t;

    /* Displace vertices */
    for (int z = 0; z < res; z++) {
        for (int x = 0; x < res; x++) {
            uint32_t idx = (uint32_t)(z * res + x);
            float px = ws->vertices[idx].position.x;
            float pz = ws->vertices[idx].position.z;
            ws->vertices[idx].position.y = water_height(px, pz, t,
                                                         spd, amp, freq);
        }
    }

    /* Recompute normals via finite differences */
    float h = 2.0f * ext / (float)(res - 1);
    float eps = h * 0.5f;

    for (int z = 0; z < res; z++) {
        for (int x = 0; x < res; x++) {
            uint32_t idx = (uint32_t)(z * res + x);
            float px = ws->vertices[idx].position.x;
            float pz = ws->vertices[idx].position.z;

            float hL = water_height(px - eps, pz, t, spd, amp, freq);
            float hR = water_height(px + eps, pz, t, spd, amp, freq);
            float hD = water_height(px, pz - eps, t, spd, amp, freq);
            float hU = water_height(px, pz + eps, t, spd, amp, freq);

            MopVec3 n = { (hL - hR) / (2.0f * eps),
                          1.0f,
                          (hD - hU) / (2.0f * eps) };
            ws->vertices[idx].normal = mop_vec3_normalize(n);
        }
    }

    /* Update RHI buffer */
    if (ws->vertex_buffer && ws->viewport->rhi->buffer_update) {
        ws->viewport->rhi->buffer_update(
            ws->viewport->device, ws->vertex_buffer,
            ws->vertices, 0,
            ws->vertex_count * sizeof(MopVertex));
    } else if (ws->vertex_buffer) {
        /* Fallback: destroy and recreate */
        ws->viewport->rhi->buffer_destroy(ws->viewport->device,
                                           ws->vertex_buffer);
        MopRhiBufferDesc vb_desc = {
            .data = ws->vertices,
            .size = ws->vertex_count * sizeof(MopVertex)
        };
        ws->vertex_buffer = ws->viewport->rhi->buffer_create(
            ws->viewport->device, &vb_desc);
        /* Also update the mesh's vertex buffer pointer */
        if (ws->mesh) {
            ws->mesh->vertex_buffer = ws->vertex_buffer;
        }
    }
}

/* -------------------------------------------------------------------------
 * Subsystem vtable adapters
 * ------------------------------------------------------------------------- */

static void water_subsys_update(MopSubsystem *self, MopViewport *vp, float dt, float t) {
    (void)dt;
    MopWaterSurface *ws = (MopWaterSurface *)self;
    mop_water_update(ws, t);
}

static void water_subsys_destroy(MopSubsystem *self, MopViewport *vp) {
    (void)vp;
    MopWaterSurface *ws = (MopWaterSurface *)self;
    /* Free water-specific data; mesh is cleaned up by viewport's mesh loop */
    ws->vertex_buffer = NULL;
    ws->index_buffer  = NULL;
    ws->mesh = NULL;
    free(ws->vertices);
    free(ws->indices);
    free(ws);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

#define MOP_MAX_WATER_RESOLUTION 1024

MopWaterSurface *mop_viewport_add_water(MopViewport *viewport,
                                         const MopWaterDesc *desc) {
    if (!viewport || !desc) return NULL;

    int resolution = desc->resolution;
    if (resolution < 2) {
        MOP_ERROR("water resolution must be >= 2");
        return NULL;
    }
    if (resolution > MOP_MAX_WATER_RESOLUTION) {
        MOP_WARN("water resolution %d capped to %d",
                 resolution, MOP_MAX_WATER_RESOLUTION);
        resolution = MOP_MAX_WATER_RESOLUTION;
    }

    MopWaterSurface *ws = calloc(1, sizeof(MopWaterSurface));
    if (!ws) return NULL;

    /* Initialize subsystem base */
    ws->base.vtable  = &water_vtable;
    ws->base.enabled = true;

    ws->viewport       = viewport;
    ws->extent         = desc->extent;
    ws->resolution     = resolution;
    ws->wave_speed     = desc->wave_speed;
    ws->wave_amplitude = desc->wave_amplitude;
    ws->wave_frequency = desc->wave_frequency;
    ws->color          = desc->color;
    ws->opacity        = desc->opacity;
    ws->time           = 0.0f;

    /* Generate grid geometry */
    water_generate_grid(ws);
    if (!ws->vertices || !ws->indices) {
        free(ws->vertices);
        free(ws->indices);
        free(ws);
        return NULL;
    }

    /* Create a mesh in the viewport for rendering */
    MopMeshDesc md = {
        .vertices     = ws->vertices,
        .vertex_count = ws->vertex_count,
        .indices      = ws->indices,
        .index_count  = ws->index_count,
        .object_id    = 0   /* water is not pickable */
    };
    ws->mesh = mop_viewport_add_mesh(viewport, &md);
    if (!ws->mesh) {
        free(ws->vertices);
        free(ws->indices);
        free(ws);
        return NULL;
    }

    /* Configure mesh for water rendering */
    mop_mesh_set_blend_mode(ws->mesh, MOP_BLEND_ALPHA);
    mop_mesh_set_opacity(ws->mesh, ws->opacity);

    /* Store RHI buffer pointers for dynamic update */
    ws->vertex_buffer = ws->mesh->vertex_buffer;
    ws->index_buffer  = ws->mesh->index_buffer;

    /* Register in viewport's water surface array */
    if (viewport->water_count >= viewport->water_capacity) {
        uint32_t new_cap = viewport->water_capacity == 0
                           ? 4
                           : viewport->water_capacity * 2;
        MopWaterSurface **new_arr = realloc(viewport->water_surfaces,
                                             new_cap * sizeof(MopWaterSurface *));
        if (!new_arr) {
            mop_viewport_remove_mesh(viewport, ws->mesh);
            free(ws->vertices);
            free(ws->indices);
            free(ws);
            return NULL;
        }
        viewport->water_surfaces  = new_arr;
        viewport->water_capacity  = new_cap;
    }
    viewport->water_surfaces[viewport->water_count++] = ws;

    /* Register in the generic subsystem registry for phase-based dispatch */
    mop_subsystem_register(&viewport->subsystems, &ws->base);

    return ws;
}

void mop_viewport_remove_water(MopViewport *viewport,
                                MopWaterSurface *water) {
    if (!viewport || !water) return;

    /* Unregister from subsystem registry */
    mop_subsystem_unregister(&viewport->subsystems, &water->base);

    /* Remove from viewport's water array */
    for (uint32_t i = 0; i < viewport->water_count; i++) {
        if (viewport->water_surfaces[i] == water) {
            viewport->water_surfaces[i] =
                viewport->water_surfaces[viewport->water_count - 1];
            viewport->water_count--;
            break;
        }
    }

    /* Remove the mesh from the viewport */
    if (water->mesh) {
        mop_viewport_remove_mesh(viewport, water->mesh);
        water->mesh = NULL;
    }

    /* vertex/index buffers are owned by the mesh, already freed above */
    water->vertex_buffer = NULL;
    water->index_buffer  = NULL;

    free(water->vertices);
    free(water->indices);
    free(water);
}

void mop_water_set_time(MopWaterSurface *water, float t) {
    if (!water) return;
    mop_water_update(water, t);
}

/* -------------------------------------------------------------------------
 * Internal cleanup — called from viewport destroy
 *
 * Frees water-specific data without touching the viewport mesh (which
 * the viewport destroy loop frees separately).
 * ------------------------------------------------------------------------- */

void mop_water_destroy_internal(MopWaterSurface *ws) {
    if (!ws) return;
    ws->vertex_buffer = NULL;
    ws->index_buffer  = NULL;
    ws->mesh = NULL;
    free(ws->vertices);
    free(ws->indices);
    free(ws);
}
