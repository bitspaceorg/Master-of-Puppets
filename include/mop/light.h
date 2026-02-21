/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * light.h — Multi-light system
 *
 * Supports directional, point, and spot lights.  Up to MOP_MAX_LIGHTS
 * may be active per viewport simultaneously.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_LIGHT_H
#define MOP_LIGHT_H

#include "types.h"

#define MOP_MAX_LIGHTS 8

/* -------------------------------------------------------------------------
 * Light types
 * ------------------------------------------------------------------------- */

typedef enum MopLightType {
  MOP_LIGHT_DIRECTIONAL = 0,
  MOP_LIGHT_POINT = 1,
  MOP_LIGHT_SPOT = 2,
} MopLightType;

/* -------------------------------------------------------------------------
 * Light descriptor
 * ------------------------------------------------------------------------- */

typedef struct MopLight {
  MopLightType type;
  MopVec3 position;     /* world space (point/spot) */
  MopVec3 direction;    /* normalized (directional/spot) */
  MopColor color;       /* linear RGB */
  float intensity;      /* multiplier */
  float range;          /* attenuation cutoff (point/spot) */
  float spot_inner_cos; /* cos(inner_cone_angle) */
  float spot_outer_cos; /* cos(outer_cone_angle) */
  bool active;
  bool cast_shadows; /* reserved for future use */
} MopLight;

/* -------------------------------------------------------------------------
 * Viewport light management
 *
 * Viewport 0 is always a directional light matching the legacy
 * mop_viewport_set_light_dir() / set_ambient() behavior.
 * ------------------------------------------------------------------------- */

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* Add a light to the viewport.  Returns pointer to the slot, or NULL
 * if MOP_MAX_LIGHTS is reached.  The viewport copies the descriptor. */
MopLight *mop_viewport_add_light(MopViewport *vp, const MopLight *desc);

/* Remove a light (sets active = false). */
void mop_viewport_remove_light(MopViewport *vp, MopLight *light);

/* Per-light property setters */
void mop_light_set_position(MopLight *l, MopVec3 pos);
void mop_light_set_direction(MopLight *l, MopVec3 dir);
void mop_light_set_color(MopLight *l, MopColor color);
void mop_light_set_intensity(MopLight *l, float intensity);

/* Query */
uint32_t mop_viewport_light_count(const MopViewport *vp);

#endif /* MOP_LIGHT_H */
