/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * postprocess.h — Per-pixel post-processing effects
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_RENDER_POSTPROCESS_H
#define MOP_RENDER_POSTPROCESS_H

#include <mop/types.h>

/* Forward declaration */
typedef struct MopViewport MopViewport;

typedef enum MopPostEffect {
  MOP_POST_NONE = 0,
  MOP_POST_GAMMA = 1 << 0,
  MOP_POST_TONEMAP = 1 << 1,
  MOP_POST_VIGNETTE = 1 << 2,
  MOP_POST_FOG = 1 << 3,
  MOP_POST_FXAA = 1 << 4
} MopPostEffect;

typedef struct MopFogParams {
  MopColor color;
  float near_dist;
  float far_dist;
} MopFogParams;

void mop_viewport_set_post_effects(MopViewport *viewport, uint32_t effects);
void mop_viewport_set_fog(MopViewport *viewport, const MopFogParams *fog);

#endif /* MOP_RENDER_POSTPROCESS_H */
