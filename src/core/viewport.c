/*
 * Master of Puppets — Viewport Core
 * viewport.c — Viewport lifecycle, scene management, rendering orchestration
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/render_graph.h"
#include "core/thread_pool.h"
#include "core/viewport_internal.h"
#include "rhi/rhi.h"

#include <math.h>
#include <mop/util/log.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_POSIX_C_SOURCE) && !defined(MOP_PLATFORM_MACOS)
#define _POSIX_C_SOURCE 200112L
#endif
#include <unistd.h>

/* Profiling helper — defined in profile.c */
double mop_profile_now_ms(void);

/* -------------------------------------------------------------------------
 * Halton sequence for TAA sub-pixel jitter
 *
 * Returns values in [0, 1).  Subtracted by 0.5 to center around zero.
 * base=2 for X, base=3 for Y gives a low-discrepancy 2D sequence.
 * ------------------------------------------------------------------------- */

static float mop_halton(uint32_t index, uint32_t base) {
  float f = 1.0f;
  float result = 0.0f;
  uint32_t i = index;
  while (i > 0) {
    f /= (float)base;
    result += f * (float)(i % base);
    i /= base;
  }
  return result;
}

/* -------------------------------------------------------------------------
 * Pipeline hooks — dispatch helper
 * ------------------------------------------------------------------------- */

static void dispatch_hooks(MopViewport *vp, MopPipelineStage stage) {
  for (uint32_t i = 0; i < vp->hook_count; i++) {
    if (vp->hooks[i].active && vp->hooks[i].stage == stage && vp->hooks[i].fn) {
      vp->hooks[i].fn(vp, vp->hooks[i].data);
    }
  }
}

uint32_t mop_viewport_add_hook(MopViewport *vp, MopPipelineStage stage,
                               MopPipelineHookFn fn, void *user_data) {
  if (!vp || !fn || (int)stage < 0 || stage >= MOP_STAGE_COUNT)
    return UINT32_MAX;

  /* Find an inactive slot or use a new one */
  uint32_t slot = UINT32_MAX;
  for (uint32_t i = 0; i < vp->hook_count; i++) {
    if (!vp->hooks[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == UINT32_MAX) {
    if (vp->hook_count >= vp->hook_capacity) {
      if (!mop_dyn_grow((void **)&vp->hooks, &vp->hook_capacity,
                        sizeof(struct MopHookEntry), MOP_INITIAL_HOOK_CAPACITY))
        return UINT32_MAX;
    }
    slot = vp->hook_count++;
  }

  vp->hooks[slot].fn = fn;
  vp->hooks[slot].data = user_data;
  vp->hooks[slot].stage = stage;
  vp->hooks[slot].active = true;
  return slot;
}

void mop_viewport_remove_hook(MopViewport *vp, uint32_t handle) {
  if (!vp || handle >= vp->hook_count)
    return;
  vp->hooks[handle].active = false;
  vp->hooks[handle].fn = NULL;
}

void mop_viewport_set_frame_callback(MopViewport *vp, MopFrameCallbackFn fn,
                                     void *user_data) {
  if (!vp)
    return;
  vp->frame_cb = fn;
  vp->frame_cb_data = user_data;
}

/* -------------------------------------------------------------------------
 * Projection matrix helper — handles perspective, ortho, and reverse-Z
 * ------------------------------------------------------------------------- */

static MopMat4 compute_projection(MopCameraMode mode, float fov_radians,
                                  float ortho_size, float aspect,
                                  float near_plane, float far_plane,
                                  bool reverse_z) {
  if (mode == MOP_CAMERA_ORTHOGRAPHIC) {
    float half_w = ortho_size * aspect;
    float half_h = ortho_size;
    return mop_mat4_ortho(-half_w, half_w, -half_h, half_h, near_plane,
                          far_plane);
  }
  if (reverse_z)
    return mop_mat4_perspective_reverse_z(fov_radians, aspect, near_plane);
  return mop_mat4_perspective(fov_radians, aspect, near_plane, far_plane);
}

#define MOP_INITIAL_MESH_CAPACITY 64
#define MOP_GRID_ID                                                            \
  0xFFFD0001u /* chrome range — outline overlay ignores this */

/* -------------------------------------------------------------------------
 * Ground grid generation
 *
 * Professional 20x20 grid on Y=0:
 *   - Center axis lines: X = red, Z = blue (wider)
 *   - Major lines every 5 units (medium brightness, medium width)
 *   - Minor lines at every 1 unit (dim, thin)
 * ------------------------------------------------------------------------- */

static MopMesh *create_grid(MopViewport *vp) {
  const int ext = 20;
  const int lines_per_axis = 2 * ext + 1;
  const int total_lines = lines_per_axis * 2;
  const int vert_count = total_lines * 4;
  const int idx_count = total_lines * 6;

  MopVertex *v = malloc((size_t)vert_count * sizeof(MopVertex));
  uint32_t *ix = malloc((size_t)idx_count * sizeof(uint32_t));
  if (!v || !ix) {
    free(v);
    free(ix);
    return NULL;
  }

  MopVec3 n = {0, 1, 0};
  MopColor c_minor = vp->theme.grid_minor;
  MopColor c_major = vp->theme.grid_major;
  MopColor c_x = vp->theme.grid_axis_x;
  MopColor c_z = vp->theme.grid_axis_z;

  int vi = 0, ii = 0;
  float fext = (float)ext;
  float hw_axis = 0.025f, hw_major = 0.018f, hw_minor = 0.012f;

  for (int a = 0; a < 2; a++) {
    for (int i = 0; i < lines_per_axis; i++) {
      float coord = -fext + (float)i;
      int offset = i - ext;
      MopColor c;
      float hw;
      if (offset == 0) {
        c = (a == 0) ? c_z : c_x;
        hw = hw_axis;
      } else if (offset % 5 == 0) {
        c = c_major;
        hw = hw_major;
      } else {
        c = c_minor;
        hw = hw_minor;
      }
      int b = vi;
      if (a == 0) { /* Z-parallel */
        v[vi++] = (MopVertex){{coord - hw, 0, -fext}, n, c, 0, 0};
        v[vi++] = (MopVertex){{coord + hw, 0, -fext}, n, c, 0, 0};
        v[vi++] = (MopVertex){{coord + hw, 0, fext}, n, c, 0, 0};
        v[vi++] = (MopVertex){{coord - hw, 0, fext}, n, c, 0, 0};
      } else { /* X-parallel */
        v[vi++] = (MopVertex){{-fext, 0, coord - hw}, n, c, 0, 0};
        v[vi++] = (MopVertex){{fext, 0, coord - hw}, n, c, 0, 0};
        v[vi++] = (MopVertex){{fext, 0, coord + hw}, n, c, 0, 0};
        v[vi++] = (MopVertex){{-fext, 0, coord + hw}, n, c, 0, 0};
      }
      ix[ii++] = (uint32_t)b;
      ix[ii++] = (uint32_t)(b + 2);
      ix[ii++] = (uint32_t)(b + 1);
      ix[ii++] = (uint32_t)b;
      ix[ii++] = (uint32_t)(b + 3);
      ix[ii++] = (uint32_t)(b + 2);
    }
  }

  MopMesh *grid = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = v,
                         .vertex_count = (uint32_t)vert_count,
                         .indices = ix,
                         .index_count = (uint32_t)idx_count,
                         .object_id = MOP_GRID_ID});
  free(v);
  free(ix);
  return grid;
}

/* -------------------------------------------------------------------------
 * Gradient background (clip-space fullscreen quad)
 *
 * Top = lighter charcoal (0.22, 0.22, 0.25)
 * Bottom = near-black (0.11, 0.11, 0.13)
 * z = 0.9999 in clip space so all scene geometry renders in front.
 * Drawn with identity MVP, ambient=1.0 (lighting disabled), smooth shading.
 * ------------------------------------------------------------------------- */

static void create_gradient_bg(MopViewport *vp) {
  /* Alpha=0 marks background pixels so the tonemap pass skips
   * exposure / ACES for them — background brightness must stay
   * constant regardless of the exposure slider. */
  MopColor c_top = vp->theme.bg_top;
  c_top.a = 0.0f;
  MopColor c_bot = vp->theme.bg_bottom;
  c_bot.a = 0.0f;
  MopVec3 n = {0, 0, 1};

  MopVertex verts[4] = {
      {{-1.0f, -1.0f, 0.9999f}, n, c_bot, 0, 0}, /* bottom-left  */
      {{1.0f, -1.0f, 0.9999f}, n, c_bot, 0, 0},  /* bottom-right */
      {{1.0f, 1.0f, 0.9999f}, n, c_top, 0, 0},   /* top-right    */
      {{-1.0f, 1.0f, 0.9999f}, n, c_top, 0, 0},  /* top-left     */
  };
  uint32_t indices[6] = {0, 1, 2, 2, 3, 0};

  MopRhiBufferDesc vb_desc = {.data = verts, .size = sizeof(verts)};
  MopRhiBufferDesc ib_desc = {.data = indices, .size = sizeof(indices)};
  vp->bg_vb = vp->rhi->buffer_create(vp->device, &vb_desc);
  vp->bg_ib = vp->rhi->buffer_create(vp->device, &ib_desc);
}

/* -------------------------------------------------------------------------
 * Axis indicator (bottom-left corner widget)
 *
 * Three small colored axis arrows showing camera orientation.
 * Each axis = two perpendicular thin quads (cross shape) + pyramid tip.
 * X = red, Y = green, Z = blue.  Not pickable (object_id = 0).
 * ------------------------------------------------------------------------- */

static void create_one_axis(MopViewport *vp, int idx, MopVec3 dir,
                            MopVec3 perp1, MopVec3 perp2, MopColor color) {
  /* Shaft: two perpendicular thin quads from origin to 0.7 along dir */
  const float shaft_len = 0.7f;
  const float shaft_hw = 0.02f;
  /* Arrowhead: pyramid from 0.6 to 1.0 along dir */
  const float tip_base = 0.6f;
  const float tip_end = 1.0f;
  const float tip_hw = 0.06f;

  MopVec3 n = dir;
  MopColor c = color;

  /* 4 verts per quad × 2 quads for shaft = 8, + 5 for pyramid tip = 13 */
  MopVertex verts[13];
  uint32_t indices[30]; /* 2 quads × 6 + 4 tri faces × 3 + 2 base tris × 3 = 12
                           + 12 + 6 = 30 */
  int vi = 0, ii = 0;

  /* Shaft quad 1 (along perp1) */
  MopVec3 s0 = {0, 0, 0};
  MopVec3 s1 = {dir.x * shaft_len, dir.y * shaft_len, dir.z * shaft_len};
  verts[vi++] =
      (MopVertex){{s0.x - perp1.x * shaft_hw, s0.y - perp1.y * shaft_hw,
                   s0.z - perp1.z * shaft_hw},
                  n,
                  c,
                  0,
                  0};
  verts[vi++] =
      (MopVertex){{s0.x + perp1.x * shaft_hw, s0.y + perp1.y * shaft_hw,
                   s0.z + perp1.z * shaft_hw},
                  n,
                  c,
                  0,
                  0};
  verts[vi++] =
      (MopVertex){{s1.x + perp1.x * shaft_hw, s1.y + perp1.y * shaft_hw,
                   s1.z + perp1.z * shaft_hw},
                  n,
                  c,
                  0,
                  0};
  verts[vi++] =
      (MopVertex){{s1.x - perp1.x * shaft_hw, s1.y - perp1.y * shaft_hw,
                   s1.z - perp1.z * shaft_hw},
                  n,
                  c,
                  0,
                  0};
  indices[ii++] = 0;
  indices[ii++] = 1;
  indices[ii++] = 2;
  indices[ii++] = 2;
  indices[ii++] = 3;
  indices[ii++] = 0;

  /* Shaft quad 2 (along perp2) */
  verts[vi++] =
      (MopVertex){{s0.x - perp2.x * shaft_hw, s0.y - perp2.y * shaft_hw,
                   s0.z - perp2.z * shaft_hw},
                  n,
                  c,
                  0,
                  0};
  verts[vi++] =
      (MopVertex){{s0.x + perp2.x * shaft_hw, s0.y + perp2.y * shaft_hw,
                   s0.z + perp2.z * shaft_hw},
                  n,
                  c,
                  0,
                  0};
  verts[vi++] =
      (MopVertex){{s1.x + perp2.x * shaft_hw, s1.y + perp2.y * shaft_hw,
                   s1.z + perp2.z * shaft_hw},
                  n,
                  c,
                  0,
                  0};
  verts[vi++] =
      (MopVertex){{s1.x - perp2.x * shaft_hw, s1.y - perp2.y * shaft_hw,
                   s1.z - perp2.z * shaft_hw},
                  n,
                  c,
                  0,
                  0};
  indices[ii++] = 4;
  indices[ii++] = 5;
  indices[ii++] = 6;
  indices[ii++] = 6;
  indices[ii++] = 7;
  indices[ii++] = 4;

  /* Arrowhead: tip vertex + 4 base vertices */
  MopVec3 tip = {dir.x * tip_end, dir.y * tip_end, dir.z * tip_end};
  MopVec3 base_c = {dir.x * tip_base, dir.y * tip_base, dir.z * tip_base};

  int tip_vi = vi;
  verts[vi++] = (MopVertex){{tip.x, tip.y, tip.z}, n, c, 0, 0}; /* apex = 8 */
  verts[vi++] =
      (MopVertex){{base_c.x - perp1.x * tip_hw, base_c.y - perp1.y * tip_hw,
                   base_c.z - perp1.z * tip_hw},
                  n,
                  c,
                  0,
                  0}; /* 9 */
  verts[vi++] =
      (MopVertex){{base_c.x + perp2.x * tip_hw, base_c.y + perp2.y * tip_hw,
                   base_c.z + perp2.z * tip_hw},
                  n,
                  c,
                  0,
                  0}; /* 10 */
  verts[vi++] =
      (MopVertex){{base_c.x + perp1.x * tip_hw, base_c.y + perp1.y * tip_hw,
                   base_c.z + perp1.z * tip_hw},
                  n,
                  c,
                  0,
                  0}; /* 11 */
  verts[vi++] =
      (MopVertex){{base_c.x - perp2.x * tip_hw, base_c.y - perp2.y * tip_hw,
                   base_c.z - perp2.z * tip_hw},
                  n,
                  c,
                  0,
                  0}; /* 12 */

  /* 4 side faces of pyramid */
  int a = tip_vi; /* apex */
  int b0 = tip_vi + 1, b1 = tip_vi + 2, b2 = tip_vi + 3, b3 = tip_vi + 4;
  indices[ii++] = (uint32_t)a;
  indices[ii++] = (uint32_t)b0;
  indices[ii++] = (uint32_t)b1;
  indices[ii++] = (uint32_t)a;
  indices[ii++] = (uint32_t)b1;
  indices[ii++] = (uint32_t)b2;
  indices[ii++] = (uint32_t)a;
  indices[ii++] = (uint32_t)b2;
  indices[ii++] = (uint32_t)b3;
  indices[ii++] = (uint32_t)a;
  indices[ii++] = (uint32_t)b3;
  indices[ii++] = (uint32_t)b0;
  /* Base (2 tris) */
  indices[ii++] = (uint32_t)b0;
  indices[ii++] = (uint32_t)b2;
  indices[ii++] = (uint32_t)b1;
  indices[ii++] = (uint32_t)b0;
  indices[ii++] = (uint32_t)b3;
  indices[ii++] = (uint32_t)b2;

  MopRhiBufferDesc vb_desc = {.data = verts,
                              .size = (uint32_t)vi * sizeof(MopVertex)};
  MopRhiBufferDesc ib_desc = {.data = indices,
                              .size = (uint32_t)ii * sizeof(uint32_t)};
  vp->axis_ind_vb[idx] = vp->rhi->buffer_create(vp->device, &vb_desc);
  vp->axis_ind_ib[idx] = vp->rhi->buffer_create(vp->device, &ib_desc);
  vp->axis_ind_vcnt[idx] = (uint32_t)vi;
  vp->axis_ind_icnt[idx] = (uint32_t)ii;
}

static void create_axis_indicator(MopViewport *vp) {
  create_one_axis(vp, 0, (MopVec3){1, 0, 0}, (MopVec3){0, 1, 0},
                  (MopVec3){0, 0, 1}, vp->theme.axis_x); /* X = red */
  create_one_axis(vp, 1, (MopVec3){0, 1, 0}, (MopVec3){1, 0, 0},
                  (MopVec3){0, 0, 1}, vp->theme.axis_y); /* Y = green */
  create_one_axis(vp, 2, (MopVec3){0, 0, 1}, (MopVec3){1, 0, 0},
                  (MopVec3){0, 1, 0}, vp->theme.axis_z); /* Z = blue */
}

/* -------------------------------------------------------------------------
 * Viewport lifecycle
 * ------------------------------------------------------------------------- */

MopViewport *mop_viewport_create(const MopViewportDesc *desc) {
  if (!desc || desc->width <= 0 || desc->height <= 0) {
    MOP_ERROR("invalid viewport descriptor");
    return NULL;
  }

  const MopRhiBackend *rhi = mop_rhi_get_backend(desc->backend);
  if (!rhi) {
    MOP_ERROR("backend '%s' not available", mop_backend_name(desc->backend));
    return NULL;
  }

  MopRhiDevice *device = rhi->device_create();
  if (!device) {
    MOP_ERROR("device creation failed for backend '%s'", rhi->name);
    return NULL;
  }

  const int ssaa = 2; /* 2x supersampling for smooth edges */
  MopRhiFramebufferDesc fb_desc = {.width = desc->width * ssaa,
                                   .height = desc->height * ssaa};
  MopRhiFramebuffer *fb = rhi->framebuffer_create(device, &fb_desc);
  if (!fb) {
    rhi->device_destroy(device);
    return NULL;
  }

  MopViewport *vp = calloc(1, sizeof(MopViewport));
  if (!vp) {
    rhi->framebuffer_destroy(device, fb);
    rhi->device_destroy(device);
    return NULL;
  }

  /* Dynamic mesh array (Phase 5B) */
  vp->meshes = calloc(MOP_INITIAL_MESH_CAPACITY, sizeof(struct MopMesh));
  if (!vp->meshes) {
    rhi->framebuffer_destroy(device, fb);
    rhi->device_destroy(device);
    free(vp);
    return NULL;
  }
  vp->mesh_capacity = MOP_INITIAL_MESH_CAPACITY;

  /* Instanced mesh array (Phase 6B) */
  vp->instanced_meshes =
      calloc(MOP_INITIAL_INSTANCED_CAPACITY, sizeof(struct MopInstancedMesh));
  if (!vp->instanced_meshes) {
    free(vp->meshes);
    rhi->framebuffer_destroy(device, fb);
    rhi->device_destroy(device);
    free(vp);
    return NULL;
  }
  vp->instanced_capacity = MOP_INITIAL_INSTANCED_CAPACITY;
  vp->instanced_count = 0;

  vp->rhi = rhi;
  vp->device = device;
  vp->framebuffer = fb;
  vp->backend_type = (desc->backend == MOP_BACKEND_AUTO) ? mop_backend_default()
                                                         : desc->backend;
  vp->width = desc->width;
  vp->height = desc->height;
  vp->ssaa_factor = ssaa;
  vp->ssaa_color_buf =
      calloc((size_t)desc->width * desc->height * 4, sizeof(uint8_t));
  if (!vp->ssaa_color_buf) {
    free(vp->instanced_meshes);
    free(vp->meshes);
    rhi->framebuffer_destroy(device, fb);
    rhi->device_destroy(device);
    free(vp);
    return NULL;
  }
  vp->overlay_prims = calloc(MOP_MAX_OVERLAY_PRIMS, sizeof(MopOverlayPrim));
  if (!vp->overlay_prims) {
    free(vp->ssaa_color_buf);
    free(vp->instanced_meshes);
    free(vp->meshes);
    rhi->framebuffer_destroy(device, fb);
    rhi->device_destroy(device);
    free(vp);
    return NULL;
  }
  vp->overlay_prim_count = 0;

  vp->render_mode = MOP_RENDER_SOLID;
  vp->light_dir = (MopVec3){0.3f, 1.0f, 0.5f};
  vp->ambient = 0.25f;
  vp->shading_mode = MOP_SHADING_SMOOTH;
  vp->post_effects = MOP_POST_GAMMA | MOP_POST_FXAA;
  vp->exposure = 1.0f;
  vp->bloom_threshold = 1.0f;
  vp->bloom_intensity = 0.5f;
  vp->ssr_intensity = 0.5f;
  vp->volumetric_params = (MopVolumetricParams){
      .density = 0.02f,
      .color = {1.0f, 1.0f, 1.0f, 1.0f},
      .anisotropy = 0.3f,
      .steps = 32,
  };
  vp->env_type = MOP_ENV_GRADIENT;
  vp->env_intensity = 1.0f;
  vp->mesh_count = 0;

  /* Theme: MOP design language — must be initialized before visual resource
   * creation so that create_gradient_bg, create_grid, and create_axis_indicator
   * can read from vp->theme. */
  vp->theme = mop_theme_default();
  vp->clear_color = vp->theme.bg_bottom;

  /* Multi-light system (dynamic array) */
  vp->light_capacity = MOP_INITIAL_LIGHT_CAPACITY;
  vp->lights = calloc(vp->light_capacity, sizeof(MopLight));
  vp->light_indicators = calloc(vp->light_capacity, sizeof(MopMesh *));
  if (!vp->lights || !vp->light_indicators) {
    free(vp->lights);
    free(vp->light_indicators);
    free(vp->overlay_prims);
    free(vp->ssaa_color_buf);
    free(vp->instanced_meshes);
    free(vp->meshes);
    rhi->framebuffer_destroy(device, fb);
    rhi->device_destroy(device);
    free(vp);
    return NULL;
  }
  vp->lights[0] = (MopLight){
      .type = MOP_LIGHT_DIRECTIONAL,
      .direction = vp->light_dir,
      .color = (MopColor){1.0f, 1.0f, 1.0f, 1.0f},
      .intensity = 1.0f - vp->ambient,
      .active = true,
  };
  vp->light_count = 1;
  vp->default_light_active = true;

  /* Dynamic arrays: cameras, hooks, overlays, selected, events, undo */
  vp->camera_capacity = MOP_INITIAL_CAMERA_CAPACITY;
  vp->cameras = calloc(vp->camera_capacity, sizeof(struct MopCameraObject));

  vp->hook_capacity = MOP_INITIAL_HOOK_CAPACITY;
  vp->hooks = calloc(vp->hook_capacity, sizeof(struct MopHookEntry));

  vp->overlay_capacity = MOP_INITIAL_OVERLAY_CAPACITY;
  vp->overlays = calloc(vp->overlay_capacity, sizeof(MopOverlayEntry));
  vp->overlay_enabled = calloc(vp->overlay_capacity, sizeof(bool));

  vp->selected_capacity = MOP_INITIAL_SELECTED_CAPACITY;
  vp->selected_ids = calloc(vp->selected_capacity, sizeof(uint32_t));

  vp->event_capacity = MOP_INITIAL_EVENT_CAPACITY;
  vp->events = calloc(vp->event_capacity, sizeof(MopEvent));

  vp->undo_capacity = MOP_INITIAL_UNDO_CAPACITY;
  vp->undo_entries = calloc(vp->undo_capacity, sizeof(MopUndoEntry));

  vp->selection.element_capacity = MOP_INITIAL_SELECTED_ELEMENTS_CAPACITY;
  vp->selection.elements =
      calloc(vp->selection.element_capacity, sizeof(uint32_t));

  /* Verify all dynamic allocations */
  if (!vp->cameras || !vp->hooks || !vp->overlays || !vp->overlay_enabled ||
      !vp->selected_ids || !vp->events || !vp->undo_entries ||
      !vp->selection.elements) {
    free(vp->selection.elements);
    free(vp->undo_entries);
    free(vp->events);
    free(vp->selected_ids);
    free(vp->overlay_enabled);
    free(vp->overlays);
    free(vp->hooks);
    free(vp->cameras);
    free(vp->light_indicators);
    free(vp->lights);
    free(vp->overlay_prims);
    free(vp->ssaa_color_buf);
    free(vp->instanced_meshes);
    free(vp->meshes);
    rhi->framebuffer_destroy(device, fb);
    rhi->device_destroy(device);
    free(vp);
    return NULL;
  }

  /* Display settings + overlays: all disabled by default except outline */
  vp->display = mop_display_settings_default();
  vp->overlay_count = MOP_OVERLAY_BUILTIN_COUNT;
  vp->overlay_enabled[MOP_OVERLAY_OUTLINE] = true; /* always-on by default */

  /* Owned subsystems */
  vp->camera = mop_orbit_camera_default();
  vp->gizmo = mop_gizmo_create(vp);
  create_gradient_bg(vp);
  vp->grid = create_grid(vp);
  create_axis_indicator(vp);

  /* Chrome defaults to visible */
  vp->show_chrome = true;

  /* Reversed-Z depth — always enabled for Vulkan (hardware support) */
  vp->reverse_z =
      (vp->backend_type == MOP_BACKEND_VULKAN) ? true : desc->reverse_z;

  /* Register built-in subsystems */
  mop_postprocess_register(vp);

  /* Interaction state */
  vp->selected_id = 0;
  vp->selected_count = 0;
  vp->interact_state = MOP_INTERACT_IDLE;
  vp->drag_axis = MOP_GIZMO_AXIS_NONE;
  vp->event_head = 0;
  vp->event_tail = 0;

  /* Apply camera to set initial matrices */
  mop_orbit_camera_apply(&vp->camera, vp);

  /* Create generic thread pool for render graph MT execution.
   * Non-fatal: NULL pool falls back to sequential execution. */
  {
    int num_threads = 3; /* leave 1 core for main thread */
#if defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 1)
      num_threads = (int)(n - 1 > 15 ? 15 : n - 1);
#endif
    vp->thread_pool = mop_threadpool_create(num_threads);
  }

  return vp;
}

void mop_viewport_destroy(MopViewport *viewport) {
  if (!viewport) {
    return;
  }

  /* Destroy camera object meshes (Phase 5) */
  for (uint32_t i = 0; i < viewport->camera_count; i++) {
    struct MopCameraObject *cam = &viewport->cameras[i];
    if (cam->active) {
      if (cam->frustum_mesh)
        mop_viewport_remove_mesh(viewport, cam->frustum_mesh);
      if (cam->icon_mesh)
        mop_viewport_remove_mesh(viewport, cam->icon_mesh);
      cam->active = false;
    }
  }

  /* Destroy light indicators */
  mop_light_destroy_indicators(viewport);

  /* Destroy owned gizmo */
  if (viewport->gizmo) {
    mop_gizmo_destroy(viewport->gizmo);
    viewport->gizmo = NULL;
  }

  /* Destroy shader plugins (before device teardown) */
  mop_shader_plugins_destroy_all(viewport);

  /* Destroy all registered subsystems (water, particles, postprocess, etc.)
   * Must happen before meshes since subsystems may own internal meshes. */
  mop_subsystem_destroy_all(&viewport->subsystems, viewport);

  /* Free the legacy tracking arrays (kept for backward-compat API) */
  free(viewport->water_surfaces);
  viewport->water_surfaces = NULL;
  viewport->water_count = 0;

  free(viewport->emitters);
  viewport->emitters = NULL;
  viewport->emitter_count = 0;

  /* Destroy all active instanced meshes (Phase 6B) */
  for (uint32_t i = 0; i < viewport->instanced_count; i++) {
    struct MopInstancedMesh *im = &viewport->instanced_meshes[i];
    if (im->active) {
      if (im->vertex_buffer)
        viewport->rhi->buffer_destroy(viewport->device, im->vertex_buffer);
      if (im->index_buffer)
        viewport->rhi->buffer_destroy(viewport->device, im->index_buffer);
      free(im->transforms);
      im->active = false;
    }
  }

  /* Destroy environment map resources */
  if (viewport->env_texture)
    viewport->rhi->texture_destroy(viewport->device, viewport->env_texture);
  if (viewport->env_irradiance)
    viewport->rhi->texture_destroy(viewport->device, viewport->env_irradiance);
  if (viewport->env_prefiltered)
    viewport->rhi->texture_destroy(viewport->device, viewport->env_prefiltered);
  if (viewport->env_brdf_lut)
    viewport->rhi->texture_destroy(viewport->device, viewport->env_brdf_lut);
  free(viewport->env_hdr_data);
  free(viewport->env_irradiance_data);
  free(viewport->env_prefiltered_data);
  free(viewport->env_brdf_lut_data);

  /* Destroy gradient background buffers */
  if (viewport->bg_vb)
    viewport->rhi->buffer_destroy(viewport->device, viewport->bg_vb);
  if (viewport->bg_ib)
    viewport->rhi->buffer_destroy(viewport->device, viewport->bg_ib);

  /* Destroy axis indicator buffers */
  for (int i = 0; i < 3; i++) {
    if (viewport->axis_ind_vb[i])
      viewport->rhi->buffer_destroy(viewport->device, viewport->axis_ind_vb[i]);
    if (viewport->axis_ind_ib[i])
      viewport->rhi->buffer_destroy(viewport->device, viewport->axis_ind_ib[i]);
  }

  /* Destroy all active mesh buffers */
  for (uint32_t i = 0; i < viewport->mesh_count; i++) {
    struct MopMesh *mesh = &viewport->meshes[i];
    if (mesh->active) {
      if (mesh->vertex_buffer) {
        viewport->rhi->buffer_destroy(viewport->device, mesh->vertex_buffer);
      }
      if (mesh->index_buffer) {
        viewport->rhi->buffer_destroy(viewport->device, mesh->index_buffer);
      }
      free(mesh->vertex_format);
      mesh->vertex_format = NULL;
      mesh->active = false;
    }
  }

  if (viewport->framebuffer) {
    viewport->rhi->framebuffer_destroy(viewport->device, viewport->framebuffer);
  }
  if (viewport->device) {
    viewport->rhi->device_destroy(viewport->device);
  }

  free(viewport->overlay_prims);
  free(viewport->ssaa_color_buf);
  free(viewport->trans_sort_idx);
  free(viewport->trans_sort_dist);
  mop_sw_framebuffer_free(&viewport->shadow_fb);
  free(viewport->instanced_meshes);
  free(viewport->meshes);

  /* Free dynamic arrays */
  free(viewport->lights);
  free(viewport->light_indicators);
  free(viewport->cameras);
  free(viewport->hooks);
  free(viewport->overlays);
  free(viewport->overlay_enabled);
  free(viewport->selected_ids);
  free(viewport->events);
  /* Free batch heap memory in undo entries before freeing the array */
  for (uint32_t i = 0; i < viewport->undo_capacity; i++) {
    if (viewport->undo_entries[i].type == MOP_UNDO_BATCH &&
        viewport->undo_entries[i].batch.entries) {
      free(viewport->undo_entries[i].batch.entries);
    }
  }
  free(viewport->undo_entries);
  free(viewport->selection.elements);

  /* Destroy thread pool (Phase 1B) */
  mop_threadpool_destroy(viewport->thread_pool);

  free(viewport);
}

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

void mop_viewport_resize(MopViewport *viewport, int width, int height) {
  if (!viewport || width <= 0 || height <= 0) {
    return;
  }
  viewport->width = width;
  viewport->height = height;

  /* Resize the RHI framebuffer at SSAA resolution */
  int sf = viewport->ssaa_factor;
  viewport->rhi->framebuffer_resize(viewport->device, viewport->framebuffer,
                                    width * sf, height * sf);

  /* Reallocate the downsample buffer for the new presentation size */
  free(viewport->ssaa_color_buf);
  viewport->ssaa_color_buf =
      calloc((size_t)width * height * 4, sizeof(uint8_t));

  /* Invalidate TAA history — resolution changed */
  viewport->taa_has_history = false;

  /* Recompute projection matrix */
  float aspect = (float)width / (float)height;
  viewport->projection_matrix = compute_projection(
      viewport->cam_mode, viewport->cam_fov_radians, viewport->cam_ortho_size,
      aspect, viewport->cam_near, viewport->cam_far, viewport->reverse_z);
}

void mop_viewport_set_clear_color(MopViewport *viewport, MopColor color) {
  if (!viewport)
    return;
  viewport->clear_color = color;
}

void mop_viewport_set_render_mode(MopViewport *viewport, MopRenderMode mode) {
  if (!viewport)
    return;
  viewport->render_mode = mode;
}

void mop_viewport_set_light_dir(MopViewport *viewport, MopVec3 dir) {
  if (!viewport)
    return;
  viewport->light_dir = dir;
  /* Sync with multi-light system */
  viewport->lights[0].direction = dir;
}

void mop_viewport_set_ambient(MopViewport *viewport, float ambient) {
  if (!viewport)
    return;
  viewport->ambient = (ambient < 0.0f)   ? 0.0f
                      : (ambient > 1.0f) ? 1.0f
                                         : ambient;
  /* Sync with multi-light system: intensity = 1 - ambient */
  viewport->lights[0].intensity = 1.0f - viewport->ambient;
}

void mop_viewport_set_shading(MopViewport *viewport, MopShadingMode mode) {
  if (!viewport)
    return;
  viewport->shading_mode = mode;
}

void mop_viewport_set_camera(MopViewport *viewport, MopVec3 eye, MopVec3 target,
                             MopVec3 up, float fov_degrees, float near_plane,
                             float far_plane) {
  if (!viewport)
    return;

  viewport->cam_eye = eye;
  viewport->cam_target = target;
  viewport->cam_up = up;
  viewport->cam_fov_radians = fov_degrees * (3.14159265358979323846f / 180.0f);
  viewport->cam_near = near_plane;
  viewport->cam_far = far_plane;

  viewport->view_matrix = mop_mat4_look_at(eye, target, up);

  float aspect = (float)viewport->width / (float)viewport->height;
  viewport->projection_matrix = compute_projection(
      viewport->cam_mode, viewport->cam_fov_radians, viewport->cam_ortho_size,
      aspect, near_plane, far_plane, viewport->reverse_z);

  /* Sync the orbit camera so mop_orbit_camera_apply() won't overwrite.
   * Convert eye/target to spherical orbit parameters (distance, yaw, pitch). */
  MopVec3 offset = mop_vec3_sub(eye, target);
  float dist = mop_vec3_length(offset);
  if (dist > 1e-6f) {
    viewport->camera.target = target;
    viewport->camera.distance = dist;
    viewport->camera.yaw = atan2f(offset.x, offset.z);
    viewport->camera.pitch = asinf(offset.y / dist);
    viewport->camera.fov_degrees = fov_degrees;
    viewport->camera.near_plane = near_plane;
    viewport->camera.far_plane = far_plane;
  }
}

void mop_viewport_set_camera_orbit(MopViewport *viewport, MopVec3 eye,
                                   MopVec3 target, MopVec3 up,
                                   float fov_degrees, float near_plane,
                                   float far_plane) {
  if (!viewport)
    return;

  viewport->cam_eye = eye;
  viewport->cam_target = target;
  viewport->cam_up = up;
  viewport->cam_fov_radians = fov_degrees * (3.14159265358979323846f / 180.0f);
  viewport->cam_near = near_plane;
  viewport->cam_far = far_plane;

  viewport->view_matrix = mop_mat4_look_at(eye, target, up);

  float aspect = (float)viewport->width / (float)viewport->height;
  viewport->projection_matrix = compute_projection(
      viewport->cam_mode, viewport->cam_fov_radians, viewport->cam_ortho_size,
      aspect, near_plane, far_plane, viewport->reverse_z);
  /* Skip orbit parameter reconstruction — the orbit camera already holds
   * the authoritative pitch/yaw/distance values and asinf() would clamp
   * pitch to [-π/2, π/2], preventing full vertical orbit. */
}

MopBackendType mop_viewport_get_backend(MopViewport *viewport) {
  if (!viewport)
    return MOP_BACKEND_CPU;
  return viewport->backend_type;
}

float mop_viewport_gpu_frame_time_ms(MopViewport *viewport) {
  if (!viewport || !viewport->rhi || !viewport->rhi->frame_gpu_time_ms)
    return 0.0f;
  return viewport->rhi->frame_gpu_time_ms(viewport->device);
}

void mop_viewport_set_camera_mode(MopViewport *viewport, MopCameraMode mode) {
  if (!viewport)
    return;
  viewport->cam_mode = mode;
  viewport->camera.mode = mode;

  /* Compute matching ortho_size from current perspective params so switching
   * modes preserves the visible scene extent at the target point. */
  if (mode == MOP_CAMERA_ORTHOGRAPHIC && viewport->camera.ortho_size <= 0.0f) {
    float half_fov = viewport->cam_fov_radians * 0.5f;
    viewport->camera.ortho_size = viewport->camera.distance * tanf(half_fov);
    viewport->cam_ortho_size = viewport->camera.ortho_size;
  }

  /* Recompute projection */
  float aspect = (float)viewport->width / (float)viewport->height;
  viewport->projection_matrix = compute_projection(
      viewport->cam_mode, viewport->cam_fov_radians, viewport->cam_ortho_size,
      aspect, viewport->cam_near, viewport->cam_far, viewport->reverse_z);
}

MopCameraMode mop_viewport_get_camera_mode(const MopViewport *viewport) {
  if (!viewport)
    return MOP_CAMERA_PERSPECTIVE;
  return viewport->cam_mode;
}

MopVec3 mop_viewport_get_camera_eye(const MopViewport *viewport) {
  if (!viewport)
    return (MopVec3){0, 0, 0};
  return viewport->cam_eye;
}

MopVec3 mop_viewport_get_camera_target(const MopViewport *viewport) {
  if (!viewport)
    return (MopVec3){0, 0, 0};
  return viewport->cam_target;
}

/* -------------------------------------------------------------------------
 * Scene management
 * ------------------------------------------------------------------------- */

MopMesh *mop_viewport_add_mesh(MopViewport *viewport, const MopMeshDesc *desc) {
  if (!viewport || !desc || !desc->vertices || !desc->indices) {
    MOP_ERROR("invalid mesh descriptor");
    return NULL;
  }
  if (desc->vertex_count == 0 || desc->index_count == 0) {
    MOP_ERROR("mesh has zero vertices or indices");
    return NULL;
  }
  if (desc->index_count % 3 != 0) {
    MOP_ERROR("index count %u is not a multiple of 3", desc->index_count);
    return NULL;
  }
  /* Find a free slot (reuse inactive entries) */
  uint32_t slot = viewport->mesh_count;
  for (uint32_t i = 0; i < viewport->mesh_count; i++) {
    if (!viewport->meshes[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == viewport->mesh_count) {
    /* Need a new slot — grow if at capacity */
    if (viewport->mesh_count >= viewport->mesh_capacity) {
      if (viewport->mesh_capacity > UINT32_MAX / 2) {
        MOP_ERROR("mesh array capacity overflow");
        return NULL;
      }
      uint32_t new_cap = viewport->mesh_capacity * 2;
      struct MopMesh *new_arr =
          realloc(viewport->meshes, new_cap * sizeof(struct MopMesh));
      if (!new_arr) {
        MOP_ERROR("mesh realloc failed (capacity %u)", new_cap);
        return NULL;
      }
      /* Zero-init the newly allocated portion */
      memset(new_arr + viewport->mesh_capacity, 0,
             (new_cap - viewport->mesh_capacity) * sizeof(struct MopMesh));
      viewport->meshes = new_arr;
      viewport->mesh_capacity = new_cap;
    }
    viewport->mesh_count++;
  }

  /* Create RHI buffers */
  MopRhiBufferDesc vb_desc = {.data = desc->vertices,
                              .size = desc->vertex_count * sizeof(MopVertex)};
  MopRhiBuffer *vb = viewport->rhi->buffer_create(viewport->device, &vb_desc);
  if (!vb) {
    return NULL;
  }

  MopRhiBufferDesc ib_desc = {.data = desc->indices,
                              .size = desc->index_count * sizeof(uint32_t)};
  MopRhiBuffer *ib = viewport->rhi->buffer_create(viewport->device, &ib_desc);
  if (!ib) {
    viewport->rhi->buffer_destroy(viewport->device, vb);
    return NULL;
  }

  /* Compute an average base color from vertex colors */
  MopColor avg = {0.0f, 0.0f, 0.0f, 1.0f};
  for (uint32_t i = 0; i < desc->vertex_count; i++) {
    avg.r += desc->vertices[i].color.r;
    avg.g += desc->vertices[i].color.g;
    avg.b += desc->vertices[i].color.b;
  }
  float inv = 1.0f / (float)desc->vertex_count;
  avg.r *= inv;
  avg.g *= inv;
  avg.b *= inv;

  struct MopMesh *mesh = &viewport->meshes[slot];
  mesh->vertex_buffer = vb;
  mesh->index_buffer = ib;
  mesh->vertex_count = desc->vertex_count;
  mesh->index_count = desc->index_count;
  mesh->object_id = desc->object_id;
  mesh->transform = mop_mat4_identity();
  mesh->base_color = avg;
  mesh->opacity = 1.0f;
  mesh->active = true;
  mesh->position = (MopVec3){0, 0, 0};
  mesh->rotation = (MopVec3){0, 0, 0};
  mesh->scale_val = (MopVec3){1, 1, 1};
  mesh->use_trs = true;
  mesh->parent_index = -1;
  mesh->world_transform = mop_mat4_identity();
  mesh->texture = NULL;
  mesh->has_material = false;
  mesh->material = mop_material_default();
  mesh->blend_mode = MOP_BLEND_OPAQUE;
  mesh->shading_mode_override = -1;
  mesh->vertex_capacity = desc->vertex_count;
  mesh->index_capacity = desc->index_count;

  return mesh;
}

MopMesh *mop_viewport_add_mesh_ex(MopViewport *viewport,
                                  const MopMeshDescEx *desc) {
  if (!viewport || !desc || !desc->vertex_data || !desc->indices ||
      !desc->vertex_format) {
    MOP_ERROR("invalid extended mesh descriptor");
    return NULL;
  }
  if (desc->vertex_count == 0 || desc->index_count == 0) {
    MOP_ERROR("mesh has zero vertices or indices");
    return NULL;
  }
  if (desc->index_count % 3 != 0) {
    MOP_ERROR("index count %u is not a multiple of 3", desc->index_count);
    return NULL;
  }

  /* Find a free slot */
  uint32_t slot = viewport->mesh_count;
  for (uint32_t i = 0; i < viewport->mesh_count; i++) {
    if (!viewport->meshes[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == viewport->mesh_count) {
    if (viewport->mesh_count >= viewport->mesh_capacity) {
      if (viewport->mesh_capacity > UINT32_MAX / 2) {
        MOP_ERROR("mesh array capacity overflow");
        return NULL;
      }
      uint32_t new_cap = viewport->mesh_capacity * 2;
      struct MopMesh *new_arr =
          realloc(viewport->meshes, new_cap * sizeof(struct MopMesh));
      if (!new_arr) {
        MOP_ERROR("mesh realloc failed (capacity %u)", new_cap);
        return NULL;
      }
      memset(new_arr + viewport->mesh_capacity, 0,
             (new_cap - viewport->mesh_capacity) * sizeof(struct MopMesh));
      viewport->meshes = new_arr;
      viewport->mesh_capacity = new_cap;
    }
    viewport->mesh_count++;
  }

  /* Create RHI buffers — raw bytes */
  size_t vb_size = (size_t)desc->vertex_count * desc->vertex_format->stride;
  MopRhiBufferDesc vb_desc = {.data = desc->vertex_data, .size = vb_size};
  MopRhiBuffer *vb = viewport->rhi->buffer_create(viewport->device, &vb_desc);
  if (!vb)
    return NULL;

  MopRhiBufferDesc ib_desc = {.data = desc->indices,
                              .size = desc->index_count * sizeof(uint32_t)};
  MopRhiBuffer *ib = viewport->rhi->buffer_create(viewport->device, &ib_desc);
  if (!ib) {
    viewport->rhi->buffer_destroy(viewport->device, vb);
    return NULL;
  }

  /* Try to extract a base color from the COLOR attribute */
  MopColor avg = {0.8f, 0.8f, 0.8f, 1.0f};
  const MopVertexAttrib *color_attr =
      mop_vertex_format_find(desc->vertex_format, MOP_ATTRIB_COLOR);
  if (color_attr && color_attr->format == MOP_FORMAT_FLOAT4) {
    float r_sum = 0, g_sum = 0, b_sum = 0;
    const uint8_t *raw = (const uint8_t *)desc->vertex_data;
    for (uint32_t i = 0; i < desc->vertex_count; i++) {
      const float *c =
          (const float *)(raw + (size_t)i * desc->vertex_format->stride +
                          color_attr->offset);
      r_sum += c[0];
      g_sum += c[1];
      b_sum += c[2];
    }
    float inv = 1.0f / (float)desc->vertex_count;
    avg = (MopColor){r_sum * inv, g_sum * inv, b_sum * inv, 1.0f};
  }

  /* Allocate and copy vertex format */
  MopVertexFormat *fmt_copy = malloc(sizeof(MopVertexFormat));
  if (!fmt_copy) {
    viewport->rhi->buffer_destroy(viewport->device, ib);
    viewport->rhi->buffer_destroy(viewport->device, vb);
    return NULL;
  }
  *fmt_copy = *desc->vertex_format;

  struct MopMesh *mesh = &viewport->meshes[slot];
  mesh->vertex_buffer = vb;
  mesh->index_buffer = ib;
  mesh->vertex_count = desc->vertex_count;
  mesh->index_count = desc->index_count;
  mesh->object_id = desc->object_id;
  mesh->transform = mop_mat4_identity();
  mesh->base_color = avg;
  mesh->opacity = 1.0f;
  mesh->active = true;
  mesh->position = (MopVec3){0, 0, 0};
  mesh->rotation = (MopVec3){0, 0, 0};
  mesh->scale_val = (MopVec3){1, 1, 1};
  mesh->use_trs = true;
  mesh->parent_index = -1;
  mesh->world_transform = mop_mat4_identity();
  mesh->texture = NULL;
  mesh->has_material = false;
  mesh->material = mop_material_default();
  mesh->blend_mode = MOP_BLEND_OPAQUE;
  mesh->shading_mode_override = -1;
  mesh->vertex_capacity = desc->vertex_count;
  mesh->index_capacity = desc->index_count;
  mesh->vertex_format = fmt_copy;

  return mesh;
}

void mop_viewport_remove_mesh(MopViewport *viewport, MopMesh *mesh) {
  if (!viewport || !mesh)
    return;

  if (mesh->vertex_buffer) {
    viewport->rhi->buffer_destroy(viewport->device, mesh->vertex_buffer);
    mesh->vertex_buffer = NULL;
  }
  if (mesh->index_buffer) {
    viewport->rhi->buffer_destroy(viewport->device, mesh->index_buffer);
    mesh->index_buffer = NULL;
  }
  free(mesh->vertex_format);
  mesh->vertex_format = NULL;
  free(mesh->bind_pose_data);
  mesh->bind_pose_data = NULL;
  free(mesh->bone_matrices);
  mesh->bone_matrices = NULL;
  free(mesh->bone_parents);
  mesh->bone_parents = NULL;
  mesh->bone_count = 0;
  free(mesh->morph_targets);
  mesh->morph_targets = NULL;
  free(mesh->morph_weights);
  mesh->morph_weights = NULL;
  mesh->morph_target_count = 0;

  /* Destroy LOD buffers (Phase 9C) */
  for (uint32_t li = 0; li < mesh->lod_level_count; li++) {
    if (mesh->lod_levels[li].vertex_buffer)
      viewport->rhi->buffer_destroy(viewport->device,
                                    mesh->lod_levels[li].vertex_buffer);
    if (mesh->lod_levels[li].index_buffer)
      viewport->rhi->buffer_destroy(viewport->device,
                                    mesh->lod_levels[li].index_buffer);
  }
  mesh->lod_level_count = 0;
  mesh->active_lod = 0;

  mesh->active = false;
}

void mop_mesh_update_geometry(MopMesh *mesh, MopViewport *viewport,
                              const MopVertex *vertices, uint32_t vertex_count,
                              const uint32_t *indices, uint32_t index_count) {
  if (!mesh || !viewport || !vertices || !indices)
    return;
  if (vertex_count == 0 || index_count == 0)
    return;
  if (vertex_count > 16 * 1024 * 1024) { /* 16M vertex limit */
    MOP_ERROR("vertex count exceeds maximum (%u)", vertex_count);
    return;
  }

  /* --- Vertex buffer --- */
  if (vertex_count <= mesh->vertex_capacity) {
    /* Fast path: update in-place */
    viewport->rhi->buffer_update(viewport->device, mesh->vertex_buffer,
                                 vertices, 0, vertex_count * sizeof(MopVertex));
  } else {
    /* Grow: destroy old, create new with 2x capacity */
    viewport->rhi->buffer_destroy(viewport->device, mesh->vertex_buffer);
    uint32_t new_cap = mesh->vertex_capacity;
    while (new_cap < vertex_count)
      new_cap = new_cap ? new_cap * 2 : 64;

    void *tmp = calloc(new_cap, sizeof(MopVertex));
    if (!tmp)
      return;
    memcpy(tmp, vertices, vertex_count * sizeof(MopVertex));
    MopRhiBufferDesc vb_desc = {.data = tmp,
                                .size = new_cap * sizeof(MopVertex)};
    mesh->vertex_buffer =
        viewport->rhi->buffer_create(viewport->device, &vb_desc);
    free(tmp);
    if (!mesh->vertex_buffer) {
      mesh->active = false;
      return;
    }
    mesh->vertex_capacity = new_cap;
  }
  mesh->vertex_count = vertex_count;
  mesh->aabb_valid = false; /* vertex data changed, invalidate AABB cache */

  /* --- Index buffer --- */
  if (index_count <= mesh->index_capacity) {
    viewport->rhi->buffer_update(viewport->device, mesh->index_buffer, indices,
                                 0, index_count * sizeof(uint32_t));
  } else {
    viewport->rhi->buffer_destroy(viewport->device, mesh->index_buffer);
    uint32_t new_cap = mesh->index_capacity;
    while (new_cap < index_count)
      new_cap = new_cap ? new_cap * 2 : 64;

    void *tmp = calloc(new_cap, sizeof(uint32_t));
    if (!tmp)
      return;
    memcpy(tmp, indices, index_count * sizeof(uint32_t));
    MopRhiBufferDesc ib_desc = {.data = tmp,
                                .size = new_cap * sizeof(uint32_t)};
    mesh->index_buffer =
        viewport->rhi->buffer_create(viewport->device, &ib_desc);
    free(tmp);
    if (!mesh->index_buffer) {
      mesh->active = false;
      return;
    }
    mesh->index_capacity = new_cap;
  }
  mesh->index_count = index_count;
}

/* -------------------------------------------------------------------------
 * Skeletal skinning — set bone matrices and apply CPU-side deformation
 * ------------------------------------------------------------------------- */

void mop_mesh_set_bone_matrices(MopMesh *mesh, MopViewport *viewport,
                                const MopMat4 *matrices, uint32_t bone_count) {
  if (!mesh || !viewport || !matrices || bone_count == 0)
    return;

  /* Vertex format must include joints + weights */
  const MopVertexFormat *fmt = mesh->vertex_format;
  if (!fmt)
    return; /* standard MopVertex has no joints/weights */
  const MopVertexAttrib *joints_attr =
      mop_vertex_format_find(fmt, MOP_ATTRIB_JOINTS);
  const MopVertexAttrib *weights_attr =
      mop_vertex_format_find(fmt, MOP_ATTRIB_WEIGHTS);
  if (!joints_attr || !weights_attr)
    return;

  /* On first call, snapshot the bind-pose vertex data */
  if (!mesh->bind_pose_data) {
    size_t vb_size = (size_t)mesh->vertex_count * fmt->stride;
    const void *raw = viewport->rhi->buffer_read(mesh->vertex_buffer);
    if (!raw)
      return;
    mesh->bind_pose_data = malloc(vb_size);
    if (!mesh->bind_pose_data)
      return;
    memcpy(mesh->bind_pose_data, raw, vb_size);
  }

  /* Copy bone matrices */
  if (bone_count != mesh->bone_count) {
    free(mesh->bone_matrices);
    mesh->bone_matrices = malloc(bone_count * sizeof(MopMat4));
    if (!mesh->bone_matrices) {
      mesh->bone_count = 0;
      return;
    }
    mesh->bone_count = bone_count;
  }
  memcpy(mesh->bone_matrices, matrices, bone_count * sizeof(MopMat4));
  mesh->skin_dirty = true;
}

/* Apply CPU skinning for a single mesh.
 * Reads joints/weights from bind_pose_data, transforms positions and normals
 * using bone_matrices, and writes to a temporary buffer that is uploaded
 * to the vertex buffer. */
static void mop_skin_apply(MopMesh *mesh, MopViewport *viewport) {
  if (!mesh->skin_dirty || !mesh->bind_pose_data || mesh->bone_count == 0)
    return;
  mesh->skin_dirty = false;

  const MopVertexFormat *fmt = mesh->vertex_format;
  if (!fmt)
    return;

  const MopVertexAttrib *pos_attr =
      mop_vertex_format_find(fmt, MOP_ATTRIB_POSITION);
  const MopVertexAttrib *norm_attr =
      mop_vertex_format_find(fmt, MOP_ATTRIB_NORMAL);
  const MopVertexAttrib *joints_attr =
      mop_vertex_format_find(fmt, MOP_ATTRIB_JOINTS);
  const MopVertexAttrib *weights_attr =
      mop_vertex_format_find(fmt, MOP_ATTRIB_WEIGHTS);
  if (!pos_attr || !joints_attr || !weights_attr)
    return;

  size_t vb_size = (size_t)mesh->vertex_count * fmt->stride;
  uint8_t *deformed = malloc(vb_size);
  if (!deformed)
    return;
  memcpy(deformed, mesh->bind_pose_data, vb_size);

  const uint8_t *src = (const uint8_t *)mesh->bind_pose_data;

  for (uint32_t v = 0; v < mesh->vertex_count; v++) {
    const uint8_t *vert = src + (size_t)v * fmt->stride;
    uint8_t *out = deformed + (size_t)v * fmt->stride;

    /* Read joint indices (ubyte4) */
    const uint8_t *jdata = vert + joints_attr->offset;
    uint8_t j0 = jdata[0], j1 = jdata[1], j2 = jdata[2], j3 = jdata[3];

    /* Read weights (float4) */
    const float *wdata = (const float *)(vert + weights_attr->offset);
    float w0 = wdata[0], w1 = wdata[1], w2 = wdata[2], w3 = wdata[3];

    /* Read bind-pose position (float3) */
    const float *pdata = (const float *)(vert + pos_attr->offset);
    float px = pdata[0], py = pdata[1], pz = pdata[2];

    /* Compute skinned position: sum(w[i] * M[j[i]] * pos) */
    float sx = 0, sy = 0, sz = 0;
    const struct {
      uint8_t j;
      float w;
    } bones[4] = {{j0, w0}, {j1, w1}, {j2, w2}, {j3, w3}};
    for (int b = 0; b < 4; b++) {
      if (bones[b].w <= 0.0f)
        continue;
      uint32_t bi = bones[b].j;
      if (bi >= mesh->bone_count)
        continue;
      const float *m = mesh->bone_matrices[bi].d;
      /* Column-major: result = M * vec4(pos, 1) */
      float rx = m[0] * px + m[4] * py + m[8] * pz + m[12];
      float ry = m[1] * px + m[5] * py + m[9] * pz + m[13];
      float rz = m[2] * px + m[6] * py + m[10] * pz + m[14];
      sx += bones[b].w * rx;
      sy += bones[b].w * ry;
      sz += bones[b].w * rz;
    }
    float *opos = (float *)(out + pos_attr->offset);
    opos[0] = sx;
    opos[1] = sy;
    opos[2] = sz;

    /* Skin normal (if present) — use upper-left 3x3 (no translation) */
    if (norm_attr) {
      const float *ndata = (const float *)(vert + norm_attr->offset);
      float nx = ndata[0], ny = ndata[1], nz = ndata[2];
      float snx = 0, sny = 0, snz = 0;
      for (int b = 0; b < 4; b++) {
        if (bones[b].w <= 0.0f)
          continue;
        uint32_t bi = bones[b].j;
        if (bi >= mesh->bone_count)
          continue;
        const float *m = mesh->bone_matrices[bi].d;
        float rx = m[0] * nx + m[4] * ny + m[8] * nz;
        float ry = m[1] * nx + m[5] * ny + m[9] * nz;
        float rz = m[2] * nx + m[6] * ny + m[10] * nz;
        snx += bones[b].w * rx;
        sny += bones[b].w * ry;
        snz += bones[b].w * rz;
      }
      /* Normalize */
      float len = sqrtf(snx * snx + sny * sny + snz * snz);
      if (len > 1e-8f) {
        snx /= len;
        sny /= len;
        snz /= len;
      }
      float *onorm = (float *)(out + norm_attr->offset);
      onorm[0] = snx;
      onorm[1] = sny;
      onorm[2] = snz;
    }
  }

  /* Upload deformed data to vertex buffer */
  viewport->rhi->buffer_update(viewport->device, mesh->vertex_buffer, deformed,
                               0, vb_size);
  free(deformed);
  mesh->aabb_valid = false;
}

/* -------------------------------------------------------------------------
 * Bone hierarchy (for skeleton visualization)
 * ------------------------------------------------------------------------- */

void mop_mesh_set_bone_hierarchy(MopMesh *mesh, const int32_t *parent_indices,
                                 uint32_t bone_count) {
  if (!mesh || !parent_indices || bone_count == 0)
    return;
  if (bone_count != mesh->bone_count)
    return; /* must match existing bone_count from set_bone_matrices */

  free(mesh->bone_parents);
  mesh->bone_parents = malloc(bone_count * sizeof(int32_t));
  if (!mesh->bone_parents)
    return;
  memcpy(mesh->bone_parents, parent_indices, bone_count * sizeof(int32_t));
}

/* -------------------------------------------------------------------------
 * Morph targets / blend shapes
 * ------------------------------------------------------------------------- */

void mop_mesh_set_morph_targets(MopMesh *mesh, MopViewport *viewport,
                                const float *targets, const float *weights,
                                uint32_t target_count) {
  if (!mesh || !viewport || !targets || !weights || target_count == 0)
    return;
  if (mesh->vertex_count == 0)
    return;

  /* Snapshot bind-pose if not already done (for skinned meshes it's already
   * captured; for morph-only meshes we need it now). */
  const MopVertexFormat *fmt = mesh->vertex_format;
  size_t stride = fmt ? fmt->stride : sizeof(MopVertex);
  if (!mesh->bind_pose_data) {
    size_t vb_size = (size_t)mesh->vertex_count * stride;
    const void *raw = viewport->rhi->buffer_read(mesh->vertex_buffer);
    if (!raw)
      return;
    mesh->bind_pose_data = malloc(vb_size);
    if (!mesh->bind_pose_data)
      return;
    memcpy(mesh->bind_pose_data, raw, vb_size);
  }

  /* Copy morph target deltas */
  size_t targets_size =
      (size_t)target_count * mesh->vertex_count * 3 * sizeof(float);
  free(mesh->morph_targets);
  mesh->morph_targets = malloc(targets_size);
  if (!mesh->morph_targets)
    return;
  memcpy(mesh->morph_targets, targets, targets_size);

  /* Copy weights */
  free(mesh->morph_weights);
  mesh->morph_weights = malloc(target_count * sizeof(float));
  if (!mesh->morph_weights) {
    free(mesh->morph_targets);
    mesh->morph_targets = NULL;
    return;
  }
  memcpy(mesh->morph_weights, weights, target_count * sizeof(float));
  mesh->morph_target_count = target_count;
  mesh->morph_dirty = true;
}

void mop_mesh_set_morph_weights(MopMesh *mesh, const float *weights,
                                uint32_t target_count) {
  if (!mesh || !weights || target_count == 0)
    return;
  if (target_count != mesh->morph_target_count || !mesh->morph_weights)
    return;
  memcpy(mesh->morph_weights, weights, target_count * sizeof(float));
  mesh->morph_dirty = true;
}

/* Apply CPU morph target blending for a single mesh.
 * Reads base positions from bind_pose_data, adds weighted deltas,
 * and uploads the result to the vertex buffer. */
static void mop_morph_apply(MopMesh *mesh, MopViewport *viewport) {
  if (!mesh->morph_dirty || !mesh->bind_pose_data ||
      mesh->morph_target_count == 0 || !mesh->morph_targets)
    return;
  mesh->morph_dirty = false;

  const MopVertexFormat *fmt = mesh->vertex_format;
  size_t stride = fmt ? fmt->stride : sizeof(MopVertex);
  size_t pos_off = 0; /* position is always at offset 0 in MopVertex */
  if (fmt) {
    const MopVertexAttrib *pos_attr =
        mop_vertex_format_find(fmt, MOP_ATTRIB_POSITION);
    if (!pos_attr)
      return;
    pos_off = pos_attr->offset;
  }

  size_t vb_size = (size_t)mesh->vertex_count * stride;
  uint8_t *deformed = malloc(vb_size);
  if (!deformed)
    return;
  memcpy(deformed, mesh->bind_pose_data, vb_size);

  uint32_t vc = mesh->vertex_count;
  for (uint32_t t = 0; t < mesh->morph_target_count; t++) {
    float w = mesh->morph_weights[t];
    if (w == 0.0f)
      continue;
    const float *deltas = mesh->morph_targets + (size_t)t * vc * 3;
    for (uint32_t v = 0; v < vc; v++) {
      float *pos = (float *)(deformed + v * stride + pos_off);
      pos[0] += w * deltas[v * 3 + 0];
      pos[1] += w * deltas[v * 3 + 1];
      pos[2] += w * deltas[v * 3 + 2];
    }
  }

  viewport->rhi->buffer_update(viewport->device, mesh->vertex_buffer, deformed,
                               0, vb_size);
  free(deformed);
  mesh->aabb_valid = false;
}

void mop_mesh_set_transform(MopMesh *mesh, const MopMat4 *transform) {
  if (!mesh || !transform)
    return;
  mesh->transform = *transform;
  mesh->use_trs = false; /* explicit matrix overrides TRS */
}

void mop_mesh_set_opacity(MopMesh *mesh, float opacity) {
  if (!mesh)
    return;
  mesh->opacity = (opacity < 0.0f) ? 0.0f : (opacity > 1.0f) ? 1.0f : opacity;
}

void mop_mesh_set_blend_mode(MopMesh *mesh, MopBlendMode mode) {
  if (!mesh)
    return;
  mesh->blend_mode = mode;
}

/* -------------------------------------------------------------------------
 * Texture management
 * ------------------------------------------------------------------------- */

MopTexture *mop_viewport_create_texture(MopViewport *viewport, int width,
                                        int height, const uint8_t *rgba_data) {
  if (!viewport || width <= 0 || height <= 0 || !rgba_data)
    return NULL;

  MopRhiTexture *rhi_tex =
      viewport->rhi->texture_create(viewport->device, width, height, rgba_data);
  if (!rhi_tex)
    return NULL;

  MopTexture *tex = malloc(sizeof(MopTexture));
  if (!tex) {
    viewport->rhi->texture_destroy(viewport->device, rhi_tex);
    return NULL;
  }
  tex->rhi_texture = rhi_tex;
  return tex;
}

void mop_viewport_destroy_texture(MopViewport *viewport, MopTexture *texture) {
  if (!viewport || !texture)
    return;
  viewport->rhi->texture_destroy(viewport->device, texture->rhi_texture);
  free(texture);
}

void mop_mesh_set_texture(MopMesh *mesh, MopTexture *texture) {
  if (!mesh)
    return;
  mesh->texture = texture;
}

/* -------------------------------------------------------------------------
 * Material system
 * ------------------------------------------------------------------------- */

MopMaterial mop_material_default(void) {
  MopMaterial m;
  m.base_color = (MopColor){1.0f, 1.0f, 1.0f, 1.0f};
  m.metallic = 0.0f;
  m.roughness = 0.5f;
  m.emissive = (MopVec3){0.0f, 0.0f, 0.0f};
  m.albedo_map = NULL;
  m.normal_map = NULL;
  return m;
}

void mop_mesh_set_material(MopMesh *mesh, const MopMaterial *material) {
  if (!mesh || !material)
    return;
  mesh->material = *material;
  mesh->has_material = true;
}

/* -------------------------------------------------------------------------
 * Per-mesh TRS accessors
 * ------------------------------------------------------------------------- */

void mop_mesh_set_position(MopMesh *mesh, MopVec3 position) {
  if (!mesh)
    return;
  mesh->position = position;
  mesh->use_trs = true;
}

void mop_mesh_set_rotation(MopMesh *mesh, MopVec3 rotation) {
  if (!mesh)
    return;
  mesh->rotation = rotation;
  mesh->use_trs = true;
}

void mop_mesh_set_scale(MopMesh *mesh, MopVec3 scale) {
  if (!mesh)
    return;
  mesh->scale_val = scale;
  mesh->use_trs = true;
}

MopVec3 mop_mesh_get_position(const MopMesh *mesh) {
  return mesh ? mesh->position : (MopVec3){0, 0, 0};
}

MopVec3 mop_mesh_get_rotation(const MopMesh *mesh) {
  return mesh ? mesh->rotation : (MopVec3){0, 0, 0};
}

MopVec3 mop_mesh_get_scale(const MopMesh *mesh) {
  return mesh ? mesh->scale_val : (MopVec3){1, 1, 1};
}

void mop_mesh_set_shading(MopMesh *mesh, MopShadingMode mode) {
  if (!mesh)
    return;
  mesh->shading_mode_override = (int)mode;
}

/* -------------------------------------------------------------------------
 * Rendering — internal pass functions
 *
 * The render loop is decomposed into discrete passes.  Each pass handles
 * one rendering concern.  The pass list is fixed (not user-configurable).
 * ------------------------------------------------------------------------- */

/* Forward declaration for built-in overlay dispatch */
void mop_overlay_builtin_wireframe(MopViewport *vp, void *user_data);
void mop_overlay_builtin_normals(MopViewport *vp, void *user_data);
void mop_overlay_builtin_bounds(MopViewport *vp, void *user_data);
void mop_overlay_builtin_selection(MopViewport *vp, void *user_data);
void mop_overlay_builtin_outline(MopViewport *vp, void *user_data);
void mop_overlay_builtin_skeleton(MopViewport *vp, void *user_data);
void mop_overlay_builtin_grid(MopViewport *vp, void *user_data);
void mop_overlay_builtin_light_indicators(MopViewport *vp, void *user_data);
void mop_overlay_builtin_camera_objects(MopViewport *vp, void *user_data);
void mop_overlay_builtin_gizmo_2d(MopViewport *vp, void *user_data);
void mop_overlay_builtin_axis_indicator_2d(MopViewport *vp, void *user_data);

/* Per-frame counters — accumulated across passes */
static uint32_t s_triangle_count;
static uint32_t s_draw_call_count;
static uint32_t s_vertex_count;
static uint32_t s_lod_transitions;

/* Forward declaration for LOD selection (defined after mop_viewport_set_chrome)
 */
static uint32_t lod_select(const MopMesh *mesh, float projected_diameter,
                           float lod_bias);

/* ---- Helper macro: issue a draw call for a mesh ---- */
/* Chrome meshes (grid = MOP_GRID_ID, >= 0xFFFE0000 for gizmo/indicators)
 * are rendered fully unlit (ambient=1, no lights) so they keep their vertex
 * colors without being darkened or brightened by scene lighting. */
#define EMIT_DRAW(vp, mesh_ptr)                                                \
  do {                                                                         \
    struct MopMesh *m_ = (mesh_ptr);                                           \
    /* LOD selection (Phase 9C): use active LOD's buffers if available */      \
    MopRhiBuffer *vb_ = m_->vertex_buffer;                                     \
    MopRhiBuffer *ib_ = m_->index_buffer;                                      \
    uint32_t vcnt_ = m_->vertex_count;                                         \
    uint32_t icnt_ = m_->index_count;                                          \
    if (m_->active_lod > 0 && m_->active_lod <= m_->lod_level_count) {         \
      uint32_t li_ = m_->active_lod - 1;                                       \
      if (m_->lod_levels[li_].vertex_buffer) {                                 \
        vb_ = m_->lod_levels[li_].vertex_buffer;                               \
        ib_ = m_->lod_levels[li_].index_buffer;                                \
        vcnt_ = m_->lod_levels[li_].vertex_count;                              \
        icnt_ = m_->lod_levels[li_].index_count;                               \
      }                                                                        \
    }                                                                          \
    s_triangle_count += icnt_ / 3;                                             \
    s_vertex_count += vcnt_;                                                   \
    s_draw_call_count++;                                                       \
    bool chrome_ = (m_->object_id >= 0xFFFD0000u);                             \
    MopMat4 mvp_ = mop_mat4_multiply(                                          \
        (vp)->projection_matrix,                                               \
        mop_mat4_multiply((vp)->view_matrix, m_->world_transform));            \
    MopRhiDrawCall call_ = {                                                   \
        .vertex_buffer = vb_,                                                  \
        .index_buffer = ib_,                                                   \
        .vertex_count = vcnt_,                                                 \
        .index_count = icnt_,                                                  \
        .object_id = m_->object_id,                                            \
        .model = m_->world_transform,                                          \
        .view = (vp)->view_matrix,                                             \
        .projection = (vp)->projection_matrix,                                 \
        .mvp = mvp_,                                                           \
        .base_color = m_->base_color,                                          \
        .opacity = m_->opacity,                                                \
        .light_dir = (vp)->light_dir,                                          \
        .ambient = chrome_ ? 1.0f : (vp)->ambient,                             \
        .shading_mode = (m_->shading_mode_override >= 0                        \
                             ? (MopShadingMode)m_->shading_mode_override       \
                             : (vp)->shading_mode),                            \
        .wireframe =                                                           \
            ((vp)->render_mode == MOP_RENDER_WIREFRAME) && m_->object_id != 0, \
        .depth_test = (m_->object_id < 0xFFFD0000u),                           \
        .depth_write = true,                                                   \
        .backface_cull = (m_->object_id < 0xFFFD0000u),                        \
        .texture = (m_->has_material && m_->material.albedo_map)               \
                       ? m_->material.albedo_map->rhi_texture                  \
                       : (m_->texture ? m_->texture->rhi_texture : NULL),      \
        .normal_map = (m_->has_material && m_->material.normal_map)            \
                          ? m_->material.normal_map->rhi_texture               \
                          : NULL,                                              \
        .metallic_roughness_map =                                              \
            (m_->has_material && m_->material.metallic_roughness_map)          \
                ? m_->material.metallic_roughness_map->rhi_texture             \
                : NULL,                                                        \
        .ao_map = (m_->has_material && m_->material.ao_map)                    \
                      ? m_->material.ao_map->rhi_texture                       \
                      : NULL,                                                  \
        .blend_mode = m_->blend_mode,                                          \
        .metallic = m_->has_material ? m_->material.metallic : 0.0f,           \
        .roughness = m_->has_material ? m_->material.roughness : 0.5f,         \
        .emissive =                                                            \
            m_->has_material ? m_->material.emissive : (MopVec3){0, 0, 0},     \
        .lights = chrome_ ? NULL : (vp)->lights,                               \
        .light_count = chrome_ ? 0 : (vp)->light_count,                        \
        .cam_eye = (vp)->cam_eye,                                              \
        .vertex_format = m_->vertex_format,                                    \
        .aabb_min = m_->aabb_valid ? m_->aabb_local.min : (MopVec3){0, 0, 0},  \
        .aabb_max = m_->aabb_valid ? m_->aabb_local.max : (MopVec3){0, 0, 0},  \
    };                                                                         \
    (vp)->rhi->draw((vp)->device, (vp)->framebuffer, &call_);                  \
  } while (0)

/* ---- Pass: gradient background ---- */

/* Declared in environment.c — sampling functions for CPU skybox + IBL */
void mop_env_sample_irradiance(const MopViewport *vp, MopVec3 normal,
                               float out[3]);
void mop_env_sample_prefiltered(const MopViewport *vp, MopVec3 reflect_dir,
                                float roughness, float out[3]);
void mop_env_sample_brdf_lut(const MopViewport *vp, float ndotv,
                             float roughness, float *scale, float *bias);

/* Bilinear sample from RGBA float image (local helper for skybox) */
static void env_sample_bilinear(const float *data, int w, int h, float u,
                                float v, float out[4]) {
  u = u - floorf(u);
  v = v - floorf(v);
  float fx = u * (float)(w - 1);
  float fy = v * (float)(h - 1);
  int x0 = (int)fx;
  int y0 = (int)fy;
  int x1 = (x0 + 1) % w;
  int y1 = y0 + 1;
  if (y1 >= h)
    y1 = h - 1;
  float sx = fx - (float)x0;
  float sy = fy - (float)y0;
  const float *p00 = &data[((size_t)y0 * w + x0) * 4];
  const float *p10 = &data[((size_t)y0 * w + x1) * 4];
  const float *p01 = &data[((size_t)y1 * w + x0) * 4];
  const float *p11 = &data[((size_t)y1 * w + x1) * 4];
  for (int c = 0; c < 4; c++) {
    float top = p00[c] * (1.0f - sx) + p10[c] * sx;
    float bot = p01[c] * (1.0f - sx) + p11[c] * sx;
    out[c] = top * (1.0f - sy) + bot * sy;
  }
}

/* CPU skybox: renders equirectangular HDR environment to color_hdr buffer.
 * Writes to all pixels (as background — depth test happens later). */
static void pass_background_hdri_cpu(MopViewport *vp) {
  MopSwFramebuffer *fb = (MopSwFramebuffer *)vp->framebuffer;
  if (!fb->color_hdr || !vp->env_hdr_data)
    return;

  int sw = fb->width, sh = fb->height;
  float fov = vp->cam_fov_radians;
  float aspect = (float)sw / (float)sh;
  float half_h = tanf(fov * 0.5f);
  float half_w = half_h * aspect;

  /* Camera basis vectors */
  MopVec3 fwd = mop_vec3_normalize(mop_vec3_sub(vp->cam_target, vp->cam_eye));
  MopVec3 right = mop_vec3_normalize(mop_vec3_cross(fwd, vp->cam_up));
  MopVec3 up = mop_vec3_cross(right, fwd);

  float rotation = vp->env_rotation;
  float intensity = vp->env_intensity;
  const float pi = 3.14159265358979f;

  for (int y = 0; y < sh; y++) {
    float ndc_y = 1.0f - 2.0f * ((float)y + 0.5f) / (float)sh;
    for (int x = 0; x < sw; x++) {
      float ndc_x = 2.0f * ((float)x + 0.5f) / (float)sw - 1.0f;

      /* Ray direction through pixel */
      float dx = fwd.x + right.x * ndc_x * half_w + up.x * ndc_y * half_h;
      float dy = fwd.y + right.y * ndc_x * half_w + up.y * ndc_y * half_h;
      float dz = fwd.z + right.z * ndc_x * half_w + up.z * ndc_y * half_h;
      float len = sqrtf(dx * dx + dy * dy + dz * dz);
      dx /= len;
      dy /= len;
      dz /= len;

      /* Equirectangular UV */
      float phi_val = atan2f(dz, dx) - rotation;
      float clamped_dy = dy < -1.0f ? -1.0f : (dy > 1.0f ? 1.0f : dy);
      float theta_val = asinf(clamped_dy);
      float eu = phi_val / (2.0f * pi) + 0.5f;
      float ev = theta_val / pi + 0.5f;

      float sample[4];
      env_sample_bilinear(vp->env_hdr_data, vp->env_width, vp->env_height, eu,
                          ev, sample);

      size_t idx = ((size_t)y * sw + x) * 4;
      fb->color_hdr[idx + 0] = sample[0] * intensity;
      fb->color_hdr[idx + 1] = sample[1] * intensity;
      fb->color_hdr[idx + 2] = sample[2] * intensity;
      fb->color_hdr[idx + 3] = 1.0f;
    }
  }
}

static void pass_background(MopViewport *vp) {
  /* HDRI/procedural sky as skybox */
  if ((vp->env_type == MOP_ENV_HDRI ||
       vp->env_type == MOP_ENV_PROCEDURAL_SKY) &&
      vp->env_hdr_data && vp->show_env_background) {
    if (vp->backend_type == MOP_BACKEND_CPU) {
      pass_background_hdri_cpu(vp);
      return;
    }
    /* GPU skybox: draw env map as background */
    if (vp->rhi->draw_skybox && vp->env_texture) {
      MopMat4 vp_mat =
          mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);
      MopMat4 inv_vp = mop_mat4_inverse(vp_mat);
      float cam_pos[3] = {vp->cam_eye.x, vp->cam_eye.y, vp->cam_eye.z};
      vp->rhi->draw_skybox(vp->device, vp->framebuffer, vp->env_texture,
                           inv_vp.d, cam_pos, vp->env_rotation,
                           vp->env_intensity * vp->exposure);
      return;
    }
  }

  /* GPU backends: skip gradient quad draw — the PBR bindless shader adds
   * IBL reflections to it, contaminating the background with env map colors.
   * The clear color already provides the background; the tonemap shader
   * preserves it via the alpha<0.5 bypass path. */
  if (vp->backend_type != MOP_BACKEND_CPU)
    return;

  if (!vp->bg_vb || !vp->bg_ib)
    return;

  MopMat4 identity = mop_mat4_identity();
  MopRhiDrawCall bg_call = {
      .vertex_buffer = vp->bg_vb,
      .index_buffer = vp->bg_ib,
      .vertex_count = 4,
      .index_count = 6,
      .object_id = 0,
      .model = identity,
      .view = identity,
      .projection = identity,
      .mvp = identity,
      .base_color = (MopColor){1, 1, 1, 1},
      .opacity = 1.0f,
      .light_dir = vp->light_dir,
      .ambient = 1.0f,
      .shading_mode = MOP_SHADING_SMOOTH,
      .wireframe = false,
      .depth_test = false,
      .backface_cull = false,
      .texture = NULL,
      .blend_mode = MOP_BLEND_OPAQUE,
      .metallic = 0.0f,
      .roughness = 0.5f,
      .emissive = (MopVec3){0, 0, 0},
      .lights = NULL,
      .light_count = 0,
      .vertex_format = NULL,
  };
  vp->rhi->draw(vp->device, vp->framebuffer, &bg_call);
}

/* ---- Pass: shadow map from directional light ---- */
#define MOP_SHADOW_MAP_SIZE 1024

static void pass_shadow(MopViewport *vp) {
  /* Find first active directional light */
  int dir_light_idx = -1;
  for (uint32_t i = 0; i < vp->light_count; i++) {
    if (vp->lights[i].active && vp->lights[i].type == MOP_LIGHT_DIRECTIONAL) {
      dir_light_idx = (int)i;
      break;
    }
  }
  if (dir_light_idx < 0) {
    vp->shadow_fb_valid = false;
    mop_sw_shadow_clear();
    return;
  }

  /* Compute scene AABB from active meshes */
  float scene_min[3] = {1e9f, 1e9f, 1e9f};
  float scene_max[3] = {-1e9f, -1e9f, -1e9f};
  bool has_meshes = false;

  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *mesh = &vp->meshes[i];
    if (!mesh->active || mesh->object_id >= 0xFFFD0000u)
      continue;
    if (mesh->blend_mode != MOP_BLEND_OPAQUE)
      continue;

    /* Use the world transform's translation as a rough center */
    float cx = mesh->world_transform.d[12];
    float cy = mesh->world_transform.d[13];
    float cz = mesh->world_transform.d[14];

    /* Estimate radius from scale */
    float sx = sqrtf(mesh->world_transform.d[0] * mesh->world_transform.d[0] +
                     mesh->world_transform.d[1] * mesh->world_transform.d[1] +
                     mesh->world_transform.d[2] * mesh->world_transform.d[2]);
    float sy = sqrtf(mesh->world_transform.d[4] * mesh->world_transform.d[4] +
                     mesh->world_transform.d[5] * mesh->world_transform.d[5] +
                     mesh->world_transform.d[6] * mesh->world_transform.d[6]);
    float sz = sqrtf(mesh->world_transform.d[8] * mesh->world_transform.d[8] +
                     mesh->world_transform.d[9] * mesh->world_transform.d[9] +
                     mesh->world_transform.d[10] * mesh->world_transform.d[10]);
    float r = sx > sy ? sx : sy;
    if (sz > r)
      r = sz;
    r *= 1.5f; /* margin */

    if (cx - r < scene_min[0])
      scene_min[0] = cx - r;
    if (cy - r < scene_min[1])
      scene_min[1] = cy - r;
    if (cz - r < scene_min[2])
      scene_min[2] = cz - r;
    if (cx + r > scene_max[0])
      scene_max[0] = cx + r;
    if (cy + r > scene_max[1])
      scene_max[1] = cy + r;
    if (cz + r > scene_max[2])
      scene_max[2] = cz + r;
    has_meshes = true;
  }

  if (!has_meshes) {
    vp->shadow_fb_valid = false;
    mop_sw_shadow_clear();
    return;
  }

  /* Allocate or resize shadow framebuffer */
  if (vp->shadow_fb.width != MOP_SHADOW_MAP_SIZE ||
      vp->shadow_fb.height != MOP_SHADOW_MAP_SIZE) {
    mop_sw_framebuffer_free(&vp->shadow_fb);
    if (!mop_sw_framebuffer_alloc(&vp->shadow_fb, MOP_SHADOW_MAP_SIZE,
                                  MOP_SHADOW_MAP_SIZE)) {
      vp->shadow_fb_valid = false;
      mop_sw_shadow_clear();
      return;
    }
  }

  /* Clear shadow depth to 1.0 */
  size_t shadow_pixels = (size_t)MOP_SHADOW_MAP_SIZE * MOP_SHADOW_MAP_SIZE;
  for (size_t p = 0; p < shadow_pixels; p++)
    vp->shadow_fb.depth[p] = 1.0f;

  /* Light direction: the "direction" field is where the light shines.
   * The shadow camera looks along this direction (eye is positioned
   * opposite to it, behind the scene). */
  MopVec3 light_dir = mop_vec3_normalize(vp->lights[dir_light_idx].direction);
  MopVec3 neg_dir = light_dir;

  /* Scene center and radius */
  float center_x = (scene_min[0] + scene_max[0]) * 0.5f;
  float center_y = (scene_min[1] + scene_max[1]) * 0.5f;
  float center_z = (scene_min[2] + scene_max[2]) * 0.5f;
  float dx = scene_max[0] - scene_min[0];
  float dy = scene_max[1] - scene_min[1];
  float dz = scene_max[2] - scene_min[2];
  float scene_radius = sqrtf(dx * dx + dy * dy + dz * dz) * 0.5f;
  if (scene_radius < 1.0f)
    scene_radius = 1.0f;

  /* Light view: position the light far back along its direction */
  MopVec3 light_eye = {center_x - neg_dir.x * scene_radius * 2.0f,
                       center_y - neg_dir.y * scene_radius * 2.0f,
                       center_z - neg_dir.z * scene_radius * 2.0f};
  MopVec3 light_target = {center_x, center_y, center_z};

  /* Choose a stable up vector (avoid collinear with light direction) */
  MopVec3 up = {0, 1, 0};
  float dot_up = fabsf(mop_vec3_dot(neg_dir, up));
  if (dot_up > 0.99f)
    up = (MopVec3){1, 0, 0};

  MopMat4 light_view = mop_mat4_look_at(light_eye, light_target, up);

  /* Orthographic projection covering the scene */
  float ortho_size = scene_radius * 1.2f;
  float ortho_near = 0.1f;
  float ortho_far = scene_radius * 4.0f;
  MopMat4 light_proj = mop_mat4_ortho(-ortho_size, ortho_size, -ortho_size,
                                      ortho_size, ortho_near, ortho_far);

  MopMat4 light_vp = mop_mat4_multiply(light_proj, light_view);

  /* Render each opaque mesh into the shadow depth buffer */
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *mesh = &vp->meshes[i];
    if (!mesh->active || mesh->object_id >= 0xFFFD0000u)
      continue;
    if (mesh->blend_mode != MOP_BLEND_OPAQUE)
      continue;

    /* Read vertex data from the RHI buffer */
    const MopVertex *verts =
        (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
    const uint32_t *indices =
        (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
    if (!verts || !indices)
      continue;

    mop_sw_shadow_render_mesh(verts, mesh->vertex_count, indices,
                              mesh->index_count, mesh->world_transform,
                              light_vp, &vp->shadow_fb);
  }

  /* Set shadow state for the main render */
  mop_sw_shadow_set(vp->shadow_fb.depth, MOP_SHADOW_MAP_SIZE,
                    MOP_SHADOW_MAP_SIZE, light_vp);
  vp->shadow_fb_valid = true;
}

/* ---- Pass: ground grid ----
 * Grid rendering is handled entirely by the post-frame_end analytical
 * overlay (mop_overlay_builtin_grid).  No geometry pass needed. */

/* ---- Pass: opaque scene meshes ---- */
static void pass_scene_opaque(MopViewport *vp) {
  MopFrustum frustum = mop_viewport_get_frustum(vp);
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *mesh = &vp->meshes[i];
    if (!mesh->active)
      continue;
    if (mesh->object_id == MOP_GRID_ID)
      continue; /* grid drawn separately after scene */
    if (mesh->blend_mode != MOP_BLEND_OPAQUE)
      continue;
    if (mesh->object_id >= 0xFFFE0000u)
      continue;
    /* Frustum cull: skip meshes entirely outside the view frustum */
    MopAABB world_aabb = mop_mesh_get_aabb_world(mesh, vp);
    if (mop_frustum_test_aabb(&frustum, world_aabb) == -1)
      continue;
    EMIT_DRAW(vp, mesh);
  }
}

/* ---- Pass: transparent scene meshes (back-to-front) ---- */
static void pass_scene_transparent(MopViewport *vp) {
  MopFrustum frustum = mop_viewport_get_frustum(vp);
  uint32_t trans_count = 0;
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *mesh = &vp->meshes[i];
    if (!mesh->active || mesh->blend_mode == MOP_BLEND_OPAQUE)
      continue;
    if (mesh->object_id >= 0xFFFE0000u)
      continue;
    /* Frustum cull transparent meshes too */
    MopAABB world_aabb = mop_mesh_get_aabb_world(mesh, vp);
    if (mop_frustum_test_aabb(&frustum, world_aabb) == -1)
      continue;
    trans_count++;
  }
  if (trans_count == 0)
    return;

  /* Grow persistent sort arrays if needed */
  if (trans_count > vp->trans_sort_capacity) {
    uint32_t new_cap = trans_count + (trans_count >> 1); /* 1.5x growth */
    uint32_t *new_idx = realloc(vp->trans_sort_idx, new_cap * sizeof(uint32_t));
    float *new_dist = realloc(vp->trans_sort_dist, new_cap * sizeof(float));
    if (!new_idx || !new_dist) {
      free(new_idx);
      free(new_dist);
      vp->trans_sort_idx = NULL;
      vp->trans_sort_dist = NULL;
      vp->trans_sort_capacity = 0;
      MOP_WARN("transparent sort allocation failed, rendering unsorted");
      for (uint32_t i = 0; i < vp->mesh_count; i++) {
        struct MopMesh *mesh = &vp->meshes[i];
        if (!mesh->active || mesh->blend_mode == MOP_BLEND_OPAQUE)
          continue;
        if (mesh->object_id >= 0xFFFE0000u)
          continue;
        EMIT_DRAW(vp, mesh);
      }
      return;
    }
    vp->trans_sort_idx = new_idx;
    vp->trans_sort_dist = new_dist;
    vp->trans_sort_capacity = new_cap;
  }
  uint32_t *trans_idx = vp->trans_sort_idx;
  float *trans_dist = vp->trans_sort_dist;

  uint32_t ti = 0;
  MopVec3 eye = vp->cam_eye;
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *mesh = &vp->meshes[i];
    if (!mesh->active || mesh->blend_mode == MOP_BLEND_OPAQUE)
      continue;
    if (mesh->object_id >= 0xFFFE0000u)
      continue;
    MopAABB world_aabb = mop_mesh_get_aabb_world(mesh, vp);
    if (mop_frustum_test_aabb(&frustum, world_aabb) == -1)
      continue;
    trans_idx[ti] = i;
    float dx = mesh->world_transform.d[12] - eye.x;
    float dy = mesh->world_transform.d[13] - eye.y;
    float dz = mesh->world_transform.d[14] - eye.z;
    trans_dist[ti] = dx * dx + dy * dy + dz * dz;
    ti++;
  }

  /* Insertion sort (back-to-front = farthest first) */
  for (uint32_t j = 1; j < trans_count; j++) {
    uint32_t key_i = trans_idx[j];
    float key_d = trans_dist[j];
    int k = (int)j - 1;
    while (k >= 0 && trans_dist[k] < key_d) {
      trans_idx[k + 1] = trans_idx[k];
      trans_dist[k + 1] = trans_dist[k];
      k--;
    }
    trans_idx[k + 1] = key_i;
    trans_dist[k + 1] = key_d;
  }

  for (uint32_t j = 0; j < trans_count; j++) {
    EMIT_DRAW(vp, &vp->meshes[trans_idx[j]]);
  }
}

/* ---- Pass: gizmo overlays + light indicators ---- */
static void pass_gizmo(MopViewport *vp) {
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *mesh = &vp->meshes[i];
    if (!mesh->active)
      continue;
    if (mesh->object_id < 0xFFFE0000u)
      continue; /* light indicators + gizmo handles */
    if (mesh->opacity < 0.01f)
      continue; /* invisible chrome — 2D overlay + screen-space picking */
    EMIT_DRAW(vp, mesh);
  }
}

/* ---- Pass: overlays (built-in + custom callbacks) ---- */
static void pass_overlays(MopViewport *vp) {
  /* Built-in overlays driven by display settings */
  if (vp->display.wireframe_overlay &&
      vp->overlay_enabled[MOP_OVERLAY_WIREFRAME]) {
    mop_overlay_builtin_wireframe(vp, NULL);
  }
  if (vp->display.show_normals && vp->overlay_enabled[MOP_OVERLAY_NORMALS]) {
    mop_overlay_builtin_normals(vp, NULL);
  }
  if (vp->display.show_bounds && vp->overlay_enabled[MOP_OVERLAY_BOUNDS]) {
    mop_overlay_builtin_bounds(vp, NULL);
  }
  if (vp->overlay_enabled[MOP_OVERLAY_SELECTION]) {
    mop_overlay_builtin_selection(vp, NULL);
  }
  if (vp->overlay_enabled[MOP_OVERLAY_SKELETON]) {
    mop_overlay_builtin_skeleton(vp, NULL);
  }
  /* NOTE: MOP_OVERLAY_OUTLINE runs as a post-process after frame_end,
   * since it needs the object_id readback buffer (populated by frame_end
   * on GPU backends). See mop_viewport_render(). */

  /* Custom overlays */
  for (uint32_t i = MOP_OVERLAY_BUILTIN_COUNT; i < vp->overlay_count; i++) {
    if (vp->overlays[i].active && vp->overlay_enabled[i] &&
        vp->overlays[i].draw_fn) {
      vp->overlays[i].draw_fn(vp, vp->overlays[i].user_data);
    }
  }
}

/* ---- Pass: instanced meshes ---- */
static void pass_instanced(MopViewport *vp) {
  for (uint32_t i = 0; i < vp->instanced_count; i++) {
    struct MopInstancedMesh *im = &vp->instanced_meshes[i];
    if (!im->active || im->instance_count == 0)
      continue;

    s_triangle_count += (im->index_count / 3) * im->instance_count;

    MopRhiDrawCall inst_call = {
        .vertex_buffer = im->vertex_buffer,
        .index_buffer = im->index_buffer,
        .vertex_count = im->vertex_count,
        .index_count = im->index_count,
        .object_id = im->object_id,
        .model = mop_mat4_identity(),
        .view = vp->view_matrix,
        .projection = vp->projection_matrix,
        .mvp = mop_mat4_identity(),
        .base_color = im->base_color,
        .opacity = im->opacity,
        .light_dir = vp->light_dir,
        .ambient = vp->ambient,
        .shading_mode = vp->shading_mode,
        .wireframe =
            (vp->render_mode == MOP_RENDER_WIREFRAME) && im->object_id != 0,
        .depth_test = true,
        .backface_cull = (im->object_id != 0),
        .texture = im->texture ? im->texture->rhi_texture : NULL,
        .blend_mode = im->blend_mode,
        .metallic = im->has_material ? im->material.metallic : 0.0f,
        .roughness = im->has_material ? im->material.roughness : 0.5f,
        .emissive =
            im->has_material ? im->material.emissive : (MopVec3){0, 0, 0},
        .lights = vp->lights,
        .light_count = vp->light_count,
        .vertex_format = NULL,
    };

    vp->rhi->draw_instanced(vp->device, vp->framebuffer, &inst_call,
                            im->transforms, im->instance_count);
  }
}

#undef EMIT_DRAW

/* =========================================================================
 * Render graph node callbacks
 *
 * Each callback wraps an existing pass or extracted inline block.
 * Signature: void(MopViewport *vp, void *user_data)
 * ========================================================================= */

/* Wrapper macro: adapt existing (MopViewport*) pass to graph node signature */
#define RG_WRAP(name, fn)                                                      \
  static void name(MopViewport *vp, void *ud) {                                \
    (void)ud;                                                                  \
    fn(vp);                                                                    \
  }

RG_WRAP(rg_background, pass_background)
RG_WRAP(rg_shadow, pass_shadow)
RG_WRAP(rg_opaque, pass_scene_opaque)
RG_WRAP(rg_transparent, pass_scene_transparent)
RG_WRAP(rg_instanced, pass_instanced)
RG_WRAP(rg_overlays, pass_overlays)
RG_WRAP(rg_gizmo, pass_gizmo)

#undef RG_WRAP

/* Hook dispatch node: user_data encodes the pipeline stage */
static void rg_dispatch_hook(MopViewport *vp, void *ud) {
  dispatch_hooks(vp, (MopPipelineStage)(uintptr_t)ud);
}

static void rg_shader_plugins(MopViewport *vp, void *ud) {
  mop_shader_plugins_dispatch(vp, (MopShaderPluginStage)(uintptr_t)ud);
}

/* Frame begin: clear + set initial exposure */
static void rg_frame_begin(MopViewport *vp, void *ud) {
  (void)ud;
  vp->rhi->frame_begin(vp->device, vp->framebuffer, vp->clear_color);
  if (vp->rhi->set_exposure)
    vp->rhi->set_exposure(vp->device, vp->exposure);
}

/* IBL texture bindings for GPU + CPU backends */
static void rg_ibl_setup(MopViewport *vp, void *ud) {
  (void)ud;
  if (vp->rhi->set_ibl_textures)
    vp->rhi->set_ibl_textures(vp->device, vp->env_irradiance,
                              vp->env_prefiltered, vp->env_brdf_lut);

  if (vp->backend_type == MOP_BACKEND_CPU && vp->env_irradiance_data) {
    mop_sw_ibl_set(vp->env_irradiance_data, vp->env_irradiance_w,
                   vp->env_irradiance_h, vp->env_prefiltered_data,
                   vp->env_prefiltered_w, vp->env_prefiltered_h,
                   vp->env_prefiltered_levels, vp->env_brdf_lut_data,
                   256, /* BRDF_LUT_SIZE */
                   vp->env_rotation, vp->env_intensity);
  }
}

/* Frame end: exposure → submit → HDR resolve (CPU) */
static void rg_frame_end(MopViewport *vp, void *ud) {
  (void)ud;
  if (vp->rhi->set_exposure)
    vp->rhi->set_exposure(vp->device, vp->exposure);

  /* Pass TAA matrices to backend before frame_end */
  if ((vp->post_effects & MOP_POST_TAA) && vp->rhi->set_taa_params) {
    MopMat4 vp_jittered =
        mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);
    MopMat4 inv_vp_jittered = mop_mat4_inverse(vp_jittered);
    MopMat4 prev_vp = mop_mat4_multiply(vp->taa_prev_proj, vp->taa_prev_view);
    vp->rhi->set_taa_params(vp->device, inv_vp_jittered.d, prev_vp.d,
                            vp->taa_jitter_x, vp->taa_jitter_y,
                            !vp->taa_has_history);
  }

  vp->rhi->frame_end(vp->device, vp->framebuffer);

  if (vp->backend_type == MOP_BACKEND_CPU) {
    MopSwFramebuffer *sw_fb = (MopSwFramebuffer *)vp->framebuffer;
    mop_sw_hdr_resolve(sw_fb, vp->exposure);
  }
}

/* Post-frame overlays: outline, grid, SDF indicators, draw_overlays */
static void rg_post_frame_overlays(MopViewport *vp, void *ud) {
  (void)ud;

  /* Outline (reads object-ID buffer) */
  if (vp->overlay_enabled[MOP_OVERLAY_OUTLINE])
    mop_overlay_builtin_outline(vp, NULL);

  /* Grid: GPU shader path or CPU fallback */
  bool gpu_grid = (vp->backend_type != MOP_BACKEND_CPU);
  if (vp->show_chrome && !gpu_grid)
    mop_overlay_builtin_grid(vp, NULL);

  /* Reset overlay command buffer for this frame */
  vp->overlay_prim_count = 0;

  /* Push-based overlays (gizmo, lights, cameras, axis indicator) */
  if (vp->show_chrome) {
    mop_overlay_builtin_light_indicators(vp, NULL);

    for (uint32_t ci = 0; ci < vp->camera_count; ci++) {
      struct MopCameraObject *cam = &vp->cameras[ci];
      if (cam->active && cam->icon_mesh)
        cam->position = cam->icon_mesh->position;
    }
    mop_overlay_builtin_camera_objects(vp, NULL);
    mop_overlay_builtin_gizmo_2d(vp, NULL);
    mop_overlay_builtin_axis_indicator_2d(vp, NULL);
  }

  /* Compute GPU grid params if needed */
  MopGridParams grid_params;
  MopGridParams *gp = NULL;
  if (vp->show_chrome && gpu_grid) {
    MopMat4 VPm = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);

    float H[9];
    H[0] = VPm.d[0];
    H[1] = VPm.d[8];
    H[2] = VPm.d[12];
    H[3] = VPm.d[1];
    H[4] = VPm.d[9];
    H[5] = VPm.d[13];
    H[6] = VPm.d[3];
    H[7] = VPm.d[11];
    H[8] = VPm.d[15];

    float det = H[0] * (H[4] * H[8] - H[5] * H[7]) -
                H[1] * (H[3] * H[8] - H[5] * H[6]) +
                H[2] * (H[3] * H[7] - H[4] * H[6]);
    if (fabsf(det) > 1e-12f) {
      float idet = 1.0f / det;
      grid_params.Hi[0] = (H[4] * H[8] - H[5] * H[7]) * idet;
      grid_params.Hi[1] = (H[2] * H[7] - H[1] * H[8]) * idet;
      grid_params.Hi[2] = (H[1] * H[5] - H[2] * H[4]) * idet;
      grid_params.Hi[3] = (H[5] * H[6] - H[3] * H[8]) * idet;
      grid_params.Hi[4] = (H[0] * H[8] - H[2] * H[6]) * idet;
      grid_params.Hi[5] = (H[2] * H[3] - H[0] * H[5]) * idet;
      grid_params.Hi[6] = (H[3] * H[7] - H[4] * H[6]) * idet;
      grid_params.Hi[7] = (H[1] * H[6] - H[0] * H[7]) * idet;
      grid_params.Hi[8] = (H[0] * H[4] - H[1] * H[3]) * idet;

      grid_params.vp_z0 = VPm.d[2];
      grid_params.vp_z2 = VPm.d[10];
      grid_params.vp_z3 = VPm.d[14];
      grid_params.vp_w0 = VPm.d[3];
      grid_params.vp_w2 = VPm.d[11];
      grid_params.vp_w3 = VPm.d[15];
      grid_params.grid_half = 8.0f;
      grid_params.reverse_z = vp->reverse_z;
      grid_params.minor_color = vp->theme.grid_minor;
      grid_params.major_color = vp->theme.grid_major;
      grid_params.axis_x_color = vp->theme.grid_axis_x;
      grid_params.axis_z_color = vp->theme.grid_axis_z;
      grid_params.axis_half_width = vp->theme.grid_line_width_axis * 0.5f;
      gp = &grid_params;
    }
  }

  /* Dispatch accumulated overlay primitives to backend */
  if ((vp->overlay_prim_count > 0 || gp) && vp->rhi->draw_overlays) {
    int w = vp->width * vp->ssaa_factor;
    int h = vp->height * vp->ssaa_factor;
    vp->rhi->draw_overlays(vp->device, vp->framebuffer, vp->overlay_prims,
                           vp->overlay_prim_count, gp, w, h);
  }
}

/* Submit — finalize and submit the GPU command buffer after overlays */
static void rg_frame_submit(MopViewport *vp, void *ud) {
  (void)ud;
  if (vp->rhi->frame_submit)
    vp->rhi->frame_submit(vp->device, vp->framebuffer);
}

/* Cleanup: clear shadow/IBL state, run post-render subsystems */
static void rg_cleanup(MopViewport *vp, void *ud) {
  (void)ud;
  if (vp->backend_type == MOP_BACKEND_CPU) {
    mop_sw_shadow_clear();
    mop_sw_ibl_clear();
  }
  mop_subsystem_dispatch(&vp->subsystems, MOP_SUBSYS_PHASE_POST_RENDER, vp,
                         0.0f, vp->last_frame_time);
}

/* -------------------------------------------------------------------------
 * Render graph builder — convenience for adding passes with resource decls
 * ------------------------------------------------------------------------- */

/* Helper: add pass with reads and writes arrays */
static void rg_add(MopRenderGraph *rg, const char *name, MopRgExecuteFn fn,
                   void *ud, const MopRgResourceId *reads, uint32_t rc,
                   const MopRgResourceId *writes, uint32_t wc) {
  MopRgPass p = {.name = name,
                 .execute = fn,
                 .user_data = ud,
                 .read_count = rc,
                 .write_count = wc};
  for (uint32_t i = 0; i < rc && i < MOP_RG_MAX_PASS_RESOURCES; i++)
    p.reads[i] = reads[i];
  for (uint32_t i = 0; i < wc && i < MOP_RG_MAX_PASS_RESOURCES; i++)
    p.writes[i] = writes[i];
  mop_rg_add_pass(rg, &p);
}

/* Shorthand: pass with no resource declarations */
static void rg_add_simple(MopRenderGraph *rg, const char *name,
                          MopRgExecuteFn fn, void *ud) {
  rg_add(rg, name, fn, ud, NULL, 0, NULL, 0);
}

/* -------------------------------------------------------------------------
 * Main render entry point
 * ------------------------------------------------------------------------- */

MopRenderResult mop_viewport_render(MopViewport *viewport) {
  if (!viewport)
    return MOP_RENDER_ERROR;

  double t_frame_start = mop_profile_now_ms();

  /* --- PRE_RENDER hooks + frame callback --- */
  dispatch_hooks(viewport, MOP_STAGE_PRE_RENDER);
  if (viewport->frame_cb)
    viewport->frame_cb(viewport, true, viewport->frame_cb_data);

  /* Tick camera inertia (smooth orbit/zoom deceleration) */
  {
    float dt = viewport->last_frame_time - viewport->prev_frame_time;
    if (dt > 0.0f && dt < 0.5f)
      mop_orbit_camera_tick(&viewport->camera, dt);
    viewport->prev_frame_time = viewport->last_frame_time;
  }

  /* Apply owned camera each frame */
  mop_orbit_camera_apply(&viewport->camera, viewport);

  /* Update geometry light indicators for picking (object_id buffer).
   * Visual rendering is handled by 2D overlay after frame_end.
   * Set geometry indicator opacity to 0 so they're invisible but
   * still rasterized (writes to object_id buffer for click detection). */
  if (viewport->show_chrome) {
    mop_light_update_indicators(viewport);
    for (uint32_t i = 0; i < viewport->light_count; i++) {
      if (viewport->light_indicators[i]) {
        viewport->light_indicators[i]->opacity = 0.0f;
        viewport->light_indicators[i]->blend_mode = MOP_BLEND_ALPHA;
      }
    }
  }

  /* Refresh gizmo scale for current camera distance.
   * Set handle geometry opacity to 0 — invisible but still rasterized
   * (writes object_id for picking).  2D overlay provides the visuals. */
  mop_gizmo_update(viewport->gizmo);
  mop_gizmo_set_handles_opacity(viewport->gizmo, 0.0f);

  /* --- Transform phase (TRS + hierarchical world transforms) --- */
  double t_transform_start = mop_profile_now_ms();
  s_triangle_count = 0;
  s_draw_call_count = 0;
  s_vertex_count = 0;
  s_lod_transitions = 0;

  /* Compute local transforms for all active meshes */
  for (uint32_t i = 0; i < viewport->mesh_count; i++) {
    struct MopMesh *mesh = &viewport->meshes[i];
    if (!mesh->active)
      continue;
    if (mesh->use_trs) {
      mesh->transform =
          mop_mat4_compose_trs(mesh->position, mesh->rotation, mesh->scale_val);
    }
  }

  /* Compute world_transform (roots first, then children) */
  for (uint32_t i = 0; i < viewport->mesh_count; i++) {
    struct MopMesh *mesh = &viewport->meshes[i];
    if (!mesh->active)
      continue;
    if (mesh->parent_index == -1) {
      mesh->world_transform = mesh->transform;
    }
  }
  for (int pass = 0; pass < 16; pass++) {
    bool changed = false;
    for (uint32_t i = 0; i < viewport->mesh_count; i++) {
      struct MopMesh *mesh = &viewport->meshes[i];
      if (!mesh->active || mesh->parent_index < 0)
        continue;
      uint32_t pi = (uint32_t)mesh->parent_index;
      if (pi < viewport->mesh_count && viewport->meshes[pi].active) {
        MopMat4 new_world = mop_mat4_multiply(
            viewport->meshes[pi].world_transform, mesh->transform);
        if (new_world.d[0] != mesh->world_transform.d[0] ||
            new_world.d[12] != mesh->world_transform.d[12]) {
          changed = true;
        }
        mesh->world_transform = new_world;
      }
    }
    if (!changed)
      break;
  }

  /* LOD selection (Phase 9C) — compute projected diameter and select LOD */
  {
    float half_h = (float)viewport->height * 0.5f;
    MopMat4 vp_mat =
        mop_mat4_multiply(viewport->projection_matrix, viewport->view_matrix);
    for (uint32_t i = 0; i < viewport->mesh_count; i++) {
      struct MopMesh *mesh = &viewport->meshes[i];
      if (!mesh->active || mesh->lod_level_count == 0)
        continue;

      /* Compute projected diameter from AABB */
      float radius = 1.0f;
      if (mesh->aabb_valid) {
        MopVec3 ext = {mesh->aabb_local.max.x - mesh->aabb_local.min.x,
                       mesh->aabb_local.max.y - mesh->aabb_local.min.y,
                       mesh->aabb_local.max.z - mesh->aabb_local.min.z};
        radius = 0.5f * sqrtf(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z);
      }

      /* World-space center */
      MopVec3 center = {mesh->world_transform.d[12],
                        mesh->world_transform.d[13],
                        mesh->world_transform.d[14]};

      /* Project to clip space — only need w for perspective divide */
      float cw = vp_mat.d[3] * center.x + vp_mat.d[7] * center.y +
                 vp_mat.d[11] * center.z + vp_mat.d[15];

      float projected_diameter = 0.0f;
      if (cw > 0.001f)
        projected_diameter = (radius / cw) * half_h * 2.0f;
      else
        projected_diameter = 1e6f; /* very close — use highest detail */

      mesh->prev_lod = mesh->active_lod;
      mesh->active_lod =
          lod_select(mesh, projected_diameter, viewport->lod_bias);
      if (mesh->active_lod != mesh->prev_lod)
        s_lod_transitions++;
    }
  }

  double t_transform_end = mop_profile_now_ms();

  /* --- Simulation update (water, particles, etc. via subsystem dispatch) ---
   * Must run BEFORE frame_begin so that Vulkan buffer updates (one-shot
   * command buffers) complete before the main render command buffer reads
   * the vertex data inside the render pass. */
  mop_subsystem_dispatch(&viewport->subsystems, MOP_SUBSYS_PHASE_SIMULATE,
                         viewport, 0.0f, viewport->last_frame_time);

  /* --- Apply CPU morph blending + skinning for dirty meshes --- */
  for (uint32_t i = 0; i < viewport->mesh_count; i++) {
    MopMesh *m = &viewport->meshes[i];
    if (!m->active)
      continue;
    /* Morph blending runs first (modifies bind-pose → vertex buffer),
     * then skinning transforms the result further if applicable. */
    if (m->morph_dirty)
      mop_morph_apply(m, viewport);
    if (m->skin_dirty)
      mop_skin_apply(m, viewport);
  }

  /* === TAA: apply sub-pixel jitter to projection matrix === */
  MopMat4 unjittered_proj = viewport->projection_matrix;
  bool taa_enabled = (viewport->post_effects & MOP_POST_TAA) != 0;
  if (taa_enabled) {
    /* Compute Halton(2,3) jitter in [-0.5, +0.5] pixels */
    uint32_t fi = (viewport->taa_frame_index % 16) + 1; /* 1-based, 16 phases */
    viewport->taa_jitter_x = mop_halton(fi, 2) - 0.5f;
    viewport->taa_jitter_y = mop_halton(fi, 3) - 0.5f;

    /* Apply jitter as sub-pixel translation in clip space:
     * proj[2][0] += jitter_x * 2 / render_width
     * proj[2][1] += jitter_y * 2 / render_height
     * Column-major: d[col*4+row], so col=2,row=0 → d[8], col=2,row=1 → d[9] */
    int rw = viewport->width * viewport->ssaa_factor;
    int rh = viewport->height * viewport->ssaa_factor;
    viewport->projection_matrix.d[8] +=
        viewport->taa_jitter_x * 2.0f / (float)rw;
    viewport->projection_matrix.d[9] +=
        viewport->taa_jitter_y * 2.0f / (float)rh;
  }

  /* === Build render graph for this frame === */
  double t_rasterize_start = mop_profile_now_ms();
  MopRenderGraph rg;
  mop_rg_clear(&rg);

  /* clang-format off */
  static const MopRgResourceId w_clear[] = {MOP_RG_RES_COLOR_HDR, MOP_RG_RES_DEPTH, MOP_RG_RES_PICK};
  static const MopRgResourceId w_color[] = {MOP_RG_RES_COLOR_HDR};
  static const MopRgResourceId w_shadow[] = {MOP_RG_RES_SHADOW_MAP};
  static const MopRgResourceId w_scene[] = {MOP_RG_RES_COLOR_HDR, MOP_RG_RES_DEPTH, MOP_RG_RES_PICK};
  static const MopRgResourceId r_shadow[] = {MOP_RG_RES_SHADOW_MAP};
  static const MopRgResourceId r_depth[] = {MOP_RG_RES_DEPTH};
  static const MopRgResourceId r_hdr[] = {MOP_RG_RES_COLOR_HDR};
  static const MopRgResourceId w_ldr[] = {MOP_RG_RES_LDR_COLOR};
  static const MopRgResourceId rw_overlay[] = {MOP_RG_RES_LDR_COLOR, MOP_RG_RES_OVERLAY_BUF};
  /* clang-format on */

  rg_add(&rg, "frame_begin", rg_frame_begin, NULL, NULL, 0, w_clear, 3);

  rg_add_simple(&rg, "hook:post_clear", rg_dispatch_hook,
                (void *)(uintptr_t)MOP_STAGE_POST_CLEAR);

  if (viewport->show_chrome)
    rg_add(&rg, "background", rg_background, NULL, NULL, 0, w_color, 1);

  rg_add_simple(&rg, "hook:pre_scene", rg_dispatch_hook,
                (void *)(uintptr_t)MOP_STAGE_PRE_SCENE);

  if (viewport->backend_type == MOP_BACKEND_CPU)
    rg_add(&rg, "shadow", rg_shadow, NULL, NULL, 0, w_shadow, 1);

  rg_add_simple(&rg, "ibl_setup", rg_ibl_setup, NULL);

  rg_add(&rg, "opaque", rg_opaque, NULL, r_shadow, 1, w_scene, 3);

  rg_add_simple(&rg, "hook:post_opaque", rg_dispatch_hook,
                (void *)(uintptr_t)MOP_STAGE_POST_OPAQUE);

  rg_add(&rg, "plugins:post_opaque", rg_shader_plugins,
         (void *)(uintptr_t)MOP_SHADER_PLUGIN_POST_OPAQUE, r_depth, 1, w_scene,
         3);

  rg_add(&rg, "transparent", rg_transparent, NULL, r_depth, 1, w_scene, 3);

  rg_add(&rg, "instanced", rg_instanced, NULL, r_depth, 1, w_scene, 3);

  rg_add_simple(&rg, "hook:post_scene", rg_dispatch_hook,
                (void *)(uintptr_t)MOP_STAGE_POST_SCENE);

  rg_add(&rg, "plugins:post_scene", rg_shader_plugins,
         (void *)(uintptr_t)MOP_SHADER_PLUGIN_POST_SCENE, r_depth, 1, w_scene,
         3);

  rg_add(&rg, "overlays", rg_overlays, NULL, NULL, 0, w_color, 1);

  if (viewport->show_chrome)
    rg_add(&rg, "gizmo", rg_gizmo, NULL, r_depth, 1, w_scene, 3);

  rg_add(&rg, "plugins:overlay", rg_shader_plugins,
         (void *)(uintptr_t)MOP_SHADER_PLUGIN_OVERLAY, r_depth, 1, w_color, 1);

  rg_add_simple(&rg, "hook:post_overlay", rg_dispatch_hook,
                (void *)(uintptr_t)MOP_STAGE_POST_OVERLAY);

  rg_add(&rg, "frame_end", rg_frame_end, NULL, r_hdr, 1, w_ldr, 1);

  rg_add(&rg, "plugins:post_process", rg_shader_plugins,
         (void *)(uintptr_t)MOP_SHADER_PLUGIN_POST_PROCESS, r_hdr, 1, w_ldr, 1);

  rg_add(&rg, "post_overlays", rg_post_frame_overlays, NULL, rw_overlay, 2,
         rw_overlay, 2);

  rg_add_simple(&rg, "frame_submit", rg_frame_submit, NULL);

  rg_add_simple(&rg, "cleanup", rg_cleanup, NULL);

  /* === Compile + Execute === */
  mop_rg_compile(&rg);
  mop_rg_execute_mt(&rg, viewport, viewport->thread_pool);

  /* === TAA: save current matrices for next frame, restore unjittered proj ===
   */
  if (taa_enabled) {
    viewport->taa_prev_view = viewport->view_matrix;
    viewport->taa_prev_proj = unjittered_proj;
    viewport->projection_matrix =
        unjittered_proj; /* restore for picking etc. */
    viewport->taa_frame_index++;
    viewport->taa_has_history = true;
  }

  double t_rasterize_end = mop_profile_now_ms();

  /* --- POST_RENDER hooks + frame callback --- */
  dispatch_hooks(viewport, MOP_STAGE_POST_RENDER);
  if (viewport->frame_cb)
    viewport->frame_cb(viewport, false, viewport->frame_cb_data);

  double t_frame_end = mop_profile_now_ms();

  /* Store profiling stats */
  viewport->last_stats = (MopFrameStats){
      .frame_time_ms = t_frame_end - t_frame_start,
      .clear_ms = 0.0, /* absorbed into graph execution */
      .transform_ms = t_transform_end - t_transform_start,
      .rasterize_ms = t_rasterize_end - t_rasterize_start,
      .triangle_count = s_triangle_count,
      .pixel_count = (uint32_t)(viewport->width * viewport->height),
      .draw_call_count = s_draw_call_count,
      .vertex_count = s_vertex_count,
      .lod_transitions = s_lod_transitions,
      .gpu_frame_ms = viewport->rhi->frame_gpu_time_ms
                          ? viewport->rhi->frame_gpu_time_ms(viewport->device)
                          : 0.0,
  };

  viewport->last_render_result = MOP_RENDER_OK;
  viewport->last_render_error[0] = '\0';
  return MOP_RENDER_OK;
}

const char *mop_viewport_get_last_error(const MopViewport *viewport) {
  if (!viewport || viewport->last_render_error[0] == '\0')
    return NULL;
  return viewport->last_render_error;
}

/* -------------------------------------------------------------------------
 * Framebuffer readback
 * ------------------------------------------------------------------------- */

const uint8_t *mop_viewport_read_color(MopViewport *viewport, int *out_width,
                                       int *out_height) {
  if (!viewport)
    return NULL;

  int render_w = 0, render_h = 0;
  const uint8_t *src = viewport->rhi->framebuffer_read_color(
      viewport->device, viewport->framebuffer, &render_w, &render_h);
  if (!src)
    return NULL;

  int sf = viewport->ssaa_factor;
  if (sf <= 1 || !viewport->ssaa_color_buf) {
    /* No supersampling — return raw buffer */
    if (out_width)
      *out_width = render_w;
    if (out_height)
      *out_height = render_h;
    return src;
  }

  /* Box-filter downsample: average sf×sf pixel blocks */
  int pw = viewport->width;
  int ph = viewport->height;
  uint8_t *dst = viewport->ssaa_color_buf;

  for (int y = 0; y < ph; y++) {
    for (int x = 0; x < pw; x++) {
      int r = 0, g = 0, b = 0, a = 0;
      for (int dy = 0; dy < sf; dy++) {
        for (int dx = 0; dx < sf; dx++) {
          int sx = x * sf + dx;
          int sy = y * sf + dy;
          const uint8_t *p = &src[(sy * render_w + sx) * 4];
          r += p[0];
          g += p[1];
          b += p[2];
          a += p[3];
        }
      }
      int n = sf * sf;
      uint8_t *d = &dst[(y * pw + x) * 4];
      d[0] = (uint8_t)(r / n);
      d[1] = (uint8_t)(g / n);
      d[2] = (uint8_t)(b / n);
      d[3] = (uint8_t)(a / n);
    }
  }

  if (out_width)
    *out_width = pw;
  if (out_height)
    *out_height = ph;
  return dst;
}

/* -------------------------------------------------------------------------
 * Screen-space picking helpers
 *
 * Chrome elements (gizmo handles, light indicators) are rendered as 2D
 * overlays only — no geometry in the object_id buffer.  Picking is done
 * by projecting their 3D positions to screen space and computing the
 * distance from the click point to each handle/indicator.
 * ------------------------------------------------------------------------- */

/* Project world point to screen (presentation) coords.
 * Returns false if behind camera. */
static bool pick_project(MopVec3 p, const MopViewport *vp, float *sx,
                         float *sy) {
  MopMat4 vpm = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);
  MopVec4 clip = mop_mat4_mul_vec4(vpm, (MopVec4){p.x, p.y, p.z, 1.0f});
  if (clip.w <= 0.001f)
    return false;
  float nx = clip.x / clip.w;
  float ny = clip.y / clip.w;
  *sx = (nx * 0.5f + 0.5f) * (float)vp->width;
  *sy = (1.0f - (ny * 0.5f + 0.5f)) * (float)vp->height;
  return true;
}

/* Distance from point (px,py) to line segment (ax,ay)-(bx,by) */
static float dist_to_segment(float px, float py, float ax, float ay, float bx,
                             float by) {
  float dx = bx - ax, dy = by - ay;
  float len2 = dx * dx + dy * dy;
  if (len2 < 0.001f)
    return sqrtf((px - ax) * (px - ax) + (py - ay) * (py - ay));
  float t = ((px - ax) * dx + (py - ay) * dy) / len2;
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;
  float cx = ax + t * dx, cy = ay + t * dy;
  return sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

/* Screen-space gizmo pick — returns the handle object_id, or 0 */
static uint32_t pick_gizmo_screen(const MopViewport *vp, float mx, float my) {
  if (!vp->gizmo || !mop_gizmo_is_visible(vp->gizmo))
    return 0;

  MopVec3 pos = mop_gizmo_get_position_internal(vp->gizmo);
  MopGizmoMode mode = mop_gizmo_get_mode(vp->gizmo);

  float dist_cam = mop_vec3_length(mop_vec3_sub(pos, vp->cam_eye));
  float s = dist_cam * 0.18f;
  if (s < 0.05f)
    s = 0.05f;

  float threshold = 16.0f; /* pixels — widened for easier clicking */
  float best_dist = threshold;
  int best_axis = -1;

  for (int a = 0; a < 3; a++) {
    MopVec3 dir = mop_gizmo_get_axis_dir(vp->gizmo, a);

    if (mode == MOP_GIZMO_TRANSLATE || mode == MOP_GIZMO_SCALE) {
      MopVec3 shaft_s = {pos.x + dir.x * 0.20f * s, pos.y + dir.y * 0.20f * s,
                         pos.z + dir.z * 0.20f * s};
      MopVec3 shaft_e = {pos.x + dir.x * 1.20f * s, pos.y + dir.y * 1.20f * s,
                         pos.z + dir.z * 1.20f * s};
      float ss_x, ss_y, se_x, se_y;
      if (pick_project(shaft_s, vp, &ss_x, &ss_y) &&
          pick_project(shaft_e, vp, &se_x, &se_y)) {
        float d = dist_to_segment(mx, my, ss_x, ss_y, se_x, se_y);
        if (d < best_dist) {
          best_dist = d;
          best_axis = a;
        }
      }
    } else {
      /* Rotate — test distance to circle ring */
      MopVec3 up = {0, 1, 0};
      if (fabsf(mop_vec3_dot(dir, up)) > 0.99f)
        up = (MopVec3){0, 0, 1};
      MopVec3 axis_u = mop_vec3_normalize(mop_vec3_cross(up, dir));
      MopVec3 axis_v = mop_vec3_cross(dir, axis_u);
      int segs = 24;
      for (int i = 0; i < segs; i++) {
        float a0 = (float)i * 2.0f * 3.14159265f / (float)segs;
        float a1 = (float)(i + 1) * 2.0f * 3.14159265f / (float)segs;
        float ca0 = cosf(a0), sa0 = sinf(a0);
        float ca1 = cosf(a1), sa1 = sinf(a1);
        MopVec3 p0 = {pos.x + (axis_u.x * ca0 + axis_v.x * sa0) * s,
                      pos.y + (axis_u.y * ca0 + axis_v.y * sa0) * s,
                      pos.z + (axis_u.z * ca0 + axis_v.z * sa0) * s};
        MopVec3 p1 = {pos.x + (axis_u.x * ca1 + axis_v.x * sa1) * s,
                      pos.y + (axis_u.y * ca1 + axis_v.y * sa1) * s,
                      pos.z + (axis_u.z * ca1 + axis_v.z * sa1) * s};
        float s0x, s0y, s1x, s1y;
        if (pick_project(p0, vp, &s0x, &s0y) &&
            pick_project(p1, vp, &s1x, &s1y)) {
          float d = dist_to_segment(mx, my, s0x, s0y, s1x, s1y);
          if (d < best_dist) {
            best_dist = d;
            best_axis = a;
          }
        }
      }
    }
  }

  /* Center handle — small region around gizmo center */
  float csx, csy;
  if (pick_project(pos, vp, &csx, &csy)) {
    float d = sqrtf((mx - csx) * (mx - csx) + (my - csy) * (my - csy));
    if (d < 10.0f && d < best_dist) {
      best_axis = 3; /* center */
    }
  }

  if (best_axis < 0)
    return 0;

  /* Reconstruct the handle object_id that mop_gizmo_test_pick expects */
  /* Handle IDs: base + 1 + axis, where base = 0xFFFF0000 + instance*8 */
  /* For the first gizmo instance (counter=0): IDs are 0xFFFF0001..0xFFFF0004 */
  /* We use the gizmo's internal handle_ids via a new accessor */
  return mop_gizmo_get_handle_id(vp->gizmo, best_axis);
}

/* Screen-space light indicator pick — returns the light indicator object_id */
static uint32_t pick_light_screen(const MopViewport *vp, float mx, float my) {
  float threshold = 18.0f;
  float best_dist = threshold;
  uint32_t best_id = 0;

  MopMat4 vpm = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);
  (void)vpm;

  for (uint32_t i = 0; i < vp->light_count; i++) {
    if (!vp->lights[i].active)
      continue;

    MopVec3 pos;
    if (vp->lights[i].type == MOP_LIGHT_DIRECTIONAL) {
      MopVec3 dir = mop_vec3_normalize(vp->lights[i].direction);
      pos = mop_vec3_add(vp->cam_target, mop_vec3_scale(dir, 3.0f));
    } else {
      pos = vp->lights[i].position;
    }

    float sx, sy;
    if (!pick_project(pos, vp, &sx, &sy))
      continue;

    float d = sqrtf((mx - sx) * (mx - sx) + (my - sy) * (my - sy));

    /* Use a distance threshold scaled by camera distance */
    float cam_dist = mop_vec3_length(mop_vec3_sub(pos, vp->cam_eye));
    float s = cam_dist * 0.16f;
    if (s < 0.05f)
      s = 0.05f;
    float pixel_radius = s * (float)vp->width * 0.1f;
    if (pixel_radius < threshold)
      pixel_radius = threshold;

    if (d < pixel_radius && d < best_dist) {
      best_dist = d;
      /* Light indicator object_ids start at 0xFFFE0000 + light_index */
      best_id = 0xFFFE0000u + i;
    }
  }

  return best_id;
}

/* Screen-space camera object pick — returns the camera's object_id, or 0 */
static uint32_t pick_camera_screen(const MopViewport *vp, float mx, float my) {
  if (vp->camera_count == 0)
    return 0;

  float threshold = 20.0f; /* pixels — generous for 2D overlay click target */
  float best_dist = threshold;
  uint32_t best_id = 0;

  for (uint32_t i = 0; i < vp->camera_count; i++) {
    const struct MopCameraObject *cam = &vp->cameras[i];
    if (!cam->active)
      continue;
    if (vp->active_camera == cam)
      continue; /* don't pick the active camera */

    float sx, sy;
    if (!pick_project(cam->position, vp, &sx, &sy))
      continue;

    float d = sqrtf((mx - sx) * (mx - sx) + (my - sy) * (my - sy));
    if (d < best_dist) {
      best_dist = d;
      best_id = cam->object_id;
    }
  }

  return best_id;
}

/* -------------------------------------------------------------------------
 * Picking
 * ------------------------------------------------------------------------- */

MopPickResult mop_viewport_pick(MopViewport *viewport, int x, int y) {
  MopPickResult result = {.hit = false, .object_id = 0, .depth = 1.0f};

  if (!viewport)
    return result;
  if (x < 0 || x >= viewport->width || y < 0 || y >= viewport->height) {
    return result;
  }

  /* Scale from presentation coords to internal render coords */
  int sf = viewport->ssaa_factor;
  int rx = x * sf;
  int ry = y * sf;

  /* First: screen-space picking for chrome (gizmo, light indicators).
   * These have priority over scene objects — they're always "on top".
   * Skip entirely when chrome is hidden — invisible indicators must not
   * steal picks from scene objects. */
  float mx = (float)x, my = (float)y;
  if (viewport->show_chrome) {
    uint32_t chrome_id = pick_gizmo_screen(viewport, mx, my);
    if (chrome_id == 0) {
      chrome_id = pick_light_screen(viewport, mx, my);
      /* Don't let the selected light's indicator steal clicks from gizmo
       * handles. When a light is selected and the gizmo is visible, clicking
       * near the light indicator would re-select it instead of starting a
       * gizmo drag. */
      if (chrome_id == viewport->selected_id &&
          (chrome_id & 0xFFFE0000u) == 0xFFFE0000u &&
          mop_gizmo_is_visible(viewport->gizmo)) {
        chrome_id = 0;
      }
    }
    if (chrome_id != 0) {
      result.hit = true;
      result.object_id = chrome_id;
      result.depth = 0.0f; /* always on top */
      return result;
    }

    /* Screen-space camera pick — generous click target around 2D overlay */
    uint32_t cam_id = pick_camera_screen(viewport, mx, my);
    if (cam_id != 0) {
      result.hit = true;
      result.object_id = cam_id;
      result.depth = 0.0f;
      return result;
    }
  }

  /* Fallback: object_id buffer for scene objects */
  uint32_t id = viewport->rhi->pick_read_id(viewport->device,
                                            viewport->framebuffer, rx, ry);

  if (id != 0) {
    result.hit = true;
    result.object_id = id;
    result.depth = viewport->rhi->pick_read_depth(
        viewport->device, viewport->framebuffer, rx, ry);
  }

  return result;
}

/* -------------------------------------------------------------------------
 * Hierarchical transforms (Phase 4A)
 * ------------------------------------------------------------------------- */

void mop_mesh_set_parent(MopMesh *mesh, MopMesh *parent,
                         MopViewport *viewport) {
  if (!mesh || !parent || !viewport)
    return;

  /* Find the parent's index in the viewport's meshes array */
  int32_t parent_idx = -1;
  for (uint32_t i = 0; i < viewport->mesh_count; i++) {
    if (&viewport->meshes[i] == parent) {
      parent_idx = (int32_t)i;
      break;
    }
  }
  if (parent_idx < 0)
    return; /* parent not found in this viewport */

  mesh->parent_index = parent_idx;
}

void mop_mesh_clear_parent(MopMesh *mesh) {
  if (!mesh)
    return;
  mesh->parent_index = -1;
}

/* -------------------------------------------------------------------------
 * Instanced mesh API (Phase 6B)
 * ------------------------------------------------------------------------- */

MopInstancedMesh *mop_viewport_add_instanced_mesh(MopViewport *viewport,
                                                  const MopMeshDesc *desc,
                                                  const MopMat4 *transforms,
                                                  uint32_t instance_count) {
  if (!viewport || !desc || !desc->vertices || !desc->indices || !transforms ||
      instance_count == 0) {
    MOP_ERROR("invalid instanced mesh descriptor");
    return NULL;
  }
  if (desc->vertex_count == 0 || desc->index_count == 0) {
    MOP_ERROR("instanced mesh has zero vertices or indices");
    return NULL;
  }
  if (desc->index_count % 3 != 0) {
    MOP_ERROR("index count %u is not a multiple of 3", desc->index_count);
    return NULL;
  }

  /* Find a free slot (reuse inactive entries) */
  uint32_t slot = viewport->instanced_count;
  for (uint32_t i = 0; i < viewport->instanced_count; i++) {
    if (!viewport->instanced_meshes[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == viewport->instanced_count) {
    /* Need a new slot — grow if at capacity */
    if (viewport->instanced_count >= viewport->instanced_capacity) {
      uint32_t new_cap = viewport->instanced_capacity * 2;
      struct MopInstancedMesh *new_arr =
          realloc(viewport->instanced_meshes,
                  new_cap * sizeof(struct MopInstancedMesh));
      if (!new_arr) {
        MOP_ERROR("instanced mesh realloc failed (capacity %u)", new_cap);
        return NULL;
      }
      memset(new_arr + viewport->instanced_capacity, 0,
             (new_cap - viewport->instanced_capacity) *
                 sizeof(struct MopInstancedMesh));
      viewport->instanced_meshes = new_arr;
      viewport->instanced_capacity = new_cap;
    }
    viewport->instanced_count++;
  }

  /* Create RHI buffers */
  MopRhiBufferDesc vb_desc = {.data = desc->vertices,
                              .size = desc->vertex_count * sizeof(MopVertex)};
  MopRhiBuffer *vb = viewport->rhi->buffer_create(viewport->device, &vb_desc);
  if (!vb)
    return NULL;

  MopRhiBufferDesc ib_desc = {.data = desc->indices,
                              .size = desc->index_count * sizeof(uint32_t)};
  MopRhiBuffer *ib = viewport->rhi->buffer_create(viewport->device, &ib_desc);
  if (!ib) {
    viewport->rhi->buffer_destroy(viewport->device, vb);
    return NULL;
  }

  /* Copy transforms */
  MopMat4 *tforms = malloc(instance_count * sizeof(MopMat4));
  if (!tforms) {
    viewport->rhi->buffer_destroy(viewport->device, ib);
    viewport->rhi->buffer_destroy(viewport->device, vb);
    return NULL;
  }
  memcpy(tforms, transforms, instance_count * sizeof(MopMat4));

  /* Compute an average base color from vertex colors */
  MopColor avg = {0.0f, 0.0f, 0.0f, 1.0f};
  for (uint32_t i = 0; i < desc->vertex_count; i++) {
    avg.r += desc->vertices[i].color.r;
    avg.g += desc->vertices[i].color.g;
    avg.b += desc->vertices[i].color.b;
  }
  float inv = 1.0f / (float)desc->vertex_count;
  avg.r *= inv;
  avg.g *= inv;
  avg.b *= inv;

  struct MopInstancedMesh *im = &viewport->instanced_meshes[slot];
  im->vertex_buffer = vb;
  im->index_buffer = ib;
  im->vertex_count = desc->vertex_count;
  im->index_count = desc->index_count;
  im->object_id = desc->object_id;
  im->base_color = avg;
  im->opacity = 1.0f;
  im->blend_mode = MOP_BLEND_OPAQUE;
  im->active = true;
  im->transforms = tforms;
  im->instance_count = instance_count;
  im->texture = NULL;
  im->has_material = false;
  im->material = mop_material_default();

  return im;
}

void mop_instanced_mesh_update_transforms(MopInstancedMesh *mesh,
                                          const MopMat4 *transforms,
                                          uint32_t count) {
  if (!mesh || !transforms || count == 0)
    return;

  /* Reallocate if count changed */
  if (count != mesh->instance_count) {
    MopMat4 *new_t = realloc(mesh->transforms, count * sizeof(MopMat4));
    if (!new_t)
      return;
    mesh->transforms = new_t;
    mesh->instance_count = count;
  }
  memcpy(mesh->transforms, transforms, count * sizeof(MopMat4));
}

void mop_viewport_remove_instanced_mesh(MopViewport *viewport,
                                        MopInstancedMesh *mesh) {
  if (!viewport || !mesh)
    return;

  if (mesh->vertex_buffer) {
    viewport->rhi->buffer_destroy(viewport->device, mesh->vertex_buffer);
    mesh->vertex_buffer = NULL;
  }
  if (mesh->index_buffer) {
    viewport->rhi->buffer_destroy(viewport->device, mesh->index_buffer);
    mesh->index_buffer = NULL;
  }
  free(mesh->transforms);
  mesh->transforms = NULL;
  mesh->instance_count = 0;
  mesh->active = false;
}

/* -------------------------------------------------------------------------
 * Time control (Phase 8E)
 * ------------------------------------------------------------------------- */

void mop_viewport_set_time(MopViewport *viewport, float t) {
  if (!viewport)
    return;
  viewport->last_frame_time = t;
}

/* -------------------------------------------------------------------------
 * Chrome visibility
 * ------------------------------------------------------------------------- */

void mop_viewport_set_theme(MopViewport *vp, const MopTheme *theme) {
  if (!vp || !theme)
    return;
  vp->theme = *theme;
  vp->clear_color = theme->bg_bottom;

  /* Regenerate gradient background mesh with new colors */
  if (vp->bg_vb) {
    vp->rhi->buffer_destroy(vp->device, vp->bg_vb);
    vp->bg_vb = NULL;
  }
  if (vp->bg_ib) {
    vp->rhi->buffer_destroy(vp->device, vp->bg_ib);
    vp->bg_ib = NULL;
  }
  create_gradient_bg(vp);
}

const MopTheme *mop_viewport_get_theme(const MopViewport *vp) {
  if (!vp)
    return NULL;
  return &vp->theme;
}

void mop_viewport_set_chrome(MopViewport *viewport, bool visible) {
  if (!viewport)
    return;
  viewport->show_chrome = visible;

  /* Hide/show the grid mesh */
  if (viewport->grid)
    mop_mesh_set_opacity(viewport->grid, visible ? 1.0f : 0.0f);
}

/* =========================================================================
 * Debug visualization (Phase 9B)
 * ========================================================================= */

void mop_viewport_set_debug_viz(MopViewport *viewport, MopDebugViz mode) {
  if (viewport)
    viewport->debug_viz = mode;
}

MopDebugViz mop_viewport_get_debug_viz(const MopViewport *viewport) {
  return viewport ? viewport->debug_viz : MOP_DEBUG_VIZ_NONE;
}

/* =========================================================================
 * LOD system (Phase 9C)
 * ========================================================================= */

void mop_viewport_set_lod_bias(MopViewport *viewport, float bias) {
  if (viewport)
    viewport->lod_bias = bias;
}

float mop_viewport_get_lod_bias(const MopViewport *viewport) {
  return viewport ? viewport->lod_bias : 0.0f;
}

int32_t mop_mesh_add_lod(MopMesh *mesh, MopViewport *viewport,
                         const MopMeshDesc *desc, float screen_threshold) {
  if (!mesh || !viewport || !desc)
    return -1;
  if (mesh->lod_level_count >= MOP_MAX_LOD_LEVELS - 1)
    return -1;

  const MopRhiBackend *rhi = viewport->rhi;
  MopRhiDevice *dev = viewport->device;

  /* Create vertex buffer */
  MopRhiBufferDesc vb_desc = {.data = desc->vertices,
                              .size = desc->vertex_count * sizeof(MopVertex)};
  MopRhiBuffer *vb = rhi->buffer_create(dev, &vb_desc);
  if (!vb)
    return -1;

  /* Create index buffer */
  MopRhiBuffer *ib = NULL;
  if (desc->indices && desc->index_count > 0) {
    MopRhiBufferDesc ib_desc = {.data = desc->indices,
                                .size = desc->index_count * sizeof(uint32_t)};
    ib = rhi->buffer_create(dev, &ib_desc);
    if (!ib) {
      rhi->buffer_destroy(dev, vb);
      return -1;
    }
  }

  uint32_t idx = mesh->lod_level_count++;
  mesh->lod_levels[idx].vertex_buffer = vb;
  mesh->lod_levels[idx].index_buffer = ib;
  mesh->lod_levels[idx].vertex_count = desc->vertex_count;
  mesh->lod_levels[idx].index_count = desc->index_count;
  mesh->lod_levels[idx].screen_threshold = screen_threshold;

  return (int32_t)(idx + 1); /* LOD level 1..7 */
}

/* Select LOD level based on screen-space projected size.
 * Returns the LOD level index (0 = highest detail). */
static uint32_t lod_select(const MopMesh *mesh, float projected_diameter,
                           float lod_bias) {
  if (mesh->lod_level_count == 0)
    return 0;

  float effective = projected_diameter - lod_bias;
  for (uint32_t i = 0; i < mesh->lod_level_count; i++) {
    if (effective < mesh->lod_levels[i].screen_threshold)
      return i + 1; /* lower detail LOD */
  }
  return 0; /* highest detail */
}

int mop_viewport_pick_axis_indicator(MopViewport *vp, float mx, float my) {
  if (!vp)
    return 0;

  int w = vp->width, h = vp->height;
  if (w <= 0 || h <= 0)
    return 0;

  /* Same layout as axis indicator overlay — uniform scale from min(w,h) */
  float cx = roundf(0.09f * (float)w);
  float cy = roundf(0.89f * (float)h);
  float scale = fminf((float)w, (float)h) * 0.06f;

  /* View rotation only (strip translation) */
  MopMat4 view_rot = vp->view_matrix;
  view_rot.d[12] = 0.0f;
  view_rot.d[13] = 0.0f;
  view_rot.d[14] = 0.0f;
  view_rot.d[15] = 1.0f;

  MopVec3 dirs[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

  /* Map: axis index → MopViewAxis
   * +X → RIGHT(2),  +Y → TOP(4),    +Z → BACK(1)
   * -X → LEFT(3),   -Y → BOTTOM(5), -Z → FRONT(0) */
  static const int pos_view[] = {2, 4, 1}; /* MOP_VIEW_RIGHT, TOP, BACK */
  static const int neg_view[] = {3, 5, 0}; /* MOP_VIEW_LEFT, BOTTOM, FRONT */

  float hit_radius = 16.0f * (float)vp->ssaa_factor;
  float best_dist = hit_radius * hit_radius;
  int best_hit = 0;

  for (int a = 0; a < 3; a++) {
    MopVec4 dir4 = {dirs[a].x, dirs[a].y, dirs[a].z, 0.0f};
    MopVec4 rot = mop_mat4_mul_vec4(view_rot, dir4);

    /* Positive axis tip */
    float sx = cx + rot.x * scale;
    float sy = cy - rot.y * scale;
    float dx = mx - sx, dy = my - sy;
    float d2 = dx * dx + dy * dy;
    if (d2 < best_dist) {
      best_dist = d2;
      best_hit = pos_view[a] + 1; /* +1 so 0=miss */
    }

    /* Negative axis tip */
    sx = cx - rot.x * scale;
    sy = cy + rot.y * scale;
    dx = mx - sx;
    dy = my - sy;
    d2 = dx * dx + dy * dy;
    if (d2 < best_dist) {
      best_dist = d2;
      best_hit = neg_view[a] + 1;
    }
  }

  return best_hit;
}
