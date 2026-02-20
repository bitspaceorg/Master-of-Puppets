/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * camera_query.h — Camera matrix and parameter export
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CAMERA_QUERY_H
#define MOP_CAMERA_QUERY_H

#include "types.h"

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Camera state snapshot
 *
 * All fields needed to reconstruct camera rays or match the viewport
 * camera in an external renderer.
 * ------------------------------------------------------------------------- */

typedef struct MopCameraState {
    MopVec3 eye;
    MopVec3 target;
    MopVec3 up;
    float   fov_radians;
    float   near_plane;
    float   far_plane;
    float   aspect_ratio;
    MopMat4 view_matrix;
    MopMat4 projection_matrix;
} MopCameraState;

/* Retrieve a complete snapshot of the current camera state.
 * All matrices reflect the most recent mop_viewport_render or
 * mop_viewport_set_camera call. */
MopCameraState mop_viewport_get_camera_state(const MopViewport *vp);

/* Individual getters */
MopMat4 mop_viewport_get_view_matrix(const MopViewport *vp);
MopMat4 mop_viewport_get_projection_matrix(const MopViewport *vp);
float   mop_viewport_get_fov(const MopViewport *vp);
float   mop_viewport_get_near_plane(const MopViewport *vp);
float   mop_viewport_get_far_plane(const MopViewport *vp);
float   mop_viewport_get_aspect_ratio(const MopViewport *vp);
MopVec3 mop_viewport_get_camera_up(const MopViewport *vp);

/* -------------------------------------------------------------------------
 * Ray type — origin + normalized direction
 * ------------------------------------------------------------------------- */

typedef struct MopRay {
    MopVec3 origin;
    MopVec3 direction;
} MopRay;

/* -------------------------------------------------------------------------
 * Ray generation
 *
 * Given pixel coordinates (x, y) in framebuffer space (top-left origin),
 * compute the world-space ray origin and direction for that pixel.
 * This is the fundamental operation a raytracer needs to begin tracing.
 * ------------------------------------------------------------------------- */

MopRay mop_viewport_pixel_to_ray(const MopViewport *vp, float x, float y);

#endif /* MOP_CAMERA_QUERY_H */
