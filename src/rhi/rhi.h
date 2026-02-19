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

#include <mop/types.h>
#include <mop/backend.h>

/* -------------------------------------------------------------------------
 * Opaque RHI handles — each backend defines the concrete structs
 * ------------------------------------------------------------------------- */

typedef struct MopRhiDevice      MopRhiDevice;
typedef struct MopRhiBuffer      MopRhiBuffer;
typedef struct MopRhiFramebuffer MopRhiFramebuffer;

/* -------------------------------------------------------------------------
 * Buffer descriptor
 * ------------------------------------------------------------------------- */

typedef struct MopRhiBufferDesc {
    const void *data;
    size_t      size;
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
    uint32_t      vertex_count;
    uint32_t      index_count;
    uint32_t      object_id;
    MopMat4       model;
    MopMat4       view;
    MopMat4       projection;
    MopMat4       mvp;
    MopColor      base_color;
    bool          wireframe;
    bool          depth_test;
    bool          backface_cull;
} MopRhiDrawCall;

/* -------------------------------------------------------------------------
 * Backend function table
 *
 * Every backend must populate every function pointer.  NULL entries are
 * invalid and will cause the viewport to reject the backend at creation.
 * ------------------------------------------------------------------------- */

typedef struct MopRhiBackend {
    const char *name;

    /* Device lifecycle */
    MopRhiDevice *(*device_create)(void);
    void          (*device_destroy)(MopRhiDevice *device);

    /* Buffer management */
    MopRhiBuffer *(*buffer_create)(MopRhiDevice *device,
                                   const MopRhiBufferDesc *desc);
    void          (*buffer_destroy)(MopRhiDevice *device,
                                    MopRhiBuffer *buffer);

    /* Framebuffer management */
    MopRhiFramebuffer *(*framebuffer_create)(MopRhiDevice *device,
                                             const MopRhiFramebufferDesc *desc);
    void               (*framebuffer_destroy)(MopRhiDevice *device,
                                              MopRhiFramebuffer *fb);
    void               (*framebuffer_resize)(MopRhiDevice *device,
                                             MopRhiFramebuffer *fb,
                                             int width, int height);

    /* Frame commands */
    void (*frame_begin)(MopRhiDevice *device, MopRhiFramebuffer *fb,
                        MopColor clear_color);
    void (*frame_end)(MopRhiDevice *device, MopRhiFramebuffer *fb);
    void (*draw)(MopRhiDevice *device, MopRhiFramebuffer *fb,
                 const MopRhiDrawCall *call);

    /* Picking readback */
    uint32_t (*pick_read_id)(MopRhiDevice *device, MopRhiFramebuffer *fb,
                             int x, int y);
    float    (*pick_read_depth)(MopRhiDevice *device, MopRhiFramebuffer *fb,
                                int x, int y);

    /* Color buffer readback — returns RGBA8, row-major, top-left origin */
    const uint8_t *(*framebuffer_read_color)(MopRhiDevice *device,
                                             MopRhiFramebuffer *fb,
                                             int *out_width, int *out_height);
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
