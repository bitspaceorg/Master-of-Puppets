/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * viewport.h — Viewport creation, rendering, and management
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_VIEWPORT_H
#define MOP_CORE_VIEWPORT_H

#include <mop/interact/camera.h>
#include <mop/render/backend.h>
#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

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
typedef struct MopTexture MopTexture;

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

  /* Supersampling factor for the internal framebuffer.  The internal
   * framebuffer has dimensions (width * ssaa_factor, height * ssaa_factor)
   * and is downsampled on `mop_viewport_read_color`.  Higher values
   * produce smoother edges at the cost of fill rate and memory.
   *
   * Values:  0 = default (2x), 1 = no supersampling (1:1),
   *          2 = 2x, 4 = 4x.
   *
   * For DCC / game-engine integration where the host owns the output
   * texture and wants 1:1 pixel correspondence, set this to 1. */
  int ssaa_factor;

  /* Optional host-owned render target. When non-NULL, the viewport's
   * color framebuffer wraps this texture directly — the rasterizer
   * writes into the host's pixel buffer (zero-copy on CPU backend).
   * Requires ssaa_factor = 1 (the host target determines the render
   * resolution). Supported by the CPU backend today; Vulkan and GL
   * return NULL from create and fall back to internal framebuffer.
   * Texture dimensions must match desc->width × desc->height. */
  MopTexture *render_target;
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

/* Set camera projection mode (perspective or orthographic).
 * Ortho size is the half-height in world units; scroll adjusts it. */
void mop_viewport_set_camera_mode(MopViewport *viewport, MopCameraMode mode);
MopCameraMode mop_viewport_get_camera_mode(const MopViewport *viewport);

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

/* Render and synchronously wait for the GPU to finish, populating the
 * readback buffers (color, depth, object-id) before returning.
 *
 * Use this for screenshot tooling and CI golden-image flows where you need
 * a single render → read sequence to be deterministic. The standard
 * mop_viewport_render returns as soon as commands are submitted; on Vulkan,
 * readback is normally deferred to the next frame's fence wait, which
 * means the first call returns an empty buffer.
 *
 * Backends without deferred readback (CPU, OpenGL) treat this identically
 * to mop_viewport_render — there is no extra cost. */
MopRenderResult mop_viewport_render_sync(MopViewport *viewport);

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
 * Set the current simulation time for deterministic animation updates.
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

/* -------------------------------------------------------------------------
 * Thread-safe scene mutation
 *
 * mop_viewport_render acquires an internal scene mutex for the duration
 * of a frame. Hosts mutating scene state (add/remove meshes, set transforms,
 * update geometry, swap materials) from a thread other than the one that
 * drives rendering MUST bracket their mutations with scene_lock / _unlock
 * to stay serialized with the render.
 *
 * Single-threaded hosts can ignore these calls.
 *
 * The mutex is non-recursive: don't call render from inside a scene_lock,
 * don't lock twice from the same thread without unlocking.
 * ------------------------------------------------------------------------- */

void mop_viewport_scene_lock(MopViewport *viewport);
void mop_viewport_scene_unlock(MopViewport *viewport);

/* -------------------------------------------------------------------------
 * Present to host-owned texture (RTT / render-to-texture)
 *
 * Copies the rendered LDR color buffer into a host-owned MopTexture.
 * The texture must be RGBA8. Valid target dimensions:
 *   - Presentation size (width × height) — RTT downsamples from the
 *     internal SSAA framebuffer, giving the host anti-aliased pixels.
 *   - Internal size (width*ssaa_factor × height*ssaa_factor) — raw
 *     1:1 copy, host does its own downsample.
 * Any other size currently returns false (CPU backend requires an
 * integer-factor relationship; Vulkan path is lenient but behavior is
 * backend-specific for non-integer ratios).
 *
 * Call AFTER mop_viewport_render completes.
 *
 * Returns true on success; false on size mismatch, missing backend
 * support, or NULL arguments.
 *
 * Use case: DCC hosts (Blender, Houdini) and game engines that want to
 * composite MOP's output into their own render pipeline without going
 * through mop_viewport_read_color's CPU readback path.
 * ------------------------------------------------------------------------- */

bool mop_viewport_present_to_texture(MopViewport *viewport, MopTexture *target);

/* Fused render + present — renders a frame and blits the result into a
 * host-owned texture in one call. Common DCC/game-engine embed pattern.
 * Returns the render result; a blit failure does not demote the render
 * result (callers can check the target themselves). */
MopRenderResult mop_viewport_render_to(MopViewport *viewport,
                                       MopTexture *target);

/* Enable/disable GPU-driven rendering on GPU backends.
 *
 * When on, the backend populates a per-frame indirect-draw buffer and
 * runs GPU culling against it; the main render path is expected to emit
 * a single vkCmdDrawIndexedIndirectCount per pipeline bucket instead of
 * per-mesh draws. Significant performance win at high mesh counts.
 *
 * SCAFFOLDING: today the flag only controls warm-up tracking inside the
 * Vulkan backend — the render path still issues per-mesh draws. Real
 * activation requires uber-shader + pipeline bucketing; see docs/TODO.md.
 * Toggling this today is safe but a no-op. */
void mop_viewport_set_gpu_driven_rendering(MopViewport *viewport, bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_VIEWPORT_H */
