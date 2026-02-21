/*
 * Master of Puppets — Light Management
 * light.c — Multi-light add/remove/update + visual indicators
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Constants for light indicators
 * ------------------------------------------------------------------------- */

#define MOP_LIGHT_ID_BASE  0xFFFE0000u
#define LI_PI              3.14159265358979323846f

/* Geometry parameters — keep small so indicators don't dominate the scene */
#define LI_CYL_SEGS        8
#define LI_OCTA_RADIUS     0.12f
#define LI_ARROW_RADIUS    0.015f
#define LI_ARROW_START     0.0f
#define LI_ARROW_END       0.30f
#define LI_CONE_BASE       0.06f
#define LI_CONE_TIP        0.42f
#define LI_SPOT_BASE       0.15f
#define LI_SPOT_HEIGHT     0.35f

MopLight *mop_viewport_add_light(MopViewport *vp, const MopLight *desc) {
    if (!vp || !desc) return NULL;

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
    if (!vp || !light) return;
    light->active = false;
}

void mop_light_set_position(MopLight *l, MopVec3 pos) {
    if (!l) return;
    l->position = pos;
}

void mop_light_set_direction(MopLight *l, MopVec3 dir) {
    if (!l) return;
    l->direction = dir;
}

void mop_light_set_color(MopLight *l, MopColor color) {
    if (!l) return;
    l->color = color;
}

void mop_light_set_intensity(MopLight *l, float intensity) {
    if (!l) return;
    l->intensity = intensity;
}

uint32_t mop_viewport_light_count(const MopViewport *vp) {
    if (!vp) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
        if (vp->lights[i].active) count++;
    }
    return count;
}

/* =========================================================================
 * Light indicators — visual representations of lights in the viewport
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * Geometry helpers (local to this TU)
 * ------------------------------------------------------------------------- */

/* Map (along-axis, u, v) → world position.  axis=2 (Z) for default. */
static MopVec3 li_on_axis(float along, float u, float w) {
    return (MopVec3){u, w, along};  /* Z-up for default orientation */
}

/* Octahedron — 8 faces × 3 verts = 24 verts, 24 indices (flat shaded) */
#define LI_OCTA_VERTS 24
#define LI_OCTA_IDXS  24

static void li_gen_octahedron(MopVertex *verts, uint32_t *idx, float radius,
                               float cr, float cg, float cb) {
    MopColor col = {cr, cg, cb, 1.0f};
    float r = radius;
    MopVec3 p[6] = {
        { r, 0, 0}, {-r, 0, 0}, { 0, r, 0},
        { 0,-r, 0}, { 0, 0, r}, { 0, 0,-r}
    };
    static const int faces[8][3] = {
        {0,2,4}, {0,4,3}, {0,3,5}, {0,5,2},
        {1,4,2}, {1,3,4}, {1,5,3}, {1,2,5}
    };
    int vi = 0, ii = 0;
    for (int f = 0; f < 8; f++) {
        MopVec3 p0 = p[faces[f][0]], p1 = p[faces[f][1]], p2 = p[faces[f][2]];
        MopVec3 n = mop_vec3_normalize(mop_vec3_cross(
            mop_vec3_sub(p1, p0), mop_vec3_sub(p2, p0)));
        verts[vi] = (MopVertex){p0, n, col, 0, 0}; idx[ii++] = (uint32_t)vi++;
        verts[vi] = (MopVertex){p1, n, col, 0, 0}; idx[ii++] = (uint32_t)vi++;
        verts[vi] = (MopVertex){p2, n, col, 0, 0}; idx[ii++] = (uint32_t)vi++;
    }
}

/* Arrow (cylinder + cone) — for directional light indicator.
 * Points along +Z in local space; transform orients it. */
#define LI_CYL_VERTS   (2 * LI_CYL_SEGS)                     /* 16 */
#define LI_CYL_IDXS    (LI_CYL_SEGS * 6)                     /* 48 */
#define LI_CONE_VERTS  (2 * LI_CYL_SEGS + 1 + LI_CYL_SEGS)   /* 25 */
#define LI_CONE_IDXS   (LI_CYL_SEGS * 3 + LI_CYL_SEGS * 3)   /* 48 */
#define LI_ARROW_VERTS (LI_CYL_VERTS + LI_CONE_VERTS)         /* 41 */
#define LI_ARROW_IDXS  (LI_CYL_IDXS + LI_CONE_IDXS)          /* 96 */

static void li_gen_cylinder(MopVertex *verts, uint32_t *idx,
                             float radius, float start, float end,
                             float cr, float cg, float cb) {
    MopColor col = {cr, cg, cb, 1.0f};
    for (int i = 0; i < LI_CYL_SEGS; i++) {
        float a = (float)i * 2.0f * LI_PI / LI_CYL_SEGS;
        float ca = cosf(a), sa = sinf(a);
        MopVec3 n = {ca, sa, 0};
        verts[i]               = (MopVertex){li_on_axis(start, radius*ca, radius*sa), n, col, 0, 0};
        verts[i + LI_CYL_SEGS] = (MopVertex){li_on_axis(end,   radius*ca, radius*sa), n, col, 0, 0};
    }
    for (int i = 0; i < LI_CYL_SEGS; i++) {
        int nx = (i + 1) % LI_CYL_SEGS;
        int b  = i * 6;
        idx[b]=i; idx[b+1]=nx; idx[b+2]=i+LI_CYL_SEGS;
        idx[b+3]=nx; idx[b+4]=nx+LI_CYL_SEGS; idx[b+5]=i+LI_CYL_SEGS;
    }
}

static void li_gen_cone(MopVertex *verts, uint32_t *idx,
                         float base_r, float start, float end,
                         float cr, float cg, float cb) {
    MopColor col = {cr, cg, cb, 1.0f};
    float h     = end - start;
    float slant = sqrtf(h * h + base_r * base_r);
    float na    = base_r / slant;
    float nr    = h / slant;

    for (int i = 0; i < LI_CYL_SEGS; i++) {
        float a  = (float)i * 2.0f * LI_PI / LI_CYL_SEGS;
        float am = ((float)i + 0.5f) * 2.0f * LI_PI / LI_CYL_SEGS;
        float ca = cosf(a),  sa = sinf(a);
        float cm = cosf(am), sm = sinf(am);
        MopVec3 bn = {nr*ca, nr*sa, na};
        MopVec3 tn = {nr*cm, nr*sm, na};
        verts[i]               = (MopVertex){li_on_axis(start, base_r*ca, base_r*sa), bn, col, 0, 0};
        verts[i + LI_CYL_SEGS] = (MopVertex){li_on_axis(end, 0, 0), tn, col, 0, 0};
    }
    int ii = 0;
    for (int i = 0; i < LI_CYL_SEGS; i++) {
        int nx = (i + 1) % LI_CYL_SEGS;
        idx[ii++] = i; idx[ii++] = nx; idx[ii++] = i + LI_CYL_SEGS;
    }

    /* Base cap */
    int vi = 2 * LI_CYL_SEGS;
    MopVec3 cap_n = {0, 0, -1};
    verts[vi] = (MopVertex){li_on_axis(start, 0, 0), cap_n, col, 0, 0};
    int ci = vi++;
    for (int i = 0; i < LI_CYL_SEGS; i++) {
        float a = (float)i * 2.0f * LI_PI / LI_CYL_SEGS;
        verts[vi + i] = (MopVertex){
            li_on_axis(start, base_r*cosf(a), base_r*sinf(a)),
            cap_n, col, 0, 0};
    }
    for (int i = 0; i < LI_CYL_SEGS; i++) {
        int nx = (i + 1) % LI_CYL_SEGS;
        idx[ii++] = ci; idx[ii++] = vi + nx; idx[ii++] = vi + i;
    }
}

static void li_gen_arrow(MopVertex *verts, uint32_t *idx,
                          float cr, float cg, float cb) {
    li_gen_cylinder(verts, idx, LI_ARROW_RADIUS,
                     LI_ARROW_START, LI_ARROW_END, cr, cg, cb);
    li_gen_cone(verts + LI_CYL_VERTS, idx + LI_CYL_IDXS,
                 LI_CONE_BASE, LI_ARROW_END, LI_CONE_TIP, cr, cg, cb);
    for (int i = LI_CYL_IDXS; i < LI_ARROW_IDXS; i++)
        idx[i] += LI_CYL_VERTS;
}

/* Spot cone — open cone showing direction and angle */
#define LI_SPOT_VERTS (LI_CYL_SEGS + 1)  /* ring + apex */
#define LI_SPOT_IDXS  (LI_CYL_SEGS * 6)  /* sides + base ring */

static void li_gen_spot_cone(MopVertex *verts, uint32_t *idx,
                              float cr, float cg, float cb) {
    MopColor col = {cr, cg, cb, 0.8f};

    /* Apex at origin, cone opens along +Z */
    verts[0] = (MopVertex){{0, 0, 0}, {0, 0, -1}, col, 0, 0};

    /* Base ring */
    for (int i = 0; i < LI_CYL_SEGS; i++) {
        float a = (float)i * 2.0f * LI_PI / LI_CYL_SEGS;
        float ca = cosf(a), sa = sinf(a);
        MopVec3 n = mop_vec3_normalize((MopVec3){ca, sa, -LI_SPOT_BASE / LI_SPOT_HEIGHT});
        verts[1 + i] = (MopVertex){
            {LI_SPOT_BASE * ca, LI_SPOT_BASE * sa, LI_SPOT_HEIGHT},
            n, col, 0, 0};
    }

    /* Side triangles */
    int ii = 0;
    for (int i = 0; i < LI_CYL_SEGS; i++) {
        int nx = (i + 1) % LI_CYL_SEGS;
        idx[ii++] = 0;
        idx[ii++] = 1 + i;
        idx[ii++] = 1 + nx;
    }
    /* Back face (so cone is visible from behind too) */
    for (int i = 0; i < LI_CYL_SEGS; i++) {
        int nx = (i + 1) % LI_CYL_SEGS;
        idx[ii++] = 0;
        idx[ii++] = 1 + nx;
        idx[ii++] = 1 + i;
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
    m.d[0] = x.x; m.d[1] = x.y; m.d[2]  = x.z;
    m.d[4] = y.x; m.d[5] = y.y; m.d[6]  = y.z;
    m.d[8] = z.x; m.d[9] = z.y; m.d[10] = z.z;
    return m;
}

static MopMat4 li_compute_transform(const MopViewport *vp,
                                      const MopLight *light,
                                      MopVec3 position) {
    /* Screen-space scale: same formula as gizmo handles */
    MopVec3 to_indicator = mop_vec3_sub(position, vp->cam_eye);
    float dist = mop_vec3_length(to_indicator);
    float s = dist * 0.12f;   /* slightly smaller than gizmo (0.18) */
    if (s < 0.03f) s = 0.03f;

    MopMat4 sc = mop_mat4_scale((MopVec3){s, s, s});
    MopMat4 t  = mop_mat4_translate(position);

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
        /* Place directional light indicator above the scene along negated direction */
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

    /* Determine color from light — clamp for visibility */
    float cr = light->color.r * light->intensity;
    float cg = light->color.g * light->intensity;
    float cb = light->color.b * light->intensity;
    float mx = cr > cg ? cr : cg;
    if (cb > mx) mx = cb;
    if (mx > 0.0f && mx < 0.5f) {
        float boost = 0.5f / mx;
        cr *= boost; cg *= boost; cb *= boost;
    }
    if (cr > 1.0f) cr = 1.0f;
    if (cg > 1.0f) cg = 1.0f;
    if (cb > 1.0f) cb = 1.0f;

    MopMesh *mesh = NULL;

    switch (light->type) {
    case MOP_LIGHT_POINT: {
        MopVertex verts[LI_OCTA_VERTS];
        uint32_t  indices[LI_OCTA_IDXS];
        li_gen_octahedron(verts, indices, LI_OCTA_RADIUS, cr, cg, cb);
        mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){
            .vertices = verts, .vertex_count = LI_OCTA_VERTS,
            .indices = indices, .index_count = LI_OCTA_IDXS,
            .object_id = obj_id});
        break;
    }
    case MOP_LIGHT_DIRECTIONAL: {
        MopVertex verts[LI_ARROW_VERTS];
        uint32_t  indices[LI_ARROW_IDXS];
        li_gen_arrow(verts, indices, cr, cg, cb);
        mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){
            .vertices = verts, .vertex_count = LI_ARROW_VERTS,
            .indices = indices, .index_count = LI_ARROW_IDXS,
            .object_id = obj_id});
        break;
    }
    case MOP_LIGHT_SPOT: {
        MopVertex verts[LI_SPOT_VERTS];
        uint32_t  indices[LI_SPOT_IDXS];
        li_gen_spot_cone(verts, indices, cr, cg, cb);
        mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){
            .vertices = verts, .vertex_count = LI_SPOT_VERTS,
            .indices = indices, .index_count = LI_SPOT_IDXS,
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
    if (!vp) return;

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
    if (!vp) return;
    for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
        li_destroy(vp, i);
    }
}
