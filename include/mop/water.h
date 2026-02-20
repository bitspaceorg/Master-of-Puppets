/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * water.h — Configurable water surface with sine-wave vertex animation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_WATER_H
#define MOP_WATER_H

#include "types.h"

/* Forward declarations */
typedef struct MopViewport     MopViewport;
typedef struct MopWaterSurface MopWaterSurface;

typedef struct MopWaterDesc {
    float    extent;       /* half-size of the water plane */
    int      resolution;   /* vertices per edge (e.g. 64) */
    float    wave_speed;
    float    wave_amplitude;
    float    wave_frequency;
    MopColor color;
    float    opacity;
} MopWaterDesc;

MopWaterSurface *mop_viewport_add_water(MopViewport *viewport,
                                         const MopWaterDesc *desc);
void mop_viewport_remove_water(MopViewport *viewport,
                                MopWaterSurface *water);
void mop_water_set_time(MopWaterSurface *water, float t);

#endif /* MOP_WATER_H */
