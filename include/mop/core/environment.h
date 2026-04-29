/*
 * Master of Puppets — Environment API
 * environment.h — HDR environment maps and procedural sky
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_ENVIRONMENT_H
#define MOP_CORE_ENVIRONMENT_H

#include <mop/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Environment type
 * ------------------------------------------------------------------------- */

typedef enum MopEnvironmentType {
  MOP_ENV_NONE = 0,
  MOP_ENV_GRADIENT = 1,      /* default gradient background */
  MOP_ENV_HDRI = 2,          /* equirectangular .hdr file */
  MOP_ENV_PROCEDURAL_SKY = 3 /* analytical sky model */
} MopEnvironmentType;

/* -------------------------------------------------------------------------
 * Environment descriptor
 * ------------------------------------------------------------------------- */

typedef struct MopEnvironmentDesc {
  MopEnvironmentType type;
  const char *hdr_path; /* path to .hdr or .exr file (MOP_ENV_HDRI only) */
  float rotation;       /* Y-axis rotation in radians */
  float intensity;      /* brightness multiplier (default 1.0) */
} MopEnvironmentDesc;

/* -------------------------------------------------------------------------
 * Procedural sky descriptor
 * ------------------------------------------------------------------------- */

typedef struct MopProceduralSkyDesc {
  MopVec3 sun_direction;
  float turbidity;     /* atmospheric turbidity (2-10) */
  float ground_albedo; /* ground reflectivity (0-1) */
} MopProceduralSkyDesc;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Set the viewport environment (skybox + IBL source).
 * Returns true on success, false on failure (e.g. file not found). */
bool mop_viewport_set_environment(MopViewport *vp,
                                  const MopEnvironmentDesc *desc);

/* Adjust environment Y-axis rotation (radians). */
void mop_viewport_set_environment_rotation(MopViewport *vp, float rotation);

/* Adjust environment brightness multiplier. */
void mop_viewport_set_environment_intensity(MopViewport *vp, float intensity);

/* Show/hide HDRI as skybox background (default: false = gray gradient). */
void mop_viewport_set_environment_background(MopViewport *vp, bool show);

/* Set procedural sky parameters (only when type == MOP_ENV_PROCEDURAL_SKY). */
void mop_viewport_set_procedural_sky(MopViewport *vp,
                                     const MopProceduralSkyDesc *desc);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_ENVIRONMENT_H */
