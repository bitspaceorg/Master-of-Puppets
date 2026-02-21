/*
 * Master of Puppets — Subsystem Interface
 * subsystem.h — VTable and registry for pluggable simulation/effect subsystems
 *
 * Each subsystem (water, particle, postprocess, etc.) embeds a MopSubsystem
 * as its first field, enabling first-field casting.  The viewport dispatches
 * all registered subsystems by phase each frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_SUBSYSTEM_H
#define MOP_SUBSYSTEM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct MopViewport MopViewport;
typedef struct MopSubsystem MopSubsystem;

/* -------------------------------------------------------------------------
 * Subsystem phase — when in the frame each subsystem runs
 * ------------------------------------------------------------------------- */

typedef enum MopSubsystemPhase {
  MOP_SUBSYS_PHASE_SIMULATE,    /* before rendering (water waves, particles) */
  MOP_SUBSYS_PHASE_POST_RENDER, /* after rasterization (postprocess effects) */
} MopSubsystemPhase;

/* -------------------------------------------------------------------------
 * Virtual function table — every subsystem type provides one
 * ------------------------------------------------------------------------- */

typedef struct MopSubsystemVTable {
  const char *name;
  MopSubsystemPhase phase;
  void (*update)(MopSubsystem *self, MopViewport *vp, float dt, float t);
  void (*destroy)(MopSubsystem *self, MopViewport *vp);
} MopSubsystemVTable;

/* -------------------------------------------------------------------------
 * Base struct — must be the first field in every subsystem struct
 *
 * This enables C first-field casting:
 *   MopWaterSurface *ws = (MopWaterSurface *)subsystem;
 * ------------------------------------------------------------------------- */

struct MopSubsystem {
  const MopSubsystemVTable *vtable;
  bool enabled;
};

/* -------------------------------------------------------------------------
 * Registry — fixed-size array owned by the viewport
 * ------------------------------------------------------------------------- */

#define MOP_MAX_SUBSYSTEMS 32

typedef struct MopSubsystemRegistry {
  MopSubsystem *entries[MOP_MAX_SUBSYSTEMS];
  uint32_t count;
} MopSubsystemRegistry;

/* Register a subsystem.  Returns false if the registry is full. */
bool mop_subsystem_register(MopSubsystemRegistry *reg, MopSubsystem *sub);

/* Remove a subsystem from the registry.  Returns false if not found. */
bool mop_subsystem_unregister(MopSubsystemRegistry *reg, MopSubsystem *sub);

/* Dispatch update to all enabled subsystems matching the given phase. */
void mop_subsystem_dispatch(MopSubsystemRegistry *reg, MopSubsystemPhase phase,
                            MopViewport *vp, float dt, float t);

/* Destroy all registered subsystems (calls each vtable->destroy). */
void mop_subsystem_destroy_all(MopSubsystemRegistry *reg, MopViewport *vp);

#endif /* MOP_SUBSYSTEM_H */
