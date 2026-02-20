/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * viewport.h — Viewport creation, rendering, and management
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_VIEWPORT_H
#define MOP_VIEWPORT_H

#include "types.h"
#include "backend.h"

/* -------------------------------------------------------------------------
 * Opaque handle — application never sees internals
 * ------------------------------------------------------------------------- */

typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Viewport descriptor — passed to mop_viewport_create
 *
 * width / height : initial framebuffer dimensions in pixels (must be > 0)
 * backend        : which rendering backend to use
 * ------------------------------------------------------------------------- */

typedef struct MopViewportDesc {
    int            width;
    int            height;
    MopBackendType backend;
} MopViewportDesc;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* Create a viewport.  Returns NULL on failure. */
MopViewport *mop_viewport_create(const MopViewportDesc *desc);

/* Destroy a viewport and release all resources it owns. */
void mop_viewport_destroy(MopViewport *viewport);

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

/* Resize the framebuffer.  width and height must be > 0. */
void mop_viewport_resize(MopViewport *viewport, int width, int height);

/* Set the clear color used at the start of each frame. */
void mop_viewport_set_clear_color(MopViewport *viewport, MopColor color);

/* Set solid or wireframe rendering. */
void mop_viewport_set_render_mode(MopViewport *viewport, MopRenderMode mode);

/* Set the directional light direction (world-space, will be normalized).
 * Default: (0.3, 1.0, 0.5). */
void mop_viewport_set_light_dir(MopViewport *viewport, MopVec3 dir);

/* Set the ambient lighting factor [0, 1].  Default: 0.2. */
void mop_viewport_set_ambient(MopViewport *viewport, float ambient);

/* Set the shading mode.  Default: MOP_SHADING_FLAT.
 * MOP_SHADING_SMOOTH interpolates per-vertex normals (Gouraud). */
void mop_viewport_set_shading(MopViewport *viewport, MopShadingMode mode);

/* -------------------------------------------------------------------------
 * Camera
 *
 * The viewport maintains a single camera.  The projection is a symmetric
 * perspective frustum derived from the parameters below.
 * ------------------------------------------------------------------------- */

void mop_viewport_set_camera(MopViewport *viewport,
                             MopVec3 eye, MopVec3 target, MopVec3 up,
                             float fov_degrees,
                             float near_plane, float far_plane);

/* -------------------------------------------------------------------------
 * Rendering
 *
 * mop_viewport_render executes a full frame:
 *   1. Clear framebuffer
 *   2. For each mesh: transform, rasterize / draw
 *   3. Finalize framebuffer
 *
 * After rendering, the color buffer is available via mop_viewport_read_color.
 * ------------------------------------------------------------------------- */

void mop_viewport_render(MopViewport *viewport);

/* -------------------------------------------------------------------------
 * Framebuffer readback
 *
 * Returns a pointer to the RGBA8 color buffer.  The pointer is valid until
 * the next call to mop_viewport_render, mop_viewport_resize, or
 * mop_viewport_destroy.
 *
 * out_width / out_height receive the framebuffer dimensions.
 * Returns NULL if readback is not available.
 * ------------------------------------------------------------------------- */

const uint8_t *mop_viewport_read_color(MopViewport *viewport,
                                       int *out_width, int *out_height);

/* Return the active backend type. */
MopBackendType mop_viewport_get_backend(MopViewport *viewport);

/* Return the current camera eye position (world-space). */
MopVec3 mop_viewport_get_camera_eye(const MopViewport *viewport);

/* Return the current camera target position (world-space). */
MopVec3 mop_viewport_get_camera_target(const MopViewport *viewport);

/* -------------------------------------------------------------------------
 * Time control
 *
 * Set the current simulation time for deterministic updates of water
 * surfaces and particle emitters.
 * ------------------------------------------------------------------------- */

void mop_viewport_set_time(MopViewport *viewport, float t);

#endif /* MOP_VIEWPORT_H */
