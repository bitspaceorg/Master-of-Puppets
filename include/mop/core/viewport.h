/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * viewport.h — Viewport creation, rendering, and management
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_VIEWPORT_H
#define MOP_CORE_VIEWPORT_H

#include <mop/render/backend.h>
#include <mop/types.h>

/* -------------------------------------------------------------------------
 * Render result — returned by mop_viewport_render
 * ------------------------------------------------------------------------- */

typedef enum MopRenderResult {
  MOP_RENDER_OK = 0,
  MOP_RENDER_ERROR = 1,
  MOP_RENDER_DEVICE_LOST = 2,
} MopRenderResult;

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
  int width;
  int height;
  MopBackendType backend;
  bool reverse_z; /* Use reversed-Z depth buffer for improved precision */
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

void mop_viewport_set_camera(MopViewport *viewport, MopVec3 eye, MopVec3 target,
                             MopVec3 up, float fov_degrees, float near_plane,
                             float far_plane);

/* Set camera without reconstructing orbit parameters.  Used by the orbit
 * camera to avoid asinf() clamping pitch to ±90°. */
void mop_viewport_set_camera_orbit(MopViewport *viewport, MopVec3 eye,
                                   MopVec3 target, MopVec3 up,
                                   float fov_degrees, float near_plane,
                                   float far_plane);

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

MopRenderResult mop_viewport_render(MopViewport *viewport);

/* Return the last error message from mop_viewport_render, or NULL. */
const char *mop_viewport_get_last_error(const MopViewport *viewport);

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

const uint8_t *mop_viewport_read_color(MopViewport *viewport, int *out_width,
                                       int *out_height);

/* Return the active backend type. */
MopBackendType mop_viewport_get_backend(MopViewport *viewport);

/* Return the GPU frame time in milliseconds for the last completed frame.
 * Returns 0.0f on the CPU backend or when timing queries are unavailable. */
float mop_viewport_gpu_frame_time_ms(MopViewport *viewport);

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

/* -------------------------------------------------------------------------
 * Chrome visibility
 *
 * Controls whether editor chrome (grid, axis indicator, background
 * gradient, gizmo) is drawn.  Enabled by default.
 * Disable for clean game/presentation rendering.
 * ------------------------------------------------------------------------- */

void mop_viewport_set_chrome(MopViewport *viewport, bool visible);

#endif /* MOP_CORE_VIEWPORT_H */
