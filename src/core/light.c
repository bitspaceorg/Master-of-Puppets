/*
 * Master of Puppets — Light Management
 * light.c — Multi-light add/remove/update + visual indicators
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include <math.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants for light indicators
 * ------------------------------------------------------------------------- */

#define MOP_LIGHT_ID_BASE 0xFFFE0000u
#define LI_PI 3.14159265358979323846f

/* Wireframe icon parameters — thin line icons like Maya/Blender/Houdini.
 * Each "line" is a cross-shaped pair of quads (8 verts, 4 tris) for
 * visibility from all angles — same technique as gizmo tube handles. */
#define LI_HW 0.012f /* half-width of line quads — clear 2D UI feel */
#define LI_SEGS 16   /* segments for circles/arcs */

MopLight *mop_viewport_add_light(MopViewport *vp, const MopLight *desc) {
  if (!vp || !desc)
    return NULL;

  /* First user-added light replaces the built-in default directional.
   * This ensures the default doesn't interfere with explicit lighting. */
  if (vp->default_light_active) {
    vp->lights[0].active = false;
    vp->default_light_active = false;
  }

  /* Find a free slot */
  for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
    if (!vp->lights[i].active) {
      vp->lights[i] = *desc;
      vp->lights[i].active = true;
      if (vp->light_count <= i) {
        vp->light_count = i + 1;
      }
      return &vp->lights[i];
    }
  }
  return NULL; /* all slots full */
}

void mop_viewport_remove_light(MopViewport *vp, MopLight *light) {
  if (!vp || !light)
    return;
  light->active = false;
}

void mop_viewport_clear_lights(MopViewport *vp) {
  if (!vp)
    return;
  for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
    vp->lights[i].active = false;
  }
  vp->light_count = 0;
}

void mop_light_set_position(MopLight *l, MopVec3 pos) {
  if (!l)
    return;
  l->position = pos;
}

void mop_light_set_direction(MopLight *l, MopVec3 dir) {
  if (!l)
    return;
  l->direction = dir;
}

void mop_light_set_color(MopLight *l, MopColor color) {
  if (!l)
    return;
  l->color = color;
}

void mop_light_set_intensity(MopLight *l, float intensity) {
  if (!l)
    return;
  l->intensity = intensity;
}

uint32_t mop_viewport_light_count(const MopViewport *vp) {
  if (!vp)
    return 0;
  uint32_t count = 0;
  for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
    if (vp->lights[i].active)
      count++;
  }
  return count;
}

/* =========================================================================
 * Light indicators — visual representations of lights in the viewport
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * Geometry helpers (local to this TU)
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Cross-shaped line helper
 *
 * Each "line" is TWO perpendicular quads (8 verts, 4 tris) forming a
 * cross-section, visible from all viewing angles — same technique as
 * the gizmo tube handles (CYL_SEGS=4).
 * ------------------------------------------------------------------------- */

/* Emit one cross-shaped line into verts[*vi] and idx[*ii].
 * 'perp' is a unit vector perpendicular to the line direction;
 * a second perp is computed via cross product for the second quad. */
static void li_line_quad(MopVertex *verts, uint32_t *idx, int *vi, int *ii,
                         MopVec3 a, MopVec3 b, MopVec3 perp, float hw,
                         MopColor col) {
  MopVec3 n = {0, 1, 0}; /* dummy normal — chrome is unlit */

  /* Quad 1: along 'perp' */
  MopVec3 off = mop_vec3_scale(perp, hw);
  int base = *vi;
  verts[(*vi)++] = (MopVertex){mop_vec3_sub(a, off), n, col, 0, 0};
  verts[(*vi)++] = (MopVertex){mop_vec3_add(a, off), n, col, 0, 0};
  verts[(*vi)++] = (MopVertex){mop_vec3_add(b, off), n, col, 0, 0};
  verts[(*vi)++] = (MopVertex){mop_vec3_sub(b, off), n, col, 0, 0};
  idx[(*ii)++] = (uint32_t)base;
  idx[(*ii)++] = (uint32_t)(base + 1);
  idx[(*ii)++] = (uint32_t)(base + 2);
  idx[(*ii)++] = (uint32_t)(base + 2);
  idx[(*ii)++] = (uint32_t)(base + 3);
  idx[(*ii)++] = (uint32_t)base;

  /* Quad 2: perpendicular to both line direction and first perp */
  MopVec3 line_dir = mop_vec3_sub(b, a);
  float len = mop_vec3_length(line_dir);
  if (len > 0.0001f) {
    MopVec3 perp2 = mop_vec3_normalize(mop_vec3_cross(line_dir, perp));
    MopVec3 off2 = mop_vec3_scale(perp2, hw);
    base = *vi;
    verts[(*vi)++] = (MopVertex){mop_vec3_sub(a, off2), n, col, 0, 0};
    verts[(*vi)++] = (MopVertex){mop_vec3_add(a, off2), n, col, 0, 0};
    verts[(*vi)++] = (MopVertex){mop_vec3_add(b, off2), n, col, 0, 0};
    verts[(*vi)++] = (MopVertex){mop_vec3_sub(b, off2), n, col, 0, 0};
    idx[(*ii)++] = (uint32_t)base;
    idx[(*ii)++] = (uint32_t)(base + 1);
    idx[(*ii)++] = (uint32_t)(base + 2);
    idx[(*ii)++] = (uint32_t)(base + 2);
    idx[(*ii)++] = (uint32_t)(base + 3);
    idx[(*ii)++] = (uint32_t)base;
  }
}

/* -------------------------------------------------------------------------
 * Point light icon — 6 short rays forming a starburst (like Maya's point
 * light).  Each ray is a cross-shaped line (2 quads).
 *
 * 6 rays × (8 verts + 12 indices) = 48 verts, 72 indices
 * ------------------------------------------------------------------------- */
#define LI_POINT_RAYS 6
#define LI_POINT_VERTS (LI_POINT_RAYS * 8)
#define LI_POINT_IDXS (LI_POINT_RAYS * 12)

static void li_gen_point(MopVertex *verts, uint32_t *idx, float cr, float cg,
                         float cb) {
  MopColor col = {cr, cg, cb, 1.0f};
  float inner = 0.04f;
  float outer = 0.14f;
  int vi = 0, ii = 0;

  /* 6 rays: ±X, ±Y, ±Z */
  static const float dirs[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  /* Perpendicular vectors for each ray (to give the quad width) */
  static const float perps[6][3] = {{0, 1, 0}, {0, 1, 0}, {1, 0, 0},
                                    {1, 0, 0}, {1, 0, 0}, {1, 0, 0}};

  for (int r = 0; r < LI_POINT_RAYS; r++) {
    MopVec3 d = {dirs[r][0], dirs[r][1], dirs[r][2]};
    MopVec3 p = {perps[r][0], perps[r][1], perps[r][2]};
    MopVec3 a = mop_vec3_scale(d, inner);
    MopVec3 b = mop_vec3_scale(d, outer);
    li_line_quad(verts, idx, &vi, &ii, a, b, p, LI_HW, col);
  }
}

/* -------------------------------------------------------------------------
 * Directional light icon — circle + 4 downward rays (like Maya/Blender).
 * Oriented along +Z in local space; transform orients to light direction.
 *
 * Circle: LI_SEGS cross-lines
 * Rays:   4 cross-lines
 * Total:  (LI_SEGS + 4) × (8 verts + 12 indices)
 * ------------------------------------------------------------------------- */
#define LI_DIR_QUADS (LI_SEGS + 4)
#define LI_DIR_VERTS (LI_DIR_QUADS * 8)
#define LI_DIR_IDXS (LI_DIR_QUADS * 12)

static void li_gen_directional(MopVertex *verts, uint32_t *idx, float cr,
                               float cg, float cb) {
  MopColor col = {cr, cg, cb, 1.0f};
  float radius = 0.10f;
  float ray_start = 0.0f;
  float ray_end = 0.30f;
  int vi = 0, ii = 0;

  /* Circle in XY plane at Z=0 */
  MopVec3 perp_z = {0, 0, 1}; /* quads face Z */
  for (int i = 0; i < LI_SEGS; i++) {
    float a0 = (float)i * 2.0f * LI_PI / (float)LI_SEGS;
    float a1 = (float)(i + 1) * 2.0f * LI_PI / (float)LI_SEGS;
    MopVec3 p0 = {radius * cosf(a0), radius * sinf(a0), ray_start};
    MopVec3 p1 = {radius * cosf(a1), radius * sinf(a1), ray_start};
    /* Perpendicular to segment, in XY plane */
    MopVec3 seg = mop_vec3_sub(p1, p0);
    MopVec3 perp = mop_vec3_normalize(mop_vec3_cross(seg, perp_z));
    li_line_quad(verts, idx, &vi, &ii, p0, p1, perp, LI_HW, col);
  }

  /* 4 rays pointing along +Z (will be oriented by transform) */
  MopVec3 perp_x = {1, 0, 0};
  MopVec3 perp_y = {0, 1, 0};
  static const float ray_angles[4] = {0, 1.5707963f, 3.1415927f, 4.7123890f};
  for (int r = 0; r < 4; r++) {
    float ca = cosf(ray_angles[r]) * radius;
    float sa = sinf(ray_angles[r]) * radius;
    MopVec3 a = {ca, sa, ray_start};
    MopVec3 b = {ca, sa, ray_end};
    /* Choose perp based on ray angle */
    MopVec3 perp = (r == 0 || r == 2) ? perp_y : perp_x;
    li_line_quad(verts, idx, &vi, &ii, a, b, perp, LI_HW, col);
  }
}

/* -------------------------------------------------------------------------
 * Spot light icon — wireframe cone outline (apex at origin, opens along +Z).
 * 4 edge lines from apex to base ring + circle at base.
 *
 * Circle: LI_SEGS cross-lines
 * Edges:  4 cross-lines
 * Total:  (LI_SEGS + 4) × (8 verts + 12 indices)
 * ------------------------------------------------------------------------- */
#define LI_SPOT_QUADS (LI_SEGS + 4)
#define LI_SPOT_VERTS (LI_SPOT_QUADS * 8)
#define LI_SPOT_IDXS (LI_SPOT_QUADS * 12)

static void li_gen_spot(MopVertex *verts, uint32_t *idx, float cr, float cg,
                        float cb) {
  MopColor col = {cr, cg, cb, 1.0f};
  float base_r = 0.15f;
  float height = 0.30f;
  int vi = 0, ii = 0;

  /* Base circle at Z=height */
  MopVec3 perp_z = {0, 0, 1};
  for (int i = 0; i < LI_SEGS; i++) {
    float a0 = (float)i * 2.0f * LI_PI / (float)LI_SEGS;
    float a1 = (float)(i + 1) * 2.0f * LI_PI / (float)LI_SEGS;
    MopVec3 p0 = {base_r * cosf(a0), base_r * sinf(a0), height};
    MopVec3 p1 = {base_r * cosf(a1), base_r * sinf(a1), height};
    MopVec3 seg = mop_vec3_sub(p1, p0);
    MopVec3 perp = mop_vec3_normalize(mop_vec3_cross(seg, perp_z));
    li_line_quad(verts, idx, &vi, &ii, p0, p1, perp, LI_HW, col);
  }

  /* 4 edge lines from apex (origin) to base ring */
  MopVec3 apex = {0, 0, 0};
  MopVec3 perp_x = {1, 0, 0};
  MopVec3 perp_y = {0, 1, 0};
  static const float edge_angles[4] = {0, 1.5707963f, 3.1415927f, 4.7123890f};
  for (int e = 0; e < 4; e++) {
    float ca = cosf(edge_angles[e]) * base_r;
    float sa = sinf(edge_angles[e]) * base_r;
    MopVec3 base_pt = {ca, sa, height};
    MopVec3 perp = (e == 0 || e == 2) ? perp_y : perp_x;
    li_line_quad(verts, idx, &vi, &ii, apex, base_pt, perp, LI_HW, col);
  }
}

/* -------------------------------------------------------------------------
 * Transform computation for light indicators
 *
 * Builds a TRS matrix that positions the indicator at the light location
 * and orients it along the light direction.
 * ------------------------------------------------------------------------- */

/* Build a rotation matrix that aligns +Z with the given direction */
static MopMat4 li_look_along(MopVec3 dir) {
  MopVec3 z = mop_vec3_normalize(dir);
  /* Choose a "not-parallel" up vector */
  MopVec3 up = {0, 1, 0};
  if (fabsf(mop_vec3_dot(z, up)) > 0.99f)
    up = (MopVec3){1, 0, 0};
  MopVec3 x = mop_vec3_normalize(mop_vec3_cross(up, z));
  MopVec3 y = mop_vec3_cross(z, x);

  MopMat4 m = mop_mat4_identity();
  /* Column 0 = x, Column 1 = y, Column 2 = z */
  m.d[0] = x.x;
  m.d[1] = x.y;
  m.d[2] = x.z;
  m.d[4] = y.x;
  m.d[5] = y.y;
  m.d[6] = y.z;
  m.d[8] = z.x;
  m.d[9] = z.y;
  m.d[10] = z.z;
  return m;
}

static MopMat4 li_compute_transform(const MopViewport *vp,
                                    const MopLight *light, MopVec3 position) {
  /* Screen-space scale: same formula as gizmo handles */
  MopVec3 to_indicator = mop_vec3_sub(position, vp->cam_eye);
  float dist = mop_vec3_length(to_indicator);
  float s = dist * 0.16f; /* visible from distance, smaller than gizmo (0.18) */
  if (s < 0.05f)
    s = 0.05f;

  MopMat4 sc = mop_mat4_scale((MopVec3){s, s, s});
  MopMat4 t = mop_mat4_translate(position);

  /* For directional/spot: orient along direction */
  if (light->type == MOP_LIGHT_DIRECTIONAL || light->type == MOP_LIGHT_SPOT) {
    MopMat4 r = li_look_along(light->direction);
    return mop_mat4_multiply(t, mop_mat4_multiply(r, sc));
  }

  return mop_mat4_multiply(t, sc);
}

/* Compute display position for a light.
 * Point/spot: use their world position.
 * Directional: place at a fixed offset from camera target. */
static MopVec3 li_light_position(const MopViewport *vp, const MopLight *light) {
  if (light->type == MOP_LIGHT_DIRECTIONAL) {
    /* Place directional light indicator above the scene along negated direction
     */
    MopVec3 dir = mop_vec3_normalize(light->direction);
    return mop_vec3_add(vp->cam_target, mop_vec3_scale(dir, 3.0f));
  }
  return light->position;
}

/* -------------------------------------------------------------------------
 * Indicator lifecycle
 * ------------------------------------------------------------------------- */

static void li_create(MopViewport *vp, uint32_t idx) {
  const MopLight *light = &vp->lights[idx];
  uint32_t obj_id = MOP_LIGHT_ID_BASE + idx;

  /* Use the theme accent color for all light indicators — thin, bright,
   * contrasting UI chrome that stands out against the dark background. */
  MopColor accent = vp->theme.accent;
  float cr = accent.r;
  float cg = accent.g;
  float cb = accent.b;

  MopMesh *mesh = NULL;

  switch (light->type) {
  case MOP_LIGHT_POINT: {
    MopVertex verts[LI_POINT_VERTS];
    uint32_t indices[LI_POINT_IDXS];
    li_gen_point(verts, indices, cr, cg, cb);
    mesh =
        mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                 .vertex_count = LI_POINT_VERTS,
                                                 .indices = indices,
                                                 .index_count = LI_POINT_IDXS,
                                                 .object_id = obj_id});
    break;
  }
  case MOP_LIGHT_DIRECTIONAL: {
    MopVertex verts[LI_DIR_VERTS];
    uint32_t indices[LI_DIR_IDXS];
    li_gen_directional(verts, indices, cr, cg, cb);
    mesh =
        mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                 .vertex_count = LI_DIR_VERTS,
                                                 .indices = indices,
                                                 .index_count = LI_DIR_IDXS,
                                                 .object_id = obj_id});
    break;
  }
  case MOP_LIGHT_SPOT: {
    MopVertex verts[LI_SPOT_VERTS];
    uint32_t indices[LI_SPOT_IDXS];
    li_gen_spot(verts, indices, cr, cg, cb);
    mesh =
        mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                 .vertex_count = LI_SPOT_VERTS,
                                                 .indices = indices,
                                                 .index_count = LI_SPOT_IDXS,
                                                 .object_id = obj_id});
    break;
  }
  }

  if (mesh) {
    mop_mesh_set_opacity(mesh, 0.9f);
    MopVec3 pos = li_light_position(vp, light);
    MopMat4 xform = li_compute_transform(vp, light, pos);
    mop_mesh_set_transform(mesh, &xform);
  }

  vp->light_indicators[idx] = mesh;
}

static void li_destroy(MopViewport *vp, uint32_t idx) {
  if (vp->light_indicators[idx]) {
    mop_viewport_remove_mesh(vp, vp->light_indicators[idx]);
    vp->light_indicators[idx] = NULL;
  }
}

/* -------------------------------------------------------------------------
 * Public (internal) API — called from viewport.c each frame
 * ------------------------------------------------------------------------- */

void mop_light_update_indicators(MopViewport *vp) {
  if (!vp)
    return;

  for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
    bool active = vp->lights[i].active;
    bool has_indicator = (vp->light_indicators[i] != NULL);

    if (active && !has_indicator) {
      li_create(vp, i);
    } else if (!active && has_indicator) {
      li_destroy(vp, i);
    } else if (active && has_indicator) {
      /* Update position and transform */
      MopVec3 pos = li_light_position(vp, &vp->lights[i]);
      MopMat4 xform = li_compute_transform(vp, &vp->lights[i], pos);
      mop_mesh_set_transform(vp->light_indicators[i], &xform);
    }
  }
}

void mop_light_destroy_indicators(MopViewport *vp) {
  if (!vp)
    return;
  for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
    li_destroy(vp, i);
  }
}
