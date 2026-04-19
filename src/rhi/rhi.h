/*
 * Master of Puppets — Render Hardware Interface
 * rhi.h — Backend function table and abstract types
 *
 * This header is INTERNAL.  It must never appear in include/mop/.
 * The viewport core consumes RHI.  Backends implement RHI.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_RHI_H
#define MOP_RHI_H

#include <mop/core/light.h>
#include <mop/core/vertex_format.h>
#include <mop/render/backend.h>
#include <mop/types.h>

/* -------------------------------------------------------------------------
 * Opaque RHI handles — each backend defines the concrete structs
 * ------------------------------------------------------------------------- */

typedef struct MopRhiDevice MopRhiDevice;
typedef struct MopRhiBuffer MopRhiBuffer;
typedef struct MopRhiFramebuffer MopRhiFramebuffer;
typedef struct MopRhiTexture MopRhiTexture;
typedef struct MopRhiShader MopRhiShader;

/* -------------------------------------------------------------------------
 * Buffer descriptor
 * ------------------------------------------------------------------------- */

typedef struct MopRhiBufferDesc {
  const void *data;
  size_t size;
} MopRhiBufferDesc;

/* -------------------------------------------------------------------------
 * Framebuffer descriptor
 * ------------------------------------------------------------------------- */

typedef struct MopRhiFramebufferDesc {
  int width;
  int height;
} MopRhiFramebufferDesc;

/* -------------------------------------------------------------------------
 * Draw call — everything the backend needs to rasterize one mesh
 * ------------------------------------------------------------------------- */

typedef struct MopRhiDrawCall {
  MopRhiBuffer *vertex_buffer;
  MopRhiBuffer *index_buffer;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t object_id;
  MopMat4 model;
  MopMat4 view;
  MopMat4 projection;
  MopMat4 mvp;
  MopColor base_color;
  float opacity;
  MopVec3 light_dir;
  float ambient;
  MopShadingMode shading_mode;
  bool wireframe;
  bool depth_test;
  bool depth_write; /* false = read-only depth test (no Z-buffer update) */
  bool backface_cull;

  /* Texture (Phase 2C) — NULL = no texture */
  MopRhiTexture *texture;

  /* PBR texture maps */
  MopRhiTexture *normal_map;
  MopRhiTexture *metallic_roughness_map;
  MopRhiTexture *ao_map;

  /* Blend mode (Phase 6A) */
  MopBlendMode blend_mode;

  /* Material properties (Phase 2D) */
  float metallic;
  float roughness;
  MopVec3 emissive;

  /* Multi-light system — NULL = single legacy light (light_dir + ambient) */
  const MopLight *lights;
  uint32_t light_count;

  /* Camera eye position (for specular / multi-light world-space calcs) */
  MopVec3 cam_eye;

  /* Flexible vertex format — NULL = standard MopVertex layout */
  const MopVertexFormat *vertex_format;

  /* Line rendering (Phase 1) */
  float line_width; /* wireframe line width in pixels, default 1.0 */
  float depth_bias; /* z offset for coplanar overlay prevention */

  /* Local-space bounding box (Phase 2B) — for GPU frustum culling.
   * Zero min/max = no bounds available (skip culling for this draw). */
  MopVec3 aabb_min;
  MopVec3 aabb_max;
} MopRhiDrawCall;

/* -------------------------------------------------------------------------
 * Backend function table
 *
 * Functions fall into two groups:
 *
 *   Mandatory  — device lifecycle, buffer/framebuffer/texture management,
 *                frame_begin/frame_end/draw, all readbacks, buffer_read.
 *                The viewport invokes these unconditionally; a backend that
 *                leaves any of them NULL will crash at first render.
 *
 *   Optional   — every set_* effect hook (bloom, ssao, ssr, oit, volumetric,
 *                taa, ibl, exposure), decal operations, draw_skybox,
 *                draw_overlays, frame_submit, frame_gpu_time_ms,
 *                texture_create_ex, texture_create_hdr, shader_create,
 *                shader_destroy, framebuffer_copy_to_texture. Every
 *                caller guards these with a NULL check; a backend may
 *                leave them NULL if the feature is unsupported (CPU
 *                backend does this for GPU-only effects).
 * ------------------------------------------------------------------------- */

typedef struct MopRhiBackend {
  const char *name;

  /* Device lifecycle */
  MopRhiDevice *(*device_create)(void);
  void (*device_destroy)(MopRhiDevice *device);

  /* Buffer management */
  MopRhiBuffer *(*buffer_create)(MopRhiDevice *device,
                                 const MopRhiBufferDesc *desc);
  void (*buffer_destroy)(MopRhiDevice *device, MopRhiBuffer *buffer);

  /* Framebuffer management */
  MopRhiFramebuffer *(*framebuffer_create)(MopRhiDevice *device,
                                           const MopRhiFramebufferDesc *desc);
  void (*framebuffer_destroy)(MopRhiDevice *device, MopRhiFramebuffer *fb);
  void (*framebuffer_resize)(MopRhiDevice *device, MopRhiFramebuffer *fb,
                             int width, int height);

  /* Create a framebuffer that wraps a host-owned color texture. Depth
   * and picking attachments remain backend-internal. The host retains
   * ownership of `color` — MOP will not destroy it. Optional — NULL if
   * the backend doesn't support external color targets. */
  MopRhiFramebuffer *(*framebuffer_create_from_texture)(MopRhiDevice *device,
                                                        MopRhiTexture *color,
                                                        int width, int height);

  /* Frame commands */
  void (*frame_begin)(MopRhiDevice *device, MopRhiFramebuffer *fb,
                      MopColor clear_color);
  void (*frame_end)(MopRhiDevice *device, MopRhiFramebuffer *fb);
  void (*frame_submit)(MopRhiDevice *device, MopRhiFramebuffer *fb);
  void (*draw)(MopRhiDevice *device, MopRhiFramebuffer *fb,
               const MopRhiDrawCall *call);

  /* Picking readback */
  uint32_t (*pick_read_id)(MopRhiDevice *device, MopRhiFramebuffer *fb, int x,
                           int y);
  float (*pick_read_depth)(MopRhiDevice *device, MopRhiFramebuffer *fb, int x,
                           int y);

  /* Color buffer readback — returns RGBA8, row-major, top-left origin */
  const uint8_t *(*framebuffer_read_color)(MopRhiDevice *device,
                                           MopRhiFramebuffer *fb,
                                           int *out_width, int *out_height);

  /* Object-ID buffer readback — returns uint32_t per pixel, row-major */
  const uint32_t *(*framebuffer_read_object_id)(MopRhiDevice *device,
                                                MopRhiFramebuffer *fb,
                                                int *out_width,
                                                int *out_height);

  /* Depth buffer readback — returns float per pixel, row-major */
  const float *(*framebuffer_read_depth)(MopRhiDevice *device,
                                         MopRhiFramebuffer *fb, int *out_width,
                                         int *out_height);

  /* Texture management */
  MopRhiTexture *(*texture_create)(MopRhiDevice *device, int width, int height,
                                   const uint8_t *rgba_data);
  MopRhiTexture *(*texture_create_hdr)(MopRhiDevice *device, int width,
                                       int height,
                                       const float *rgba_float_data);

  /* Extended texture creation with format, mip count, and data size.
   * For compressed formats (BC1/3/5/7), data is pre-compressed blocks.
   * format: MopTexFormat enum value.
   * mip_levels: number of mip levels in data (0 = single level).
   * data_size: total byte size of data buffer.
   * Returns NULL if the format is unsupported by the backend. */
  MopRhiTexture *(*texture_create_ex)(MopRhiDevice *device, int width,
                                      int height, int format, int mip_levels,
                                      const uint8_t *data, size_t data_size);

  void (*texture_destroy)(MopRhiDevice *device, MopRhiTexture *texture);

  /* Instanced drawing (Phase 6B) */
  void (*draw_instanced)(MopRhiDevice *device, MopRhiFramebuffer *fb,
                         const MopRhiDrawCall *call,
                         const MopMat4 *instance_transforms,
                         uint32_t instance_count);

  /* Dynamic buffer update (Phase 8A) */
  void (*buffer_update)(MopRhiDevice *device, MopRhiBuffer *buffer,
                        const void *data, size_t offset, size_t size);

  /* Read raw vertex data from a buffer (overlay safety).
   * CPU returns buf->data, Vulkan returns buf->shadow. */
  const void *(*buffer_read)(MopRhiBuffer *buffer);

  /* Return the GPU frame time in milliseconds for the last completed frame.
   * CPU backend returns 0.0f. */
  float (*frame_gpu_time_ms)(MopRhiDevice *dev);

  /* Set HDR exposure for tonemapping.
   * GPU backends store this for the tonemap pass; CPU backend is a no-op
   * (exposure handled by mop_sw_hdr_resolve in the viewport core). */
  void (*set_exposure)(MopRhiDevice *dev, float exposure);

  /* Set bloom parameters for the HDR bloom post-process.
   * GPU backends store threshold/intensity and enable bloom.
   * CPU backend is a no-op. */
  void (*set_bloom)(MopRhiDevice *dev, bool enabled, float threshold,
                    float intensity);

  /* Enable/disable SSAO. GPU backends toggle the SSAO pass. */
  void (*set_ssao)(MopRhiDevice *dev, bool enabled);

  /* Enable/disable SSR. GPU backends toggle the SSR pass.
   * intensity: reflection strength (0..1). */
  void (*set_ssr)(MopRhiDevice *dev, bool enabled, float intensity);

  /* Enable/disable OIT (Order-Independent Transparency).
   * GPU backends use Weighted Blended OIT (McGuire/Bavoil 2013). */
  void (*set_oit)(MopRhiDevice *dev, bool enabled);

  /* Deferred decals: add/remove/clear projected decal volumes.
   * transform: 16 floats (column-major 4x4 decal box transform).
   * Returns decal ID (0..255) or -1 on failure. */
  int32_t (*add_decal)(MopRhiDevice *dev, const float *transform, float opacity,
                       int32_t texture_idx);
  void (*remove_decal)(MopRhiDevice *dev, int32_t decal_id);
  void (*clear_decals)(MopRhiDevice *dev);

  /* Volumetric fog control.  GPU backends store parameters; CPU is a no-op. */
  void (*set_volumetric)(MopRhiDevice *dev, float density, float r, float g,
                         float b, float anisotropy, int steps);

  /* Set TAA parameters for the temporal anti-aliasing resolve pass.
   * inv_vp_jittered: 16 floats, inverse of (jittered projection * view).
   * prev_vp: 16 floats, previous frame's (unjittered) VP matrix.
   * jitter_x/y: sub-pixel jitter in pixels for current frame.
   * first_frame: true when no history is available.
   * GPU backends store for use in frame_end; CPU backend is a no-op. */
  void (*set_taa_params)(MopRhiDevice *dev, const float *inv_vp_jittered,
                         const float *prev_vp, float jitter_x, float jitter_y,
                         bool first_frame);

  /* Set IBL textures for environment-based lighting.
   * GPU backends store image views for descriptor binding.
   * CPU backend is a no-op (IBL handled via mop_sw_ibl_set). */
  void (*set_ibl_textures)(MopRhiDevice *dev, MopRhiTexture *irradiance,
                           MopRhiTexture *prefiltered, MopRhiTexture *brdf_lut);

  /* Draw environment skybox as background (fullscreen).
   * inv_vp: 16 floats (column-major 4x4 inverse view-projection matrix).
   * cam_pos: 3 floats (camera world position).
   * rotation, intensity: env map parameters.
   * GPU backends draw a fullscreen triangle with equirectangular sampling.
   * CPU backend is a no-op (skybox handled in viewport core). */
  void (*draw_skybox)(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                      MopRhiTexture *env_map, const float *inv_vp,
                      const float *cam_pos, float rotation, float intensity);

  /* Draw SDF overlay primitives on top of the LDR color image.
   * prims: array of MopOverlayPrim (from viewport_internal.h).
   * grid_params: if non-NULL, GPU grid is rendered before SDF overlays.
   * GPU backends use a fullscreen SDF shader; CPU iterates and pixel-writes.
   * May be NULL for backends that don't support overlays (caller falls back).
   */
  void (*draw_overlays)(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                        const void *prims, uint32_t prim_count,
                        const void *grid_params, int fb_width, int fb_height);

  /* Shader module management (Phase 0C — runtime shader loading).
   * bytecode: SPIR-V uint32 array for Vulkan, ignored by CPU.
   * Returns opaque handle; NULL on failure or unsupported backend. */
  MopRhiShader *(*shader_create)(MopRhiDevice *dev, const uint32_t *bytecode,
                                 size_t size);
  void (*shader_destroy)(MopRhiDevice *dev, MopRhiShader *shader);

  /* Copy the color attachment of `fb` into `target` (a host-owned RHI
   * texture, RGBA8). `fb` and `target` must have matching dimensions.
   * Returns true on success.
   *
   * Used to implement host-framebuffer / render-to-texture: the host
   * creates a texture, the viewport renders into its own framebuffer,
   * and the backend blits the result into the host's texture for
   * compositing. Optional — NULL if the backend doesn't support it. */
  bool (*framebuffer_copy_to_texture)(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                                      MopRhiTexture *target);

  /* Read RGBA8 pixels from a texture into a caller-provided buffer.
   * `buf_size` must be at least width*height*4. Returns true on success.
   *
   * Cheap on CPU backend (direct memcpy from tex->data).  GPU backends
   * require a readback staging buffer and a blocking submit; may be
   * left NULL if unsupported. Hosts that want GPU-resident output
   * should use framebuffer_copy_to_texture and keep pixels on GPU. */
  bool (*texture_read_rgba8)(MopRhiDevice *dev, MopRhiTexture *texture,
                             uint8_t *out_buf, size_t buf_size);

  /* Request GPU-driven rendering (indirect draw + per-object SSBO
   * feeding). Vulkan: enables the vkCmdDrawIndexedIndirectCount path.
   * Other backends: NULL / no-op. Today this is scaffolding — the main
   * render path still issues per-mesh draws; consuming the indirect
   * buffer requires pipeline bucketing work (see docs/TODO.md). */
  void (*set_gpu_driven_rendering)(MopRhiDevice *dev, bool enabled);
} MopRhiBackend;

/* -------------------------------------------------------------------------
 * Backend resolution
 *
 * Returns the function table for the requested backend type.
 * MOP_BACKEND_AUTO resolves to the platform default.
 * Returns NULL if the backend is not compiled in or not available.
 * ------------------------------------------------------------------------- */

const MopRhiBackend *mop_rhi_get_backend(MopBackendType type);

/* Backend factory functions — defined in each backend translation unit */
const MopRhiBackend *mop_rhi_backend_cpu(void);

#if defined(MOP_HAS_OPENGL)
const MopRhiBackend *mop_rhi_backend_opengl(void);
#endif

#if defined(MOP_HAS_VULKAN)
const MopRhiBackend *mop_rhi_backend_vulkan(void);
#endif

#endif /* MOP_RHI_H */
