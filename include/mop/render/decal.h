/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * decal.h — Deferred decal projection API
 *
 * Decals are projected onto scene geometry using the depth buffer.
 * Each decal is a box volume; any surface fragment inside the box
 * receives the decal texture with alpha blending and edge fade.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_RENDER_DECAL_H
#define MOP_RENDER_DECAL_H

#include <mop/types.h>

typedef struct MopViewport MopViewport;
typedef struct MopRhiTexture MopRhiTexture;

typedef struct MopDecalDesc {
  MopMat4 transform;   /* position, rotation, scale of the decal box */
  float opacity;       /* 0..1 alpha multiplier */
  int32_t texture_idx; /* bindless texture index, or -1 for white */
} MopDecalDesc;

/* Maximum decal ID value (fixed-capacity array in GPU backend). */
#define MOP_MAX_DECALS 256

/* Add a decal to the scene.  Returns a decal ID (0..MOP_MAX_DECALS-1),
 * or -1 on failure (capacity exceeded, no GPU backend). */
int32_t mop_viewport_add_decal(MopViewport *vp, const MopDecalDesc *desc);

/* Remove a previously added decal by ID. */
void mop_viewport_remove_decal(MopViewport *vp, int32_t decal_id);

/* Remove all decals. */
void mop_viewport_clear_decals(MopViewport *vp);

#endif /* MOP_RENDER_DECAL_H */
