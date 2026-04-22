/*
 * Master of Puppets — Viewport Internals
 * viewport_internal.h — Private viewport and mesh structures
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_VIEWPORT_INTERNAL_H
#define MOP_VIEWPORT_INTERNAL_H

#include "rasterizer/rasterizer.h"
#include "rhi/rhi.h"

#include <pthread.h>

#include <mop/core/camera_object.h>
#include <mop/core/display.h>
#include <mop/core/environment.h>
#include <mop/core/light.h>
#include <mop/core/overlay.h>
#include <mop/core/pipeline.h>
#include <mop/core/texture_pipeline.h>
#include <mop/core/theme.h>
#include <mop/interact/selection.h>
#include <mop/mop.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Opaque texture wrapper — maps public MopTexture to RHI texture
 * ------------------------------------------------------------------------- */

struct MopTexture {
  MopRhiTexture *rhi_texture;

  /* Texture pipeline fields */
  uint64_t content_hash; /* FNV-1a hash of pixel data (0 = not computed) */
  MopTexStreamState stream_state; /* streaming state */
  char path[256];           /* source file path (empty = created from data) */
  uint32_t last_used_frame; /* frame counter for cache eviction */
  int width;
  int height;
  int mip_levels;
  bool srgb;
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

  /* Cached local-space AABB for frustum culling */
  MopAABB aabb_local;
  bool aabb_valid;

  /* Skeletal skinning — bind-pose data + bone matrices.
   * When bone_count > 0, the mesh is considered skinned. Each frame,
   * CPU skinning transforms bind_pose_data → vertex_buffer using
   * bone_matrices, joints, and weights from the vertex format. */
  void *bind_pose_data;   /* original vertex data (byte copy) */
  MopMat4 *bone_matrices; /* array of bone_count transforms */
  uint32_t bone_count;
  bool skin_dirty; /* true when matrices changed, needs re-skin */

  /* Bone hierarchy — parent index per bone (-1 = root).
   * Set via mop_mesh_set_bone_hierarchy(). Used by bone overlay. */
  int32_t *bone_parents; /* array of bone_count parent indices */

  /* Morph targets (blend shapes).
   * Each target is a flat float3 array (position deltas per vertex).
   * CPU blending: final_pos = base_pos + sum(weight[i] * target[i][v]) */
  float *morph_targets; /* packed: target_count * vertex_count * 3 */
  float *morph_weights; /* array of target_count weights */
  uint32_t morph_target_count;
  bool morph_dirty; /* true when weights changed, needs re-blend */

  /* LOD chain (Phase 9C).
   * LOD 0 = base mesh (vertex_buffer/index_buffer above).
   * lod_levels[i] stores alternate vertex/index buffers for LOD i+1.
   * active_lod is set each frame by screen-space size selection. */
  struct MopLodLevel {
    MopRhiBuffer *vertex_buffer;
    MopRhiBuffer *index_buffer;
    uint32_t vertex_count;
    uint32_t index_count;
    float screen_threshold; /* switch to this LOD below this pixel diameter */
  } lod_levels[MOP_MAX_LOD_LEVELS - 1]; /* LOD 1..7 */
  uint32_t lod_level_count; /* number of extra LOD levels (0 = no LOD) */
  uint32_t active_lod;      /* currently selected LOD (0 = highest detail) */
  uint32_t prev_lod;        /* previous frame's LOD (for transition detect) */

  /* Slot index in viewport->meshes[] — used for O(1) free-list removal.
   * The mesh pool stores pointers, so pointer arithmetic can't recover
   * the index; the mesh carries it. */
  uint32_t slot_index;

  /* Back-pointer to the owning viewport. Lets setters that take only a
   * MopMesh* (e.g. mop_mesh_set_position) auto-acquire the scene lock
   * without the host plumbing a viewport handle through every call. */
  struct MopViewport *viewport;
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

  /* Slot index in viewport->instanced_meshes[] — for O(1) removal. */
  uint32_t slot_index;

  /* Back-pointer to owning viewport (auto-lock in setters). */
  struct MopViewport *viewport;
};

/* -------------------------------------------------------------------------
 * Dynamic array initial capacities
 * ------------------------------------------------------------------------- */

#define MOP_INITIAL_LIGHT_CAPACITY 8
#define MOP_INITIAL_CAMERA_CAPACITY 16
#define MOP_INITIAL_SELECTED_CAPACITY 256
#define MOP_INITIAL_EVENT_CAPACITY 64
#define MOP_INITIAL_OVERLAY_CAPACITY 16
#define MOP_INITIAL_HOOK_CAPACITY 64
#define MOP_INITIAL_UNDO_CAPACITY 256
#define MOP_INITIAL_SELECTED_ELEMENTS_CAPACITY 4096

/* -------------------------------------------------------------------------
 * Dynamic array growth helper
 *
 * Doubles the capacity of a heap-allocated array when full.
 * Zero-initializes newly allocated elements.
 * Returns true on success, false on allocation failure.
 * ------------------------------------------------------------------------- */

static inline bool mop_dyn_grow(void **arr, uint32_t *cap, size_t elem_size,
                                uint32_t initial_cap) {
  uint32_t old_cap = *cap;
  uint32_t new_cap = old_cap ? old_cap * 2 : initial_cap;
  if (new_cap < old_cap)
    return false; /* overflow */
  void *new_arr = realloc(*arr, (size_t)new_cap * elem_size);
  if (!new_arr)
    return false;
  memset((char *)new_arr + (size_t)old_cap * elem_size, 0,
         (size_t)(new_cap - old_cap) * elem_size);
  *arr = new_arr;
  *cap = new_cap;
  return true;
}

/* -------------------------------------------------------------------------
 * Camera object (Phase 5)
 * ------------------------------------------------------------------------- */

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

  /* Viewport back-pointer for auto-lock on setters. */
  struct MopViewport *viewport;
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

typedef enum MopUndoEntryType {
  MOP_UNDO_TRS = 0,
  MOP_UNDO_MATERIAL = 1,
  MOP_UNDO_BATCH = 2,
} MopUndoEntryType;

typedef struct MopUndoEntry {
  MopUndoEntryType type;
  union {
    struct {
      uint32_t mesh_index;
      MopVec3 pos;
      MopVec3 rot;
      MopVec3 scale;
    } trs;
    struct {
      uint32_t mesh_index;
      MopMaterial material;
    } mat;
    struct {
      uint32_t count;
      struct MopUndoEntry *entries; /* heap-allocated sub-entries */
    } batch;
  };
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

  /* Multi-light system (dynamic array) */
  MopLight *lights;
  uint32_t light_count; /* high-water mark for iteration */
  uint32_t light_capacity;
  bool default_light_active; /* true until user calls add_light */

  /* Camera */
  MopVec3 cam_eye;
  MopVec3 cam_target;
  MopVec3 cam_up;
  float cam_fov_radians;
  float cam_near;
  float cam_far;
  MopCameraMode cam_mode; /* perspective or orthographic */
  float cam_ortho_size;   /* ortho half-height in world units */

  /* Computed camera matrices */
  MopMat4 view_matrix;
  MopMat4 projection_matrix;

  /* Scene — pointer-stable mesh pool.
   * `meshes` is an array of MopMesh* so that growing the pool (via realloc
   * of the pointer array) does not invalidate any MopMesh* that a host may
   * be holding. Individual MopMesh structs are heap-allocated once per
   * add_mesh and recycled via the free-list (never actually freed until
   * viewport destroy). `mesh_count` is the high-water mark of slots used
   * and bounds iteration; inactive slots in [0, mesh_count) are skipped. */
  struct MopMesh **meshes;
  uint32_t mesh_capacity;
  uint32_t mesh_count;
  uint32_t *mesh_free_list; /* stack of free slot indices for O(1) add */
  uint32_t mesh_free_count;
  uint32_t mesh_free_capacity;

  /* Instanced meshes — same pointer-stable scheme as meshes. */
  struct MopInstancedMesh **instanced_meshes;
  uint32_t instanced_capacity;
  uint32_t instanced_count;
  uint32_t *instanced_free_list;
  uint32_t instanced_free_count;
  uint32_t instanced_free_capacity;

  /* Owned subsystems */
  MopGizmo *gizmo;
  MopOrbitCamera camera;
  MopMesh *grid;

  /* Camera objects (Phase 5) — dynamic array */
  struct MopCameraObject *cameras;
  uint32_t camera_count;
  uint32_t camera_capacity;
  struct MopCameraObject *active_camera; /* NULL = use orbit camera */

  /* Gradient background (clip-space quad) */
  MopRhiBuffer *bg_vb;
  MopRhiBuffer *bg_ib;

  /* Axis indicator (corner widget) — one pair per axis (X, Y, Z) */
  MopRhiBuffer *axis_ind_vb[3];
  MopRhiBuffer *axis_ind_ib[3];
  uint32_t axis_ind_vcnt[3];
  uint32_t axis_ind_icnt[3];

  /* Selection (multi-object) — dynamic array */
  uint32_t *selected_ids;
  uint32_t selected_count;
  uint32_t selected_capacity;
  uint32_t selected_id; /* backward compat: selected_ids[0] or 0 */

  /* Sub-element selection (Phase 3) */
  MopSelection selection;

  /* Interaction state */
  MopInteractState interact_state;
  MopGizmoAxis drag_axis;
  MopGizmoAxis pending_gizmo_axis; /* gizmo hit on pointer-down, deferred */
  float click_start_x, click_start_y;

  /* Event queue (ring buffer) — dynamic array, capacity always power of 2 */
  MopEvent *events;
  uint32_t event_capacity;
  int event_head, event_tail;

  /* Profiling (Phase 5C) */
  MopFrameStats last_stats;

  /* Undo/redo (Phase 4B) — dynamic ring buffer */
  MopUndoEntry *undo_entries;
  uint32_t undo_capacity;
  int undo_head;
  int undo_count;
  int redo_count;

  /* Time tracking for simulation */
  float last_frame_time;
  float prev_frame_time; /* previous last_frame_time, for computing dt */

  /* Post-processing (Phase 6C) */
  uint32_t post_effects;
  MopFogParams fog_params;

  /* Overlay system — dynamic arrays */
  MopOverlayEntry *overlays;
  uint32_t overlay_count;
  uint32_t overlay_capacity;
  bool *overlay_enabled;

  /* Display settings */
  MopDisplaySettings display;

  /* Theme (design language) */
  MopTheme theme;

  /* Pipeline hooks (Phase D) — dynamic array */
  struct MopHookEntry {
    MopPipelineHookFn fn;
    void *data;
    MopPipelineStage stage;
    bool active;
  } *hooks;
  uint32_t hook_count;
  uint32_t hook_capacity;

  /* Frame callback */
  MopFrameCallbackFn frame_cb;
  void *frame_cb_data;

  /* Light indicators (visual representations of lights) — dynamic, tracks
   * light_capacity */
  MopMesh **light_indicators;

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

  /* Bloom parameters */
  float bloom_threshold; /* default 1.0 */
  float bloom_intensity; /* default 0.5 */

  /* SSR parameters */
  float ssr_intensity; /* default 0.5 */

  /* Volumetric fog parameters */
  MopVolumetricParams volumetric_params;

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

  /* Pre-allocated transparent sort arrays (reused across frames) */
  uint32_t *trans_sort_idx;
  float *trans_sort_dist;
  uint32_t trans_sort_capacity;

  /* Shader plugins — custom render passes injected by host app */
  struct MopShaderPlugin **shader_plugins;
  uint32_t shader_plugin_count;
  uint32_t shader_plugin_capacity;

  /* TAA (Temporal Anti-Aliasing) state */
  uint32_t taa_frame_index;         /* monotonic frame counter for jitter */
  float taa_jitter_x, taa_jitter_y; /* current frame jitter in pixels */
  MopMat4 taa_prev_view;            /* previous frame view matrix */
  MopMat4 taa_prev_proj;            /* previous frame projection matrix */
  bool taa_has_history;             /* false on first frame / after resize */

  /* Debug visualization mode (Phase 9B) */
  MopDebugViz debug_viz;

  /* LOD bias (Phase 9C) — shifts LOD level selection */
  float lod_bias;

  /* Error tracking for mop_viewport_render */
  MopRenderResult last_render_result;
  char last_render_error[256];

  /* Generic thread pool for render graph MT execution (Phase 1B).
   * Created in viewport_create, destroyed in viewport_destroy.
   * NULL if thread creation fails (falls back to single-threaded). */
  struct MopThreadPool *thread_pool;

  /* Scene mutex — serializes render vs. host mutation.
   *
   * RECURSIVE: every public mutation / reader function in the MOP API
   * acquires this internally, and the render path holds it for the
   * whole frame. Recursive semantics let internal paths call each other
   * without deadlock, and let hosts safely call MOP APIs from inside
   * their own scene_lock blocks.
   *
   * Single-threaded hosts pay only the uncontended fast path (~20 ns). */
  pthread_mutex_t scene_mutex;

  /* Texture cache — path-keyed dedup cache for mop_tex_load_async */
  struct MopTexCacheEntry {
    char path[256];
    MopTexture *texture;
  } *tex_cache;
  uint32_t tex_cache_count;
  uint32_t tex_cache_capacity;
  uint32_t tex_cache_hits; /* cumulative hit count for stats */
  uint32_t frame_counter;  /* monotonic frame counter for cache eviction */
};

/* -------------------------------------------------------------------------
 * Internal scene lock helpers
 *
 * Every public mutation / reader in the MOP API wraps its body with
 * MOP_VP_LOCK(vp) / MOP_VP_UNLOCK(vp). The mutex is recursive so nested
 * internal calls are safe. NULL viewport is tolerated (no-op) so callers
 * that perform their own argument validation don't need a pre-check.
 * ------------------------------------------------------------------------- */

static inline void mop_vp_lock(MopViewport *vp) {
  if (vp)
    pthread_mutex_lock(&vp->scene_mutex);
}

static inline void mop_vp_unlock(MopViewport *vp) {
  if (vp)
    pthread_mutex_unlock(&vp->scene_mutex);
}

#define MOP_VP_LOCK(vp) mop_vp_lock(vp)
#define MOP_VP_UNLOCK(vp) mop_vp_unlock(vp)

/* -------------------------------------------------------------------------
 * Internal subsystem functions
 * ------------------------------------------------------------------------- */

/* Apply post-processing effects to the framebuffer */
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

/* Texture cache cleanup — called from mop_viewport_destroy */
void mop_tex_cache_destroy_all(MopViewport *vp);

/* Shader plugin internals */
void mop_shader_plugins_destroy_all(MopViewport *vp);
void mop_shader_plugins_dispatch(MopViewport *vp, MopShaderPluginStage stage);

void mop_overlay_push_line(MopViewport *vp, float x0, float y0, float x1,
                           float y1, float r, float g, float b, float width,
                           float opacity, float depth);
void mop_overlay_push_circle(MopViewport *vp, float cx, float cy, float radius,
                             float r, float g, float b, float opacity,
                             float depth);
void mop_overlay_push_diamond(MopViewport *vp, float cx, float cy, float size,
                              float r, float g, float b, float width,
                              float opacity, float depth);

/* CPU-rasterize overlay primitives onto an RGBA8 buffer.
 *
 * Used in the post-readback path so 2D screen-space chrome (gizmo, axis
 * indicator, light / camera indicators) reliably draws on top of the
 * outline in the buffer the host actually displays. On Vulkan the GPU
 * overlay render targets an image that only becomes visible one frame
 * later, so the CPU paint on readback is what the user sees this frame.
 *
 * If depth_buf is non-NULL, primitives with depth >= 0 are depth-tested
 * against the scene depth buffer so indicators behind geometry are
 * correctly occluded. Primitives with depth < 0 are drawn on top
 * unconditionally (gizmo convention).
 *   - reverse_z: scene depth uses reverse-Z convention (closer = larger).
 *   - is_cpu_ndc: prim depth is raw NDC z in [-1, 1] (CPU / GL) and needs
 *     remapping to [0, 1] before comparison; false means already [0, 1].
 */
void mop_overlay_rasterize_prims_cpu(uint8_t *rgba, int w, int h,
                                     const MopOverlayPrim *prims,
                                     uint32_t count, const float *depth_buf,
                                     bool reverse_z, bool is_cpu_ndc);

#endif /* MOP_VIEWPORT_INTERNAL_H */
