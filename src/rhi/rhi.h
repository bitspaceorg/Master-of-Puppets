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

#include <mop/backend.h>
#include <mop/light.h>
#include <mop/types.h>
#include <mop/vertex_format.h>

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

  /* Texture management */
  MopRhiTexture *(*texture_create)(MopRhiDevice *device, int width, int height,
                                   const uint8_t *rgba_data);
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
