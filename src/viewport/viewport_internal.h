/*
 * Master of Puppets — Viewport Internals
 * viewport_internal.h — Private viewport and mesh structures
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_VIEWPORT_INTERNAL_H
#define MOP_VIEWPORT_INTERNAL_H

#include <mop/mop.h>
#include <mop/light.h>
#include <mop/overlay.h>
#include <mop/display.h>
#include "rhi/rhi.h"
#include "rasterizer/rasterizer.h"

/* -------------------------------------------------------------------------
 * Opaque texture wrapper — maps public MopTexture to RHI texture
 * ------------------------------------------------------------------------- */

struct MopTexture {
    MopRhiTexture *rhi_texture;
};

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
    float         opacity;
    bool          active;

    /* Per-mesh TRS — MOP auto-computes the model matrix from these */
    MopVec3 position;
    MopVec3 rotation;
    MopVec3 scale_val;
    bool    use_trs;       /* true = auto-compute from TRS each frame */

    /* Hierarchical transforms (Phase 4A) */
    int32_t parent_index;  /* index into viewport->meshes, -1 = no parent */
    MopMat4 world_transform;

    /* Texture (Phase 2C) */
    MopTexture *texture;

    /* Normal mapping tangents (Phase 2E) — parallel to vertex buffer */
    MopVec3  *tangents;
    uint32_t  tangent_count;

    /* Material (Phase 2D) */
    MopMaterial material;
    bool        has_material;

    /* Blend mode (Phase 6A) */
    MopBlendMode blend_mode;

    /* Buffer capacities for in-place updates (element counts, not bytes) */
    uint32_t vertex_capacity;
    uint32_t index_capacity;

    /* Flexible vertex format — NULL = standard MopVertex layout */
    MopVertexFormat *vertex_format;
};

/* -------------------------------------------------------------------------
 * Instanced mesh representation (Phase 6B)
 * ------------------------------------------------------------------------- */

#define MOP_INITIAL_INSTANCED_CAPACITY 16

struct MopInstancedMesh {
    MopRhiBuffer *vertex_buffer;
    MopRhiBuffer *index_buffer;
    uint32_t      vertex_count;
    uint32_t      index_count;
    uint32_t      object_id;
    MopColor      base_color;
    float         opacity;
    MopBlendMode  blend_mode;
    bool          active;

    /* Per-instance transforms (heap-allocated array) */
    MopMat4      *transforms;
    uint32_t      instance_count;

    /* Texture (optional) */
    MopTexture   *texture;

    /* Material (optional) */
    MopMaterial   material;
    bool          has_material;
};

/* -------------------------------------------------------------------------
 * Water surface representation (Phase 8D/8E)
 *
 * Defined here so that both water.c and viewport.c can access the struct
 * (water.c creates/updates, viewport.c destroys during cleanup).
 * ------------------------------------------------------------------------- */

struct MopWaterSurface {
    /* Owning viewport */
    MopViewport *viewport;

    /* Grid parameters */
    float extent;
    int   resolution;

    /* Wave parameters */
    float wave_speed;
    float wave_amplitude;
    float wave_frequency;

    /* Appearance */
    MopColor color;
    float    opacity;

    /* Current simulation time */
    float time;

    /* Dynamic vertex/index data */
    MopVertex *vertices;
    uint32_t  *indices;
    uint32_t   vertex_count;
    uint32_t   index_count;

    /* RHI buffers */
    MopRhiBuffer *vertex_buffer;
    MopRhiBuffer *index_buffer;

    /* Mesh registered in the viewport for rendering */
    MopMesh *mesh;
};

/* -------------------------------------------------------------------------
 * Undo ring buffer (Phase 4B)
 * ------------------------------------------------------------------------- */

#define MOP_UNDO_CAPACITY 256

typedef struct MopUndoEntry {
    uint32_t mesh_index;
    MopVec3  pos;
    MopVec3  rot;
    MopVec3  scale;
} MopUndoEntry;

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

/* -------------------------------------------------------------------------
 * Interaction state machine
 * ------------------------------------------------------------------------- */

typedef enum MopInteractState {
    MOP_INTERACT_IDLE,
    MOP_INTERACT_CLICK_PENDING,
    MOP_INTERACT_ORBITING,
    MOP_INTERACT_PANNING,
    MOP_INTERACT_GIZMO_DRAG
} MopInteractState;

#define MOP_MAX_EVENTS 64

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
    MopVec3       light_dir;       /* legacy — kept for backward compat, syncs with lights[0] */
    float         ambient;         /* legacy — kept for backward compat, syncs with lights[0] */
    MopShadingMode shading_mode;

    /* Multi-light system */
    MopLight lights[MOP_MAX_LIGHTS];
    uint32_t light_count;          /* high-water mark for iteration */

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

    /* Scene — dynamic mesh array (Phase 5B) */
    struct MopMesh *meshes;
    uint32_t        mesh_capacity;
    uint32_t        mesh_count;

    /* Instanced meshes (Phase 6B) */
    struct MopInstancedMesh *instanced_meshes;
    uint32_t                 instanced_capacity;
    uint32_t                 instanced_count;

    /* Owned subsystems */
    MopGizmo        *gizmo;
    MopOrbitCamera   camera;
    MopMesh         *grid;

    /* Gradient background (clip-space quad) */
    MopRhiBuffer *bg_vb;
    MopRhiBuffer *bg_ib;

    /* Axis indicator (corner widget) — one pair per axis (X, Y, Z) */
    MopRhiBuffer *axis_ind_vb[3];
    MopRhiBuffer *axis_ind_ib[3];
    uint32_t      axis_ind_vcnt[3];
    uint32_t      axis_ind_icnt[3];

    /* Selection */
    uint32_t selected_id;

    /* Interaction state */
    MopInteractState interact_state;
    MopGizmoAxis     drag_axis;
    float            click_start_x, click_start_y;

    /* Event queue (ring buffer) */
    MopEvent events[MOP_MAX_EVENTS];
    int      event_head, event_tail;

    /* Profiling (Phase 5C) */
    MopFrameStats last_stats;

    /* Undo/redo (Phase 4B) */
    MopUndoEntry undo_entries[MOP_UNDO_CAPACITY];
    int          undo_head;
    int          undo_count;
    int          redo_count;

    /* Particle emitters (Phase 8B/8E) */
    MopParticleEmitter **emitters;
    uint32_t             emitter_count;
    uint32_t             emitter_capacity;

    /* Water surfaces (Phase 8D/8E) */
    MopWaterSurface **water_surfaces;
    uint32_t          water_count;
    uint32_t          water_capacity;

    /* Time tracking for simulation */
    float last_frame_time;

    /* Post-processing (Phase 6C) */
    uint32_t     post_effects;
    MopFogParams fog_params;

    /* Overlay system */
    MopOverlayEntry overlays[MOP_MAX_OVERLAYS];
    uint32_t        overlay_count;
    bool            overlay_enabled[MOP_MAX_OVERLAYS];

    /* Display settings */
    MopDisplaySettings display;
};

/* -------------------------------------------------------------------------
 * Internal water/postprocess functions — called from viewport render loop
 * ------------------------------------------------------------------------- */

/* Update water surface vertex animation (defined in water/water.c) */
void mop_water_update(MopWaterSurface *ws, float t);

/* Free water surface internal data without removing the viewport mesh
 * (for use during viewport destroy when meshes are freed separately) */
void mop_water_destroy_internal(MopWaterSurface *ws);

/* Apply post-processing effects to the framebuffer (defined in postprocess/postprocess.c) */
void mop_postprocess_apply(MopSwFramebuffer *fb, uint32_t effects,
                            const MopFogParams *fog);

#endif /* MOP_VIEWPORT_INTERNAL_H */
