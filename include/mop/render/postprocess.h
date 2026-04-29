/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * postprocess.h — Per-pixel post-processing effects
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_RENDER_POSTPROCESS_H
#define MOP_RENDER_POSTPROCESS_H

#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct MopViewport MopViewport;

typedef enum MopPostEffect {
  MOP_POST_NONE = 0,
  MOP_POST_GAMMA = 1 << 0,
  MOP_POST_TONEMAP = 1 << 1,
  MOP_POST_VIGNETTE = 1 << 2,
  MOP_POST_FOG = 1 << 3,
  MOP_POST_FXAA = 1 << 4,
  MOP_POST_BLOOM = 1 << 5,
  MOP_POST_SSAO = 1 << 6,
  MOP_POST_TAA = 1 << 7,
  MOP_POST_SSR = 1 << 8,
  MOP_POST_OIT = 1 << 9,
  MOP_POST_VOLUMETRIC = 1 << 10
} MopPostEffect;

typedef struct MopFogParams {
  MopColor color;
  float near_dist;
  float far_dist;
} MopFogParams;

void mop_viewport_set_post_effects(MopViewport *viewport, uint32_t effects);
void mop_viewport_set_fog(MopViewport *viewport, const MopFogParams *fog);

/* HDR exposure control — multiplier applied before ACES Filmic tonemapping.
 * Default is 1.0.  Higher values brighten, lower values darken. */
void mop_viewport_set_exposure(MopViewport *viewport, float exposure);
float mop_viewport_get_exposure(const MopViewport *viewport);

/* Bloom parameters — threshold is the HDR brightness above which pixels
 * contribute to bloom; intensity controls how much bloom is composited.
 * Default: threshold=1.0, intensity=0.5. */
void mop_viewport_set_bloom(MopViewport *viewport, float threshold,
                            float intensity);

/* Screen-Space Reflections control.
 * intensity: reflection strength (0..1).  Default: 0.5. */
void mop_viewport_set_ssr(MopViewport *viewport, float intensity);

/* Volumetric fog parameters.
 * density: fog density (0..inf, typical 0.01-0.1).
 * color: scattering color (light scattered towards camera).
 * anisotropy: Henyey-Greenstein g parameter (-1..1).
 *   0 = isotropic, >0 forward scatter (god rays), <0 back scatter.
 * steps: raymarch steps (4..64, default 32).  More = quality, less = speed. */
typedef struct MopVolumetricParams {
  float density;
  MopColor color;
  float anisotropy;
  int steps;
} MopVolumetricParams;

void mop_viewport_set_volumetric(MopViewport *viewport,
                                 const MopVolumetricParams *params);

#ifdef __cplusplus
}
#endif

#endif /* MOP_RENDER_POSTPROCESS_H */
