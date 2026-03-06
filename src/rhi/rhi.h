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
} MopRhiDrawCall;

/* -------------------------------------------------------------------------
 * Backend function table
 *
 * Every backend must populate every function pointer.  NULL entries are
 * invalid and will cause the viewport to reject the backend at creation.
 * ------------------------------------------------------------------------- */

/* All function pointers in this table MUST be non-NULL.  The viewport
 * core calls them unconditionally.  Backends must provide complete
 * implementations for all functions. */
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

  /* Frame commands */
  void (*frame_begin)(MopRhiDevice *device, MopRhiFramebuffer *fb,
                      MopColor clear_color);
  void (*frame_end)(MopRhiDevice *device, MopRhiFramebuffer *fb);
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
