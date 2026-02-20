/*
 * Master of Puppets — Particle System
 * particle.c — Particle pool, emission, simulation, and billboard generation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/particle.h>
#include <mop/log.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Simple pseudo-random number generator (xorshift32)
 *
 * We avoid depending on rand() / srand() for deterministic behavior
 * across platforms.  A per-emitter seed is sufficient.
 * ------------------------------------------------------------------------- */

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Return a float in [0, 1) */
static float rand_float(uint32_t *state) {
    return (float)(xorshift32(state) & 0x00FFFFFFu) / (float)0x01000000u;
}

/* Return a float in [lo, hi] */
static float rand_range(uint32_t *state, float lo, float hi) {
    return lo + rand_float(state) * (hi - lo);
}

/* -------------------------------------------------------------------------
 * Emitter structure
 * ------------------------------------------------------------------------- */

struct MopParticleEmitter {
    MopParticleEmitterDesc desc;

    /* Particle pool */
    MopParticle *particles;
    uint32_t     max_particles;

    /* Emission accumulator (fractional particle carry) */
    float        emit_accum;

    /* Active flag — when false, no new particles are spawned */
    bool         active;

    /* RNG state */
    uint32_t     rng_state;

    /* Billboard mesh data (regenerated each update) */
    MopVertex   *verts;
    uint32_t    *idxs;
    uint32_t     vert_count;
    uint32_t     idx_count;
};

/* -------------------------------------------------------------------------
 * Emitter lifecycle
 * ------------------------------------------------------------------------- */

MopParticleEmitter *mop_viewport_add_emitter(MopViewport *viewport,
                                              const MopParticleEmitterDesc *desc) {
    (void)viewport;  /* emitter is standalone; viewport integration is in viewport.c */

    if (!desc || desc->max_particles == 0) {
        MOP_ERROR("invalid particle emitter descriptor");
        return NULL;
    }

    MopParticleEmitter *em = calloc(1, sizeof(MopParticleEmitter));
    if (!em) return NULL;

    em->desc = *desc;
    em->max_particles = desc->max_particles;
    em->active = true;
    em->emit_accum = 0.0f;

    /* Seed RNG from address bits + max_particles for variety */
    em->rng_state = (uint32_t)(uintptr_t)em ^ (desc->max_particles * 2654435761u);
    if (em->rng_state == 0) em->rng_state = 1;

    /* Allocate particle pool */
    em->particles = calloc(desc->max_particles, sizeof(MopParticle));
    if (!em->particles) {
        free(em);
        return NULL;
    }

    /* Allocate billboard mesh buffers (4 verts + 6 indices per particle) */
    em->verts = malloc(desc->max_particles * 4 * sizeof(MopVertex));
    em->idxs  = malloc(desc->max_particles * 6 * sizeof(uint32_t));
    if (!em->verts || !em->idxs) {
        free(em->particles);
        free(em->verts);
        free(em->idxs);
        free(em);
        return NULL;
    }

    em->vert_count = 0;
    em->idx_count  = 0;

    return em;
}

void mop_viewport_remove_emitter(MopViewport *viewport,
                                  MopParticleEmitter *emitter) {
    (void)viewport;
    if (!emitter) return;

    free(emitter->particles);
    free(emitter->verts);
    free(emitter->idxs);
    free(emitter);
}

/* -------------------------------------------------------------------------
 * Emitter configuration
 * ------------------------------------------------------------------------- */

void mop_emitter_set_position(MopParticleEmitter *emitter, MopVec3 position) {
    if (!emitter) return;
    emitter->desc.position = position;
}

void mop_emitter_set_rate(MopParticleEmitter *emitter, float rate) {
    if (!emitter) return;
    emitter->desc.emit_rate = rate;
}

void mop_emitter_set_active(MopParticleEmitter *emitter, bool active) {
    if (!emitter) return;
    emitter->active = active;
}

/* -------------------------------------------------------------------------
 * Simulation and billboard generation
 * ------------------------------------------------------------------------- */

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static MopColor lerp_color(MopColor a, MopColor b, float t) {
    MopColor c;
    c.r = lerpf(a.r, b.r, t);
    c.g = lerpf(a.g, b.g, t);
    c.b = lerpf(a.b, b.b, t);
    c.a = lerpf(a.a, b.a, t);
    return c;
}

void mop_emitter_update(MopParticleEmitter *emitter, float dt,
                         MopVec3 cam_right, MopVec3 cam_up) {
    if (!emitter || dt <= 0.0f) return;

    MopParticleEmitterDesc *d = &emitter->desc;

    /* --- Update existing alive particles --- */
    for (uint32_t i = 0; i < emitter->max_particles; i++) {
        MopParticle *p = &emitter->particles[i];
        if (!p->alive) continue;

        /* Integrate */
        p->velocity.x += d->gravity.x * dt;
        p->velocity.y += d->gravity.y * dt;
        p->velocity.z += d->gravity.z * dt;

        p->position.x += p->velocity.x * dt;
        p->position.y += p->velocity.y * dt;
        p->position.z += p->velocity.z * dt;

        p->lifetime += dt;

        /* Kill expired */
        if (p->lifetime >= p->max_lifetime) {
            p->alive = false;
        }
    }

    /* --- Spawn new particles --- */
    if (emitter->active && d->emit_rate > 0.0f) {
        emitter->emit_accum += d->emit_rate * dt;
        uint32_t to_spawn = (uint32_t)emitter->emit_accum;
        emitter->emit_accum -= (float)to_spawn;

        for (uint32_t s = 0; s < to_spawn; s++) {
            /* Find a dead slot */
            uint32_t slot = emitter->max_particles;
            for (uint32_t i = 0; i < emitter->max_particles; i++) {
                if (!emitter->particles[i].alive) {
                    slot = i;
                    break;
                }
            }
            if (slot >= emitter->max_particles) break;  /* pool full */

            MopParticle *p = &emitter->particles[slot];
            p->alive = true;
            p->lifetime = 0.0f;
            p->max_lifetime = rand_range(&emitter->rng_state,
                                          d->lifetime_min, d->lifetime_max);
            p->position = d->position;
            p->velocity.x = rand_range(&emitter->rng_state,
                                        d->velocity_min.x, d->velocity_max.x);
            p->velocity.y = rand_range(&emitter->rng_state,
                                        d->velocity_min.y, d->velocity_max.y);
            p->velocity.z = rand_range(&emitter->rng_state,
                                        d->velocity_min.z, d->velocity_max.z);
            p->size = d->size_start;
            p->color = d->color_start;
        }
    }

    /* --- Generate billboard quads --- */
    uint32_t vi = 0;
    uint32_t ii = 0;

    for (uint32_t i = 0; i < emitter->max_particles; i++) {
        MopParticle *p = &emitter->particles[i];
        if (!p->alive) continue;

        /* Compute interpolation factor t in [0, 1] */
        float t = (p->max_lifetime > 0.0f)
                  ? (p->lifetime / p->max_lifetime)
                  : 1.0f;

        float size = lerpf(d->size_start, d->size_end, t);
        MopColor color = lerp_color(d->color_start, d->color_end, t);

        /* Half-size extents */
        float hs = size * 0.5f;

        /* Billboard corners: center +/- cam_right * hs +/- cam_up * hs */
        MopVec3 r = { cam_right.x * hs, cam_right.y * hs, cam_right.z * hs };
        MopVec3 u = { cam_up.x * hs, cam_up.y * hs, cam_up.z * hs };

        MopVec3 bl = { p->position.x - r.x - u.x,
                       p->position.y - r.y - u.y,
                       p->position.z - r.z - u.z };
        MopVec3 br = { p->position.x + r.x - u.x,
                       p->position.y + r.y - u.y,
                       p->position.z + r.z - u.z };
        MopVec3 tr = { p->position.x + r.x + u.x,
                       p->position.y + r.y + u.y,
                       p->position.z + r.z + u.z };
        MopVec3 tl = { p->position.x - r.x + u.x,
                       p->position.y - r.y + u.y,
                       p->position.z - r.z + u.z };

        /* Normal faces camera (cross of right x up) */
        MopVec3 n = mop_vec3_normalize(mop_vec3_cross(cam_right, cam_up));

        uint32_t base = vi;

        emitter->verts[vi++] = (MopVertex){ bl, n, color, 0.0f, 1.0f };
        emitter->verts[vi++] = (MopVertex){ br, n, color, 1.0f, 1.0f };
        emitter->verts[vi++] = (MopVertex){ tr, n, color, 1.0f, 0.0f };
        emitter->verts[vi++] = (MopVertex){ tl, n, color, 0.0f, 0.0f };

        emitter->idxs[ii++] = base + 0;
        emitter->idxs[ii++] = base + 1;
        emitter->idxs[ii++] = base + 2;
        emitter->idxs[ii++] = base + 2;
        emitter->idxs[ii++] = base + 3;
        emitter->idxs[ii++] = base + 0;
    }

    emitter->vert_count = vi;
    emitter->idx_count  = ii;
}

void mop_emitter_get_mesh_data(const MopParticleEmitter *emitter,
                                const MopVertex **vertices,
                                uint32_t *vertex_count,
                                const uint32_t **indices,
                                uint32_t *index_count) {
    if (!emitter) {
        if (vertices)     *vertices = NULL;
        if (vertex_count) *vertex_count = 0;
        if (indices)      *indices = NULL;
        if (index_count)  *index_count = 0;
        return;
    }

    if (vertices)     *vertices = emitter->verts;
    if (vertex_count) *vertex_count = emitter->vert_count;
    if (indices)      *indices = emitter->idxs;
    if (index_count)  *index_count = emitter->idx_count;
}

/* -------------------------------------------------------------------------
 * Preset emitter descriptors (Phase 8C)
 * ------------------------------------------------------------------------- */

MopParticleEmitterDesc mop_particle_preset_smoke(void) {
    MopParticleEmitterDesc d;
    memset(&d, 0, sizeof(d));

    d.max_particles = 256;
    d.emit_rate     = 30.0f;
    d.lifetime_min  = 2.0f;
    d.lifetime_max  = 4.0f;
    d.position      = (MopVec3){ 0.0f, 0.0f, 0.0f };
    d.velocity_min  = (MopVec3){ -0.2f, 0.5f, -0.2f };
    d.velocity_max  = (MopVec3){  0.2f, 1.5f,  0.2f };
    d.gravity       = (MopVec3){ 0.0f, 0.1f, 0.0f };
    d.size_start    = 0.3f;
    d.size_end      = 1.2f;
    d.color_start   = (MopColor){ 0.5f, 0.5f, 0.5f, 0.6f };
    d.color_end     = (MopColor){ 0.3f, 0.3f, 0.3f, 0.0f };
    d.blend_mode    = MOP_BLEND_ALPHA;
    d.sprite        = NULL;

    return d;
}

MopParticleEmitterDesc mop_particle_preset_fire(void) {
    MopParticleEmitterDesc d;
    memset(&d, 0, sizeof(d));

    d.max_particles = 512;
    d.emit_rate     = 60.0f;
    d.lifetime_min  = 0.5f;
    d.lifetime_max  = 1.5f;
    d.position      = (MopVec3){ 0.0f, 0.0f, 0.0f };
    d.velocity_min  = (MopVec3){ -0.3f, 1.0f, -0.3f };
    d.velocity_max  = (MopVec3){  0.3f, 3.0f,  0.3f };
    d.gravity       = (MopVec3){ 0.0f, 0.3f, 0.0f };
    d.size_start    = 0.5f;
    d.size_end      = 0.1f;
    d.color_start   = (MopColor){ 1.0f, 0.9f, 0.2f, 1.0f };
    d.color_end     = (MopColor){ 1.0f, 0.1f, 0.0f, 0.0f };
    d.blend_mode    = MOP_BLEND_ADDITIVE;
    d.sprite        = NULL;

    return d;
}

MopParticleEmitterDesc mop_particle_preset_sparks(void) {
    MopParticleEmitterDesc d;
    memset(&d, 0, sizeof(d));

    d.max_particles = 1024;
    d.emit_rate     = 100.0f;
    d.lifetime_min  = 0.3f;
    d.lifetime_max  = 0.8f;
    d.position      = (MopVec3){ 0.0f, 0.0f, 0.0f };
    d.velocity_min  = (MopVec3){ -2.0f, 1.0f, -2.0f };
    d.velocity_max  = (MopVec3){  2.0f, 4.0f,  2.0f };
    d.gravity       = (MopVec3){ 0.0f, -2.0f, 0.0f };
    d.size_start    = 0.05f;
    d.size_end      = 0.02f;
    d.color_start   = (MopColor){ 1.0f, 0.6f, 0.1f, 1.0f };
    d.color_end     = (MopColor){ 1.0f, 0.3f, 0.0f, 0.0f };
    d.blend_mode    = MOP_BLEND_ADDITIVE;
    d.sprite        = NULL;

    return d;
}
