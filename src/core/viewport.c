/*
 * Master of Puppets — Viewport Core
 * viewport.c — Viewport lifecycle, scene management, rendering orchestration
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include "rhi/rhi.h"

#include <math.h>
#include <mop/log.h>
#include <stdlib.h>
#include <string.h>

/* Profiling helper — defined in profile.c */
double mop_profile_now_ms(void);

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
    if (vp->hook_count >= MOP_MAX_HOOKS)
      return UINT32_MAX;
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

#define MOP_INITIAL_MESH_CAPACITY 64

/* -------------------------------------------------------------------------
 * Ground grid generation
 *
 * Professional 20x20 grid on Y=0:
 *   - Center axis lines: X = red, Z = blue (wider)
 *   - Major lines every 5 units (medium brightness, medium width)
 *   - Minor lines at every 1 unit (dim, thin)
 *   object_id = 0 so the grid is not pickable.
 * ------------------------------------------------------------------------- */

#define GRID_EXTENT 20
#define GRID_HW_AXIS 0.008f
#define GRID_HW_MAJOR 0.006f
#define GRID_HW_MINOR 0.004f

static MopMesh *create_grid(MopViewport *vp) {
  const int ext = GRID_EXTENT;
  const int lines_per_axis = 2 * ext + 1;     /* -20 … +20 = 41 */
  const int total_lines = lines_per_axis * 2; /* X + Z      = 82 */
  const int vert_count = total_lines * 4;     /*              328 */
  const int idx_count = total_lines * 6;      /*              492 */

  MopVertex *v = malloc((size_t)vert_count * sizeof(MopVertex));
  uint32_t *ix = malloc((size_t)idx_count * sizeof(uint32_t));
  if (!v || !ix) {
    free(v);
    free(ix);
    return NULL;
  }

  MopVec3 n = {0, 1, 0};
  MopColor c_minor = {0.18f, 0.18f, 0.21f, 1.0f};
  MopColor c_major = {0.28f, 0.28f, 0.32f, 1.0f};
  MopColor c_x = {0.55f, 0.15f, 0.15f, 1.0f}; /* red  — X axis */
  MopColor c_z = {0.15f, 0.15f, 0.55f, 1.0f}; /* blue — Z axis */

  int vi = 0, ii = 0;
  float fext = (float)ext;

  /* Z-parallel lines (one per integer x, extending along z) */
  for (int i = 0; i < lines_per_axis; i++) {
    float x = -fext + (float)i;
    int ix_val = i - ext; /* signed offset from center */
    MopColor c;
    float hw;
    if (ix_val == 0) {
      c = c_z;
      hw = GRID_HW_AXIS;
    } else if (ix_val % 5 == 0) {
      c = c_major;
      hw = GRID_HW_MAJOR;
    } else {
      c = c_minor;
      hw = GRID_HW_MINOR;
    }
    int b = vi;
    v[vi++] = (MopVertex){{x - hw, 0.0f, -fext}, n, c, 0, 0};
    v[vi++] = (MopVertex){{x + hw, 0.0f, -fext}, n, c, 0, 0};
    v[vi++] = (MopVertex){{x + hw, 0.0f, fext}, n, c, 0, 0};
    v[vi++] = (MopVertex){{x - hw, 0.0f, fext}, n, c, 0, 0};
    ix[ii++] = (uint32_t)b;
    ix[ii++] = (uint32_t)(b + 1);
    ix[ii++] = (uint32_t)(b + 2);
    ix[ii++] = (uint32_t)(b + 2);
    ix[ii++] = (uint32_t)(b + 3);
    ix[ii++] = (uint32_t)b;
  }

  /* X-parallel lines (one per integer z, extending along x) */
  for (int i = 0; i < lines_per_axis; i++) {
    float z = -fext + (float)i;
    int iz_val = i - ext;
    MopColor c;
    float hw;
    if (iz_val == 0) {
      c = c_x;
      hw = GRID_HW_AXIS;
    } else if (iz_val % 5 == 0) {
      c = c_major;
      hw = GRID_HW_MAJOR;
    } else {
      c = c_minor;
      hw = GRID_HW_MINOR;
    }
    int b = vi;
    v[vi++] = (MopVertex){{-fext, 0.0f, z - hw}, n, c, 0, 0};
    v[vi++] = (MopVertex){{fext, 0.0f, z - hw}, n, c, 0, 0};
    v[vi++] = (MopVertex){{fext, 0.0f, z + hw}, n, c, 0, 0};
    v[vi++] = (MopVertex){{-fext, 0.0f, z + hw}, n, c, 0, 0};
    ix[ii++] = (uint32_t)b;
    ix[ii++] = (uint32_t)(b + 1);
    ix[ii++] = (uint32_t)(b + 2);
    ix[ii++] = (uint32_t)(b + 2);
    ix[ii++] = (uint32_t)(b + 3);
    ix[ii++] = (uint32_t)b;
  }

  MopMesh *grid =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){
                                    .vertices = v,
                                    .vertex_count = (uint32_t)vert_count,
                                    .indices = ix,
                                    .index_count = (uint32_t)idx_count,
                                    .object_id = 0 /* not pickable */
                                });

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
  MopColor c_top = {0.22f, 0.22f, 0.25f, 1.0f};
  MopColor c_bot = {0.11f, 0.11f, 0.13f, 1.0f};
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
                  (MopVec3){0, 0, 1},
                  (MopColor){0.85f, 0.20f, 0.20f, 1.0f}); /* X = red */
  create_one_axis(vp, 1, (MopVec3){0, 1, 0}, (MopVec3){1, 0, 0},
                  (MopVec3){0, 0, 1},
                  (MopColor){0.30f, 0.75f, 0.30f, 1.0f}); /* Y = green */
  create_one_axis(vp, 2, (MopVec3){0, 0, 1}, (MopVec3){1, 0, 0},
                  (MopVec3){0, 1, 0},
                  (MopColor){0.25f, 0.40f, 0.90f, 1.0f}); /* Z = blue */
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

  MopRhiFramebufferDesc fb_desc = {.width = desc->width,
                                   .height = desc->height};
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
  vp->clear_color = (MopColor){0.11f, 0.11f, 0.13f, 1.0f};
  vp->render_mode = MOP_RENDER_SOLID;
  vp->light_dir = (MopVec3){0.3f, 1.0f, 0.5f};
  vp->ambient = 0.25f;
  vp->shading_mode = MOP_SHADING_SMOOTH;
  vp->post_effects = MOP_POST_GAMMA;
  vp->mesh_count = 0;

  /* Multi-light: lights[0] mirrors legacy light_dir + ambient */
  memset(vp->lights, 0, sizeof(vp->lights));
  vp->lights[0] = (MopLight){
      .type = MOP_LIGHT_DIRECTIONAL,
      .direction = vp->light_dir,
      .color = (MopColor){1.0f, 1.0f, 1.0f, 1.0f},
      .intensity = 1.0f - vp->ambient,
      .active = true,
  };
  vp->light_count = 1;

  /* Display settings + overlays: all disabled by default */
  vp->display = mop_display_settings_default();
  vp->overlay_count = MOP_OVERLAY_BUILTIN_COUNT;
  memset(vp->overlays, 0, sizeof(vp->overlays));
  memset(vp->overlay_enabled, 0, sizeof(vp->overlay_enabled));

  /* Owned subsystems */
  vp->camera = mop_orbit_camera_default();
  vp->gizmo = mop_gizmo_create(vp);
  create_gradient_bg(vp);
  vp->grid = create_grid(vp);
  create_axis_indicator(vp);

  /* Chrome defaults to visible */
  vp->show_chrome = true;

  /* Register built-in subsystems */
  mop_postprocess_register(vp);

  /* Interaction state */
  vp->selected_id = 0;
  vp->interact_state = MOP_INTERACT_IDLE;
  vp->drag_axis = MOP_GIZMO_AXIS_NONE;
  vp->event_head = 0;
  vp->event_tail = 0;

  /* Apply camera to set initial matrices */
  mop_orbit_camera_apply(&vp->camera, vp);

  return vp;
}

void mop_viewport_destroy(MopViewport *viewport) {
  if (!viewport) {
    return;
  }

  /* Destroy light indicators */
  mop_light_destroy_indicators(viewport);

  /* Destroy owned gizmo */
  if (viewport->gizmo) {
    mop_gizmo_destroy(viewport->gizmo);
    viewport->gizmo = NULL;
  }

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

  free(viewport->instanced_meshes);
  free(viewport->meshes);
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

  viewport->rhi->framebuffer_resize(viewport->device, viewport->framebuffer,
                                    width, height);

  /* Recompute projection matrix */
  float aspect = (float)width / (float)height;
  viewport->projection_matrix = mop_mat4_perspective(
      viewport->cam_fov_radians, aspect, viewport->cam_near, viewport->cam_far);
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
  viewport->projection_matrix = mop_mat4_perspective(
      viewport->cam_fov_radians, aspect, near_plane, far_plane);
}

MopBackendType mop_viewport_get_backend(MopViewport *viewport) {
  if (!viewport)
    return MOP_BACKEND_CPU;
  return viewport->backend_type;
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

/* Per-frame triangle counter — accumulated across passes */
static uint32_t s_triangle_count;

/* ---- Helper macro: issue a draw call for a mesh ---- */
#define EMIT_DRAW(vp, mesh_ptr)                                                \
  do {                                                                         \
    struct MopMesh *m_ = (mesh_ptr);                                           \
    s_triangle_count += m_->index_count / 3;                                   \
    MopMat4 mvp_ = mop_mat4_multiply(                                          \
        (vp)->projection_matrix,                                               \
        mop_mat4_multiply((vp)->view_matrix, m_->world_transform));            \
    MopRhiDrawCall call_ = {                                                   \
        .vertex_buffer = m_->vertex_buffer,                                    \
        .index_buffer = m_->index_buffer,                                      \
        .vertex_count = m_->vertex_count,                                      \
        .index_count = m_->index_count,                                        \
        .object_id = m_->object_id,                                            \
        .model = m_->world_transform,                                          \
        .view = (vp)->view_matrix,                                             \
        .projection = (vp)->projection_matrix,                                 \
        .mvp = mvp_,                                                           \
        .base_color = m_->base_color,                                          \
        .opacity = m_->opacity,                                                \
        .light_dir = (vp)->light_dir,                                          \
        .ambient = (vp)->ambient,                                              \
        .shading_mode = (vp)->shading_mode,                                    \
        .wireframe =                                                           \
            ((vp)->render_mode == MOP_RENDER_WIREFRAME) && m_->object_id != 0, \
        .depth_test = (m_->object_id < 0xFFFE0000u),                           \
        .backface_cull = (m_->object_id != 0 && m_->object_id < 0xFFFE0000u),  \
        .texture = m_->texture ? m_->texture->rhi_texture : NULL,              \
        .blend_mode = m_->blend_mode,                                          \
        .metallic = m_->has_material ? m_->material.metallic : 0.0f,           \
        .roughness = m_->has_material ? m_->material.roughness : 0.5f,         \
        .emissive =                                                            \
            m_->has_material ? m_->material.emissive : (MopVec3){0, 0, 0},     \
        .lights = (vp)->lights,                                                \
        .light_count = (vp)->light_count,                                      \
        .cam_eye = (vp)->cam_eye,                                              \
        .vertex_format = m_->vertex_format,                                    \
    };                                                                         \
    (vp)->rhi->draw((vp)->device, (vp)->framebuffer, &call_);                  \
  } while (0)

/* ---- Pass: gradient background ---- */
static void pass_background(MopViewport *vp) {
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

/* ---- Pass: opaque scene meshes ---- */
static void pass_scene_opaque(MopViewport *vp) {
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *mesh = &vp->meshes[i];
    if (!mesh->active)
      continue;
    if (mesh->blend_mode != MOP_BLEND_OPAQUE)
      continue;
    if (mesh->object_id >= 0xFFFF0000u)
      continue;
    EMIT_DRAW(vp, mesh);
  }
}

/* ---- Pass: transparent scene meshes (back-to-front) ---- */
static void pass_scene_transparent(MopViewport *vp) {
  uint32_t trans_count = 0;
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *mesh = &vp->meshes[i];
    if (!mesh->active || mesh->blend_mode == MOP_BLEND_OPAQUE)
      continue;
    if (mesh->object_id >= 0xFFFF0000u)
      continue;
    trans_count++;
  }
  if (trans_count == 0)
    return;

  uint32_t *trans_idx = malloc(trans_count * sizeof(uint32_t));
  float *trans_dist = malloc(trans_count * sizeof(float));
  if (!trans_idx || !trans_dist) {
    free(trans_idx);
    free(trans_dist);
    MOP_WARN("transparent sort allocation failed, rendering unsorted");
    /* Fall through to render unsorted */
    for (uint32_t i = 0; i < vp->mesh_count; i++) {
      struct MopMesh *mesh = &vp->meshes[i];
      if (!mesh->active || mesh->blend_mode == MOP_BLEND_OPAQUE)
        continue;
      if (mesh->object_id >= 0xFFFF0000u)
        continue;
      EMIT_DRAW(vp, mesh);
    }
    return;
  }

  uint32_t ti = 0;
  MopVec3 eye = vp->cam_eye;
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *mesh = &vp->meshes[i];
    if (!mesh->active || mesh->blend_mode == MOP_BLEND_OPAQUE)
      continue;
    if (mesh->object_id >= 0xFFFF0000u)
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

  free(trans_idx);
  free(trans_dist);
}

/* ---- Pass: gizmo overlays + light indicators ---- */
static void pass_gizmo(MopViewport *vp) {
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *mesh = &vp->meshes[i];
    if (!mesh->active)
      continue;
    if (mesh->object_id < 0xFFFE0000u)
      continue; /* light indicators + gizmo handles */
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

  /* Custom overlays */
  for (uint32_t i = MOP_OVERLAY_BUILTIN_COUNT; i < vp->overlay_count; i++) {
    if (vp->overlays[i].active && vp->overlay_enabled[i] &&
        vp->overlays[i].draw_fn) {
      vp->overlays[i].draw_fn(vp, vp->overlays[i].user_data);
    }
  }
}

/* ---- Pass: axis indicator (HUD corner widget) ---- */
static void pass_hud(MopViewport *vp) {
  /* Build view-rotation-only matrix (zero out translation column) */
  MopMat4 view_rot = vp->view_matrix;
  view_rot.d[12] = 0.0f;
  view_rot.d[13] = 0.0f;
  view_rot.d[14] = 0.0f;
  view_rot.d[15] = 1.0f;

  /* Corner projection: scale down + translate to bottom-left in NDC */
  MopMat4 corner = mop_mat4_identity();
  corner.d[0] = 0.12f;   /* scale X */
  corner.d[5] = 0.12f;   /* scale Y */
  corner.d[10] = 0.12f;  /* scale Z */
  corner.d[12] = -0.82f; /* translate X (left) */
  corner.d[13] = -0.78f; /* translate Y (bottom) */

  MopMat4 axis_mvp = mop_mat4_multiply(corner, view_rot);

  for (int ax = 0; ax < 3; ax++) {
    if (!vp->axis_ind_vb[ax] || !vp->axis_ind_ib[ax])
      continue;

    MopColor ax_color;
    if (ax == 0)
      ax_color = (MopColor){0.85f, 0.20f, 0.20f, 1.0f};
    else if (ax == 1)
      ax_color = (MopColor){0.30f, 0.75f, 0.30f, 1.0f};
    else
      ax_color = (MopColor){0.25f, 0.40f, 0.90f, 1.0f};

    MopRhiDrawCall ax_call = {
        .vertex_buffer = vp->axis_ind_vb[ax],
        .index_buffer = vp->axis_ind_ib[ax],
        .vertex_count = vp->axis_ind_vcnt[ax],
        .index_count = vp->axis_ind_icnt[ax],
        .object_id = 0,
        .model = mop_mat4_identity(),
        .view = mop_mat4_identity(),
        .projection = mop_mat4_identity(),
        .mvp = axis_mvp,
        .base_color = ax_color,
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
    vp->rhi->draw(vp->device, vp->framebuffer, &ax_call);
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

/* -------------------------------------------------------------------------
 * Main render entry point
 * ------------------------------------------------------------------------- */

void mop_viewport_render(MopViewport *viewport) {
  if (!viewport)
    return;

  double t_frame_start = mop_profile_now_ms();

  /* --- PRE_RENDER hooks + frame callback --- */
  dispatch_hooks(viewport, MOP_STAGE_PRE_RENDER);
  if (viewport->frame_cb)
    viewport->frame_cb(viewport, true, viewport->frame_cb_data);

  /* Apply owned camera each frame */
  mop_orbit_camera_apply(&viewport->camera, viewport);

  /* Update light indicator meshes (create/destroy/reposition) */
  mop_light_update_indicators(viewport);

  /* Refresh gizmo scale for current camera distance */
  mop_gizmo_update(viewport->gizmo);

  /* --- Transform phase (TRS + hierarchical world transforms) --- */
  double t_transform_start = mop_profile_now_ms();
  s_triangle_count = 0;

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

  double t_transform_end = mop_profile_now_ms();

  /* --- Simulation update (water, particles, etc. via subsystem dispatch) ---
   * Must run BEFORE frame_begin so that Vulkan buffer updates (one-shot
   * command buffers) complete before the main render command buffer reads
   * the vertex data inside the render pass. */
  mop_subsystem_dispatch(&viewport->subsystems, MOP_SUBSYS_PHASE_SIMULATE,
                         viewport, 0.0f, viewport->last_frame_time);

  /* --- Clear phase --- */
  double t_clear_start = mop_profile_now_ms();
  viewport->rhi->frame_begin(viewport->device, viewport->framebuffer,
                             viewport->clear_color);
  double t_clear_end = mop_profile_now_ms();

  /* --- POST_CLEAR hooks --- */
  dispatch_hooks(viewport, MOP_STAGE_POST_CLEAR);

  /* --- Background --- */
  if (viewport->show_chrome)
    pass_background(viewport);

  /* --- PRE_SCENE hooks --- */
  dispatch_hooks(viewport, MOP_STAGE_PRE_SCENE);

  /* --- Rasterize phase --- */
  double t_rasterize_start = mop_profile_now_ms();

  pass_scene_opaque(viewport);

  /* --- POST_OPAQUE hooks --- */
  dispatch_hooks(viewport, MOP_STAGE_POST_OPAQUE);

  pass_scene_transparent(viewport);
  pass_instanced(viewport);

  /* --- POST_SCENE hooks --- */
  dispatch_hooks(viewport, MOP_STAGE_POST_SCENE);

  pass_overlays(viewport);
  if (viewport->show_chrome)
    pass_gizmo(viewport);

  /* --- POST_OVERLAY hooks --- */
  dispatch_hooks(viewport, MOP_STAGE_POST_OVERLAY);

  if (viewport->show_chrome)
    pass_hud(viewport);

  viewport->rhi->frame_end(viewport->device, viewport->framebuffer);

  /* Post-render subsystems (postprocess effects, etc.) */
  mop_subsystem_dispatch(&viewport->subsystems, MOP_SUBSYS_PHASE_POST_RENDER,
                         viewport, 0.0f, viewport->last_frame_time);

  double t_rasterize_end = mop_profile_now_ms();

  /* --- POST_RENDER hooks + frame callback --- */
  dispatch_hooks(viewport, MOP_STAGE_POST_RENDER);
  if (viewport->frame_cb)
    viewport->frame_cb(viewport, false, viewport->frame_cb_data);

  double t_frame_end = mop_profile_now_ms();

  /* Store profiling stats */
  viewport->last_stats = (MopFrameStats){
      .frame_time_ms = t_frame_end - t_frame_start,
      .clear_ms = t_clear_end - t_clear_start,
      .transform_ms = t_transform_end - t_transform_start,
      .rasterize_ms = t_rasterize_end - t_rasterize_start,
      .triangle_count = s_triangle_count,
      .pixel_count = (uint32_t)(viewport->width * viewport->height)};
}

/* -------------------------------------------------------------------------
 * Framebuffer readback
 * ------------------------------------------------------------------------- */

const uint8_t *mop_viewport_read_color(MopViewport *viewport, int *out_width,
                                       int *out_height) {
  if (!viewport)
    return NULL;
  return viewport->rhi->framebuffer_read_color(
      viewport->device, viewport->framebuffer, out_width, out_height);
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

  uint32_t id = viewport->rhi->pick_read_id(viewport->device,
                                            viewport->framebuffer, x, y);

  if (id != 0) {
    result.hit = true;
    result.object_id = id;
    result.depth = viewport->rhi->pick_read_depth(viewport->device,
                                                  viewport->framebuffer, x, y);
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

void mop_viewport_set_chrome(MopViewport *viewport, bool visible) {
  if (!viewport)
    return;
  viewport->show_chrome = visible;

  /* Hide/show the grid mesh */
  if (viewport->grid)
    mop_mesh_set_opacity(viewport->grid, visible ? 1.0f : 0.0f);
}
