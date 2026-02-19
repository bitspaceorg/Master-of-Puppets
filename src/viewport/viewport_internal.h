/*
 * Master of Puppets — Viewport Internals
 * viewport_internal.h — Private viewport and mesh structures
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_VIEWPORT_INTERNAL_H
#define MOP_VIEWPORT_INTERNAL_H

#include <mop/mop.h>
#include "rhi/rhi.h"

/* -------------------------------------------------------------------------
 * Maximum meshes per viewport
 *
 * The scene uses a flat array with a fixed upper bound.  This avoids
 * dynamic reallocation complexity.  The limit can be raised by changing
 * this constant and recompiling.
 * ------------------------------------------------------------------------- */

#define MOP_MAX_MESHES 4096

/* -------------------------------------------------------------------------
 * Internal mesh representation
 * ------------------------------------------------------------------------- */

struct MopMesh {
    MopRhiBuffer *vertex_buffer;
    MopRhiBuffer *index_buffer;
    uint32_t      vertex_count;
    uint32_t      index_count;
    uint32_t      object_id;
    MopMat4       transform;
    MopColor      base_color;
    bool          active;
};

/* -------------------------------------------------------------------------
 * Viewport structure
 *
 * The viewport owns:
 *   - One RHI device
 *   - One RHI framebuffer
 *   - The mesh array and all RHI buffers within it
 *   - Camera and rendering state
 *
 * The application owns:
 *   - The MopViewportDesc passed to create (may be stack-allocated)
 *   - Vertex / index data passed to mop_viewport_add_mesh (copied)
 * ------------------------------------------------------------------------- */

struct MopViewport {
    /* Backend */
    const MopRhiBackend *rhi;
    MopRhiDevice        *device;
    MopRhiFramebuffer   *framebuffer;
    MopBackendType       backend_type;

    /* Framebuffer dimensions */
    int width;
    int height;

    /* Rendering state */
    MopColor      clear_color;
    MopRenderMode render_mode;

    /* Camera */
    MopVec3 cam_eye;
    MopVec3 cam_target;
    MopVec3 cam_up;
    float   cam_fov_radians;
    float   cam_near;
    float   cam_far;

    /* Computed camera matrices */
    MopMat4 view_matrix;
    MopMat4 projection_matrix;

    /* Scene */
    struct MopMesh meshes[MOP_MAX_MESHES];
    uint32_t       mesh_count;
};

#endif /* MOP_VIEWPORT_INTERNAL_H */
