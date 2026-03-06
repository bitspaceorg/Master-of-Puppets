/*
 * Master of Puppets — Viewport Internals
 * viewport_internal.h — Private viewport and mesh structures
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_VIEWPORT_INTERNAL_H
#define MOP_VIEWPORT_INTERNAL_H

#include "core/subsystem.h"
#include "rasterizer/rasterizer.h"
#include "rhi/rhi.h"
#include <mop/core/camera_object.h>
#include <mop/core/display.h>
#include <mop/core/environment.h>
#include <mop/core/light.h>
#include <mop/core/overlay.h>
#include <mop/core/pipeline.h>
#include <mop/core/theme.h>
#include <mop/interact/selection.h>
#include <mop/mop.h>

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
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t object_id;
  MopMat4 transform;
  MopColor base_color;
  float opacity;
  bool active;

  /* Per-mesh TRS — MOP auto-computes the model matrix from these */
  MopVec3 position;
  MopVec3 rotation;
  MopVec3 scale_val;
  bool use_trs; /* true = auto-compute from TRS each frame */

  /* Hierarchical transforms (Phase 4A) */
  int32_t parent_index; /* index into viewport->meshes, -1 = no parent */
  MopMat4 world_transform;

  /* Texture (Phase 2C) */
  MopTexture *texture;

  /* Normal mapping tangents (Phase 2E) — parallel to vertex buffer */
  MopVec3 *tangents;
  uint32_t tangent_count;

  /* Material (Phase 2D) */
  MopMaterial material;
  bool has_material;

  /* Blend mode (Phase 6A) */
  MopBlendMode blend_mode;

  /* Buffer capacities for in-place updates (element counts, not bytes) */
  uint32_t vertex_capacity;
  uint32_t index_capacity;

  /* Flexible vertex format — NULL = standard MopVertex layout */
  MopVertexFormat *vertex_format;

  /* Edit mode (Phase 3) */
  MopEditMode edit_mode;

  /* Per-mesh shading mode override (-1 = use viewport default) */
  int shading_mode_override;
};

/* -------------------------------------------------------------------------
 * Instanced mesh representation (Phase 6B)
 * ------------------------------------------------------------------------- */

#define MOP_INITIAL_INSTANCED_CAPACITY 16

struct MopInstancedMesh {
  MopRhiBuffer *vertex_buffer;
  MopRhiBuffer *index_buffer;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t object_id;
  MopColor base_color;
  float opacity;
  MopBlendMode blend_mode;
  bool active;

  /* Per-instance transforms (heap-allocated array) */
  MopMat4 *transforms;
  uint32_t instance_count;

  /* Texture (optional) */
  MopTexture *texture;

  /* Material (optional) */
  MopMaterial material;
  bool has_material;
};

/* -------------------------------------------------------------------------
 * Camera object (Phase 5)
 * ------------------------------------------------------------------------- */

#define MOP_MAX_CAMERAS 16

struct MopCameraObject {
  MopVec3 position;
  MopVec3 target;
  MopVec3 up;
  float fov_degrees;
  float near_plane;
  float far_plane;
  float aspect_ratio;
  uint32_t object_id;
  char name[64];
  bool active;
  bool frustum_visible;

  /* Frustum wireframe mesh — regenerated when camera params change */
  MopMesh *frustum_mesh;

  /* Camera icon mesh (small box at camera position) */
  MopMesh *icon_mesh;
};

/* -------------------------------------------------------------------------
 * Water surface representation (Phase 8D/8E)
 *
 * Defined here so that both water.c and viewport.c can access the struct
 * (water.c creates/updates, viewport.c destroys during cleanup).
 * ------------------------------------------------------------------------- */

struct MopWaterSurface {
  MopSubsystem base; /* must be first — enables (MopSubsystem*)ws cast */

  /* Owning viewport */
  MopViewport *viewport;

  /* Grid parameters */
  float extent;
  int resolution;

  /* Wave parameters */
  float wave_speed;
  float wave_amplitude;
  float wave_frequency;

  /* Appearance */
  MopColor color;
  float opacity;

  /* Current simulation time */
  float time;

  /* Dynamic vertex/index data */
  MopVertex *vertices;
  uint32_t *indices;
  uint32_t vertex_count;
  uint32_t index_count;

  /* RHI buffers */
  MopRhiBuffer *vertex_buffer;
  MopRhiBuffer *index_buffer;

  /* Mesh registered in the viewport for rendering */
  MopMesh *mesh;
};

/* -------------------------------------------------------------------------
 * Overlay command buffer — SDF primitive types for GPU overlay pass
 * ------------------------------------------------------------------------- */

typedef enum MopOverlayPrimType {
  MOP_PRIM_LINE = 0,
  MOP_PRIM_FILLED_CIRCLE = 1,
  MOP_PRIM_DIAMOND = 2,
} MopOverlayPrimType;

typedef struct MopOverlayPrim {
  float x0, y0, x1, y1; /* line: endpoints, circle/diamond: center+unused */
  float r, g, b, a;     /* color + opacity */
  float width;          /* line width or ring width */
  float radius;         /* circle/diamond radius */
  int32_t type;         /* MopOverlayPrimType */
  float depth;          /* NDC depth for depth-tested overlays (-1=no test) */
} MopOverlayPrim;

#define MOP_MAX_OVERLAY_PRIMS 2048

/* Grid parameters for GPU shader grid rendering */
typedef struct MopGridParams {
  float Hi[9];               /* Inverse homography: NDC → world XZ on Y=0 */
  float vp_z0, vp_z2, vp_z3; /* VP matrix rows for depth at (wx,0,wz) */
  float vp_w0, vp_w2, vp_w3;
  float grid_half; /* Half extent in world units */
  bool reverse_z;
  MopColor minor_color, major_color, axis_x_color, axis_z_color;
  float axis_half_width; /* Half-width in pixels for axis lines */
} MopGridParams;

/* -------------------------------------------------------------------------
 * Undo ring buffer (Phase 4B)
 * ------------------------------------------------------------------------- */

#define MOP_UNDO_CAPACITY 256

typedef struct MopUndoEntry {
  uint32_t mesh_index;
  MopVec3 pos;
  MopVec3 rot;
  MopVec3 scale;
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
  MopRhiDevice *device;
  MopRhiFramebuffer *framebuffer;
  MopBackendType backend_type;

  /* Framebuffer dimensions */
  int width;
  int height;

  /* Rendering state */
  MopColor clear_color;
  MopRenderMode render_mode;
  MopVec3
      light_dir; /* legacy — kept for backward compat, syncs with lights[0] */
  float ambient; /* legacy — kept for backward compat, syncs with lights[0] */
  MopShadingMode shading_mode;

  /* Multi-light system */
  MopLight lights[MOP_MAX_LIGHTS];
  uint32_t light_count;      /* high-water mark for iteration */
  bool default_light_active; /* true until user calls add_light */

  /* Camera */
  MopVec3 cam_eye;
  MopVec3 cam_target;
  MopVec3 cam_up;
  float cam_fov_radians;
  float cam_near;
  float cam_far;

  /* Computed camera matrices */
  MopMat4 view_matrix;
  MopMat4 projection_matrix;

  /* Scene — dynamic mesh array (Phase 5B) */
  struct MopMesh *meshes;
  uint32_t mesh_capacity;
  uint32_t mesh_count;

  /* Instanced meshes (Phase 6B) */
  struct MopInstancedMesh *instanced_meshes;
  uint32_t instanced_capacity;
  uint32_t instanced_count;

  /* Owned subsystems */
  MopGizmo *gizmo;
  MopOrbitCamera camera;
  MopMesh *grid;

  /* Camera objects (Phase 5) */
  struct MopCameraObject cameras[MOP_MAX_CAMERAS];
  uint32_t camera_count;
  struct MopCameraObject *active_camera; /* NULL = use orbit camera */

  /* Gradient background (clip-space quad) */
  MopRhiBuffer *bg_vb;
  MopRhiBuffer *bg_ib;

  /* Axis indicator (corner widget) — one pair per axis (X, Y, Z) */
  MopRhiBuffer *axis_ind_vb[3];
  MopRhiBuffer *axis_ind_ib[3];
  uint32_t axis_ind_vcnt[3];
  uint32_t axis_ind_icnt[3];

  /* Selection */
  uint32_t selected_id;

  /* Sub-element selection (Phase 3) */
  MopSelection selection;

  /* Interaction state */
  MopInteractState interact_state;
  MopGizmoAxis drag_axis;
  float click_start_x, click_start_y;

  /* Event queue (ring buffer) */
  MopEvent events[MOP_MAX_EVENTS];
  int event_head, event_tail;

  /* Profiling (Phase 5C) */
  MopFrameStats last_stats;

  /* Undo/redo (Phase 4B) */
  MopUndoEntry undo_entries[MOP_UNDO_CAPACITY];
  int undo_head;
  int undo_count;
  int redo_count;

  /* Particle emitters (Phase 8B/8E) */
  MopParticleEmitter **emitters;
  uint32_t emitter_count;
  uint32_t emitter_capacity;

  /* Water surfaces (Phase 8D/8E) */
  MopWaterSurface **water_surfaces;
  uint32_t water_count;
  uint32_t water_capacity;

  /* Time tracking for simulation */
  float last_frame_time;
  float prev_frame_time; /* previous last_frame_time, for computing dt */

  /* Post-processing (Phase 6C) */
  uint32_t post_effects;
  MopFogParams fog_params;

  /* Overlay system */
  MopOverlayEntry overlays[MOP_MAX_OVERLAYS];
  uint32_t overlay_count;
  bool overlay_enabled[MOP_MAX_OVERLAYS];

  /* Display settings */
  MopDisplaySettings display;

  /* Theme (design language) */
  MopTheme theme;

/* Pipeline hooks (Phase D) */
#define MOP_MAX_HOOKS 56 /* 7 stages * 8 per stage */
  struct {
    MopPipelineHookFn fn;
    void *data;
    MopPipelineStage stage;
    bool active;
  } hooks[MOP_MAX_HOOKS];
  uint32_t hook_count;

  /* Frame callback */
  MopFrameCallbackFn frame_cb;
  void *frame_cb_data;

  /* Light indicators (visual representations of lights) */
  MopMesh *light_indicators[MOP_MAX_LIGHTS];

  /* SSAA (Supersampling Anti-Aliasing) — backend-agnostic smooth rendering */
  int ssaa_factor;         /* 1 = off, 2 = 2x SSAA (default) */
  uint8_t *ssaa_color_buf; /* downsampled color buffer for readback */

  /* Shadow map for directional light (CPU backend only) */
  MopSwFramebuffer shadow_fb;
  bool shadow_fb_valid; /* true after shadow pass rendered this frame */

  /* Chrome visibility (grid, axis indicator, background, gizmo) */
  bool show_chrome; /* true by default */

  /* HDR exposure multiplier (default 1.0) */
  float exposure;

  /* Environment map (HDRI / procedural sky) */
  MopEnvironmentType env_type;
  MopRhiTexture *env_texture; /* GPU-side HDR texture */
  float *env_hdr_data;        /* raw float RGBA data for CPU sampling */
  int env_width, env_height;
  float env_rotation;            /* Y-axis rotation in radians */
  float env_intensity;           /* brightness multiplier (default 1.0) */
  bool show_env_background;      /* show HDRI as skybox bg (default false) */
  MopRhiTexture *env_irradiance; /* precomputed diffuse irradiance map */
  float *env_irradiance_data;    /* raw float RGBA irradiance for CPU */
  int env_irradiance_w, env_irradiance_h;
  MopRhiTexture *env_prefiltered; /* prefiltered specular map */
  float *env_prefiltered_data;    /* raw float RGBA prefiltered for CPU */
  int env_prefiltered_w, env_prefiltered_h;
  int env_prefiltered_levels;    /* number of roughness mip levels */
  MopRhiTexture *env_brdf_lut;   /* split-sum BRDF LUT texture */
  float *env_brdf_lut_data;      /* raw float RG BRDF LUT for CPU */
  MopProceduralSkyDesc sky_desc; /* procedural sky parameters */

  /* Reversed-Z depth buffer — improves depth precision for large scenes */
  bool reverse_z;

  /* GPU overlay command buffer (SDF primitives) */
  MopOverlayPrim *overlay_prims;
  uint32_t overlay_prim_count;

  /* Subsystem registry — generic dispatch for water, particles, postprocess,
   * etc. */
  MopSubsystemRegistry subsystems;
};

/* -------------------------------------------------------------------------
 * Internal subsystem functions
 * ------------------------------------------------------------------------- */

/* Register the postprocess subsystem (called from viewport_create) */
void mop_postprocess_register(MopViewport *vp);

/* Apply post-processing effects to the framebuffer (also called via vtable) */
void mop_postprocess_apply(MopSwFramebuffer *fb, uint32_t effects,
                           const MopFogParams *fog);

/* Light indicator management — create/destroy/update visual indicators */
void mop_light_update_indicators(MopViewport *vp);
void mop_light_destroy_indicators(MopViewport *vp);

/* Axis indicator pick — returns MopViewAxis+1, or 0 for miss */
int mop_viewport_pick_axis_indicator(MopViewport *vp, float mx, float my);

/* Gizmo internal accessors — used by 2D overlay renderer */
bool mop_gizmo_is_visible(const MopGizmo *gizmo);
MopVec3 mop_gizmo_get_position_internal(const MopGizmo *gizmo);
MopVec3 mop_gizmo_get_rotation_internal(const MopGizmo *gizmo);
MopGizmoAxis mop_gizmo_get_hover_axis(const MopGizmo *gizmo);
MopVec3 mop_gizmo_get_axis_dir(const MopGizmo *gizmo, int axis);
uint32_t mop_gizmo_get_handle_id(const MopGizmo *gizmo, int axis);
void mop_gizmo_set_handles_opacity(MopGizmo *gizmo, float opacity);

/* -------------------------------------------------------------------------
 * Overlay command buffer push helpers
 * ------------------------------------------------------------------------- */

void mop_overlay_push_line(MopViewport *vp, float x0, float y0, float x1,
                           float y1, float r, float g, float b, float width,
                           float opacity, float depth);
void mop_overlay_push_circle(MopViewport *vp, float cx, float cy, float radius,
                             float r, float g, float b, float opacity,
                             float depth);
void mop_overlay_push_diamond(MopViewport *vp, float cx, float cy, float size,
                              float r, float g, float b, float width,
                              float opacity, float depth);

#endif /* MOP_VIEWPORT_INTERNAL_H */
