/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * particle.h — Particle system types and API
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_PARTICLE_H
#define MOP_PARTICLE_H

#include "types.h"

/* Forward declarations */
typedef struct MopViewport MopViewport;
typedef struct MopTexture  MopTexture;

/* -------------------------------------------------------------------------
 * Particle — single particle state (internal, exposed for inspection)
 * ------------------------------------------------------------------------- */

typedef struct MopParticle {
    MopVec3  position;
    MopVec3  velocity;
    float    lifetime;
    float    max_lifetime;
    float    size;
    MopColor color;
    bool     alive;
} MopParticle;

/* -------------------------------------------------------------------------
 * Particle emitter descriptor
 *
 * Describes the behavior of a particle emitter.  Passed to
 * mop_viewport_add_emitter to create an emitter.
 * ------------------------------------------------------------------------- */

typedef struct MopParticleEmitterDesc {
    uint32_t    max_particles;

    float       emit_rate;         /* particles per second */

    float       lifetime_min;
    float       lifetime_max;

    MopVec3     position;

    MopVec3     velocity_min;
    MopVec3     velocity_max;

    MopVec3     gravity;

    float       size_start;
    float       size_end;

    MopColor    color_start;
    MopColor    color_end;

    MopBlendMode blend_mode;

    MopTexture *sprite;            /* optional sprite texture, NULL = no texture */
} MopParticleEmitterDesc;

/* -------------------------------------------------------------------------
 * Opaque emitter handle
 * ------------------------------------------------------------------------- */

typedef struct MopParticleEmitter MopParticleEmitter;

/* -------------------------------------------------------------------------
 * Emitter lifecycle
 * ------------------------------------------------------------------------- */

/* Add a particle emitter to the viewport.  Returns NULL on failure. */
MopParticleEmitter *mop_viewport_add_emitter(MopViewport *viewport,
                                              const MopParticleEmitterDesc *desc);

/* Remove an emitter from the viewport and free its resources. */
void mop_viewport_remove_emitter(MopViewport *viewport,
                                  MopParticleEmitter *emitter);

/* -------------------------------------------------------------------------
 * Emitter configuration
 * ------------------------------------------------------------------------- */

/* Set the emitter's world-space position. */
void mop_emitter_set_position(MopParticleEmitter *emitter, MopVec3 position);

/* Set the emission rate (particles per second). */
void mop_emitter_set_rate(MopParticleEmitter *emitter, float rate);

/* Enable or disable emission.  Existing particles continue to live. */
void mop_emitter_set_active(MopParticleEmitter *emitter, bool active);

/* -------------------------------------------------------------------------
 * Emitter simulation and mesh generation (internal, called by viewport)
 * ------------------------------------------------------------------------- */

/* Advance the emitter by dt seconds.  cam_right and cam_up are used
 * to orient billboard quads toward the camera. */
void mop_emitter_update(MopParticleEmitter *emitter, float dt,
                         MopVec3 cam_right, MopVec3 cam_up);

/* Retrieve the current billboard mesh data for rendering.
 * Pointers are valid until the next mop_emitter_update call. */
void mop_emitter_get_mesh_data(const MopParticleEmitter *emitter,
                                const MopVertex **vertices,
                                uint32_t *vertex_count,
                                const uint32_t **indices,
                                uint32_t *index_count);

/* -------------------------------------------------------------------------
 * Preset emitter descriptors (Phase 8C)
 * ------------------------------------------------------------------------- */

MopParticleEmitterDesc mop_particle_preset_smoke(void);
MopParticleEmitterDesc mop_particle_preset_fire(void);
MopParticleEmitterDesc mop_particle_preset_sparks(void);

#endif /* MOP_PARTICLE_H */
