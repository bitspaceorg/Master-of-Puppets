/*
 * Master of Puppets — Gizmo Module
 * gizmo.c — TRS gizmo handle geometry, picking, and drag math
 *
 * This module creates visual handle meshes (translate arrows, rotate rings,
 * scale cubes) via the public viewport API and computes transform deltas
 * from mouse input using the viewport's internal camera state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include "viewport/viewport_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define PI 3.14159265358979323846f

/* Handle IDs start high to avoid collision with scene object IDs. */
#define MOP_GIZMO_ID_BASE 0xFFFF0000u

/* Geometry parameters */
#define RING_SEGS        128
#define CYL_SEGS         12
#define TORUS_MINOR_SEGS 8

/* Primitive vertex / index counts */
#define CYL_VERTS   (2 * CYL_SEGS)                         /*   24 */
#define CYL_IDXS    (CYL_SEGS * 6)                         /*   72 */
#define CONE_VERTS  (2 * CYL_SEGS + 1 + CYL_SEGS)          /*   37 */
#define CONE_IDXS   (CYL_SEGS * 3 + CYL_SEGS * 3)          /*   72 */
#define CUBE_VERTS  24
#define CUBE_IDXS   36
#define TORUS_VERTS (RING_SEGS * TORUS_MINOR_SEGS)          /* 1024 */
#define TORUS_IDXS  (RING_SEGS * TORUS_MINOR_SEGS * 6)      /* 6144 */
#define OCTA_VERTS  24
#define OCTA_IDXS   24
#define QUAD_VERTS  4
#define QUAD_IDXS   12

/* Composite handle counts */
#define TRANSLATE_VERTS (CYL_VERTS + CONE_VERTS)            /*   61 */
#define TRANSLATE_IDXS  (CYL_IDXS + CONE_IDXS)             /*  144 */
#define SCALE_VERTS     (CYL_VERTS + CUBE_VERTS)            /*   48 */
#define SCALE_IDXS      (CYL_IDXS + CUBE_IDXS)             /*  108 */
#define CENTER_VERTS    (OCTA_VERTS + 3 * QUAD_VERTS)       /*   36 */
#define CENTER_IDXS     (OCTA_IDXS + 3 * QUAD_IDXS)        /*   60 */

/* Static counter for unique gizmo IDs across multiple instances */
static uint32_t gizmo_instance_counter = 0;

/* -------------------------------------------------------------------------
 * Gizmo structure
 * ------------------------------------------------------------------------- */

/* Default opacity applied to the target mesh when the gizmo is shown */
#define GIZMO_SELECTION_OPACITY 0.4f

struct MopGizmo {
    MopViewport  *viewport;
    MopGizmoMode  mode;
    MopVec3       position;
    MopVec3       rotation;        /* local-space euler angles */
    bool          visible;
    MopMesh      *handles[4];      /* X, Y, Z, Center */
    uint32_t      handle_ids[4];   /* unique per gizmo instance */
    MopMesh      *target;          /* mesh made transparent on show */
};

/* -------------------------------------------------------------------------
 * Handle colors:  X=red, Y=green, Z=blue, Center=yellow
 * ------------------------------------------------------------------------- */

static const float GIZMO_COLORS[4][3] = {
    { 0.9f, 0.15f, 0.15f },   /* X — red    */
    { 0.15f, 0.9f, 0.15f },   /* Y — green  */
    { 0.15f, 0.15f, 0.9f },   /* Z — blue   */
    { 0.95f, 0.85f, 0.15f }   /* Center — yellow */
};

/* -------------------------------------------------------------------------
 * Rotation helpers
 * ------------------------------------------------------------------------- */

/* Build rotation matrix from euler angles (Rz*Ry*Rx, same as mop_mat4_compose_trs) */
static MopMat4 gizmo_rotation_matrix(MopVec3 rot) {
    MopMat4 rx = mop_mat4_rotate_x(rot.x);
    MopMat4 ry = mop_mat4_rotate_y(rot.y);
    MopMat4 rz = mop_mat4_rotate_z(rot.z);
    return mop_mat4_multiply(rz, mop_mat4_multiply(ry, rx));
}

/* Get the world-space direction of a local axis after rotation */
static MopVec3 rotated_axis_dir(int axis, MopVec3 rot) {
    MopVec3 dir = {0,0,0};
    if (axis == 0) dir.x = 1; else if (axis == 1) dir.y = 1; else dir.z = 1;
    MopMat4 r = gizmo_rotation_matrix(rot);
    MopVec4 d4 = mop_mat4_mul_vec4(r, (MopVec4){dir.x, dir.y, dir.z, 0.0f});
    return (MopVec3){d4.x, d4.y, d4.z};
}

/* -------------------------------------------------------------------------
 * Geometry helpers (all static)
 * ------------------------------------------------------------------------- */

/* Map (along-axis, cross-section u, cross-section v) to world XYZ */
static MopVec3 on_axis(int axis, float along, float u, float w) {
    if (axis == 0) return (MopVec3){along, u, w};
    if (axis == 1) return (MopVec3){u, along, w};
    return (MopVec3){u, w, along};
}

/* Smooth-shaded cylinder (no caps), CYL_SEGS sides */
static void gen_cylinder(MopVertex *verts, uint32_t *idx, int axis,
                         float radius, float start, float end,
                         float cr, float cg, float cb) {
    MopColor col = {cr, cg, cb, 1};
    for (int i = 0; i < CYL_SEGS; i++) {
        float a = i * 2.0f * PI / CYL_SEGS;
        float ca = cosf(a), sa = sinf(a);
        MopVec3 n = on_axis(axis, 0, ca, sa);
        verts[i]            = (MopVertex){
            on_axis(axis, start, radius*ca, radius*sa), n, col, 0, 0};
        verts[i + CYL_SEGS] = (MopVertex){
            on_axis(axis, end,   radius*ca, radius*sa), n, col, 0, 0};
    }
    for (int i = 0; i < CYL_SEGS; i++) {
        int nx = (i + 1) % CYL_SEGS;
        int b  = i * 6;
        idx[b]=i; idx[b+1]=nx; idx[b+2]=i+CYL_SEGS;
        idx[b+3]=nx; idx[b+4]=nx+CYL_SEGS; idx[b+5]=i+CYL_SEGS;
    }
}

/* Cone with base cap, CYL_SEGS sides */
static void gen_cone(MopVertex *verts, uint32_t *idx, int axis,
                     float base_r, float start, float end,
                     float cr, float cg, float cb) {
    MopColor col = {cr, cg, cb, 1};
    float h     = end - start;
    float slant = sqrtf(h * h + base_r * base_r);
    float na    = base_r / slant;   /* axial  component of surface normal */
    float nr    = h / slant;        /* radial component of surface normal */

    /* Side: base ring + per-triangle apex vertices */
    for (int i = 0; i < CYL_SEGS; i++) {
        float a  = i * 2.0f * PI / CYL_SEGS;
        float am = (i + 0.5f) * 2.0f * PI / CYL_SEGS;
        float ca = cosf(a),  sa = sinf(a);
        float cm = cosf(am), sm = sinf(am);
        verts[i]            = (MopVertex){
            on_axis(axis, start, base_r*ca, base_r*sa),
            on_axis(axis, na, nr*ca, nr*sa), col, 0, 0};
        verts[i + CYL_SEGS] = (MopVertex){
            on_axis(axis, end, 0, 0),
            on_axis(axis, na, nr*cm, nr*sm), col, 0, 0};
    }
    int ii = 0;
    for (int i = 0; i < CYL_SEGS; i++) {
        int nx = (i + 1) % CYL_SEGS;
        idx[ii++] = i; idx[ii++] = nx; idx[ii++] = i + CYL_SEGS;
    }

    /* Base cap */
    int vi = 2 * CYL_SEGS;
    MopVec3 cap_n = on_axis(axis, -1, 0, 0);
    verts[vi] = (MopVertex){on_axis(axis, start, 0, 0), cap_n, col, 0, 0};
    int ci = vi++;
    for (int i = 0; i < CYL_SEGS; i++) {
        float a = i * 2.0f * PI / CYL_SEGS;
        verts[vi + i] = (MopVertex){
            on_axis(axis, start, base_r*cosf(a), base_r*sinf(a)),
            cap_n, col, 0, 0};
    }
    for (int i = 0; i < CYL_SEGS; i++) {
        int nx = (i + 1) % CYL_SEGS;
        idx[ii++] = ci; idx[ii++] = vi + nx; idx[ii++] = vi + i;
    }
}

/* Axis-aligned cube: 6 faces x 4 verts = 24 verts, 36 indices */
static void gen_cube(MopVertex *verts, uint32_t *idx,
                     float cx, float cy, float cz, float half,
                     float cr, float cg, float cb) {
    float s = half;
    MopVec3 c[8] = {
        {cx-s,cy-s,cz-s}, {cx+s,cy-s,cz-s},
        {cx+s,cy+s,cz-s}, {cx-s,cy+s,cz-s},
        {cx-s,cy-s,cz+s}, {cx+s,cy-s,cz+s},
        {cx+s,cy+s,cz+s}, {cx-s,cy+s,cz+s}
    };
    static const int fi[6][4] = {
        {4,5,6,7}, {1,0,3,2}, {7,6,2,3}, {0,1,5,4}, {5,1,2,6}, {0,4,7,3}
    };
    static const float fn[6][3] = {
        {0,0,1}, {0,0,-1}, {0,1,0}, {0,-1,0}, {1,0,0}, {-1,0,0}
    };
    MopColor col = {cr, cg, cb, 1};
    for (int f = 0; f < 6; f++) {
        int base = f * 4;
        for (int j = 0; j < 4; j++)
            verts[base+j] = (MopVertex){c[fi[f][j]],
                {fn[f][0],fn[f][1],fn[f][2]}, col, 0, 0};
        int ib = f * 6;
        idx[ib]=base; idx[ib+1]=base+1; idx[ib+2]=base+2;
        idx[ib+3]=base+2; idx[ib+4]=base+3; idx[ib+5]=base;
    }
}

/* Smooth-shaded torus ring */
static void gen_torus(MopVertex *verts, uint32_t *idx, int axis,
                      float major_r, float minor_r,
                      float cr, float cg, float cb) {
    MopColor col = {cr, cg, cb, 1};
    int vi = 0;
    for (int i = 0; i < RING_SEGS; i++) {
        float theta = i * 2.0f * PI / RING_SEGS;
        float ct = cosf(theta), st = sinf(theta);
        for (int j = 0; j < TORUS_MINOR_SEGS; j++) {
            float phi = j * 2.0f * PI / TORUS_MINOR_SEGS;
            float cp = cosf(phi), sp = sinf(phi);
            float r = major_r + minor_r * cp;
            verts[vi++] = (MopVertex){
                on_axis(axis, minor_r * sp, r * ct, r * st),
                on_axis(axis, sp, cp * ct, cp * st),
                col, 0, 0};
        }
    }
    int ii = 0;
    for (int i = 0; i < RING_SEGS; i++) {
        int inx = (i + 1) % RING_SEGS;
        for (int j = 0; j < TORUS_MINOR_SEGS; j++) {
            int jnx = (j + 1) % TORUS_MINOR_SEGS;
            int a = i * TORUS_MINOR_SEGS + j;
            int b = i * TORUS_MINOR_SEGS + jnx;
            int c = inx * TORUS_MINOR_SEGS + jnx;
            int d = inx * TORUS_MINOR_SEGS + j;
            idx[ii++]=a; idx[ii++]=d; idx[ii++]=c;
            idx[ii++]=a; idx[ii++]=c; idx[ii++]=b;
        }
    }
}

/* Flat-shaded octahedron: 8 faces x 3 verts = 24 verts */
static void gen_octahedron(MopVertex *verts, uint32_t *idx, float radius,
                           float cr, float cg, float cb) {
    MopColor col = {cr, cg, cb, 1};
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
        MopVec3 p0=p[faces[f][0]], p1=p[faces[f][1]], p2=p[faces[f][2]];
        MopVec3 n = mop_vec3_normalize(mop_vec3_cross(
            mop_vec3_sub(p1, p0), mop_vec3_sub(p2, p0)));
        verts[vi]=(MopVertex){p0, n, col, 0, 0}; idx[ii++]=vi++;
        verts[vi]=(MopVertex){p1, n, col, 0, 0}; idx[ii++]=vi++;
        verts[vi]=(MopVertex){p2, n, col, 0, 0}; idx[ii++]=vi++;
    }
}

/* Double-sided plane quad for planar handle decoration */
static void gen_plane_quad(MopVertex *verts, uint32_t *idx,
                           int axis_u, int axis_v, float offset, float size,
                           float cr, float cg, float cb) {
    MopColor col = {cr, cg, cb, 0.5f};
    int axis_n = 3 - axis_u - axis_v;
    MopVec3 n = {0,0,0};
    if (axis_n == 0) n.x = 1; else if (axis_n == 1) n.y = 1; else n.z = 1;
    float lo = offset, hi = offset + size;
    float coords[4][2] = {{lo,lo},{hi,lo},{hi,hi},{lo,hi}};
    for (int i = 0; i < 4; i++) {
        MopVec3 p = {0,0,0};
        if (axis_u == 0) p.x = coords[i][0];
        else if (axis_u == 1) p.y = coords[i][0];
        else p.z = coords[i][0];
        if (axis_v == 0) p.x = coords[i][1];
        else if (axis_v == 1) p.y = coords[i][1];
        else p.z = coords[i][1];
        verts[i] = (MopVertex){p, n, col, 0, 0};
    }
    /* Front face */
    idx[0]=0; idx[1]=1; idx[2]=2; idx[3]=2; idx[4]=3; idx[5]=0;
    /* Back face */
    idx[6]=0; idx[7]=3; idx[8]=2; idx[9]=2; idx[10]=1; idx[11]=0;
}

/* --- Composite handle generators --- */

static void gen_translate_handle(MopVertex *v, uint32_t *idx, int axis,
                                 float cr, float cg, float cb) {
    gen_cylinder(v, idx, axis, 0.018f, 0.20f, 1.05f, cr, cg, cb);
    gen_cone(v + CYL_VERTS, idx + CYL_IDXS, axis,
             0.05f, 1.05f, 1.25f, cr, cg, cb);
    for (int i = CYL_IDXS; i < TRANSLATE_IDXS; i++) idx[i] += CYL_VERTS;
}

static void gen_scale_handle(MopVertex *v, uint32_t *idx, int axis,
                             float cr, float cg, float cb) {
    gen_cylinder(v, idx, axis, 0.018f, 0.20f, 1.05f, cr, cg, cb);
    float ec[3] = {0, 0, 0}; ec[axis] = 1.15f;
    gen_cube(v + CYL_VERTS, idx + CYL_IDXS,
             ec[0], ec[1], ec[2], 0.04f, cr, cg, cb);
    for (int i = CYL_IDXS; i < SCALE_IDXS; i++) idx[i] += CYL_VERTS;
}

static void gen_rotate_handle(MopVertex *v, uint32_t *idx, int axis,
                              float cr, float cg, float cb) {
    gen_torus(v, idx, axis, 1.0f, 0.018f, cr, cg, cb);
}

/* -------------------------------------------------------------------------
 * Screen-space projection helpers
 * ------------------------------------------------------------------------- */

static MopVec3 world_to_screen(MopVec3 p, const MopViewport *vp) {
    MopMat4 vpm = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);
    MopVec4 clip = mop_mat4_mul_vec4(vpm, (MopVec4){p.x, p.y, p.z, 1.0f});
    if (fabsf(clip.w) < 1e-6f) clip.w = 1e-6f;
    float nx = clip.x / clip.w, ny = clip.y / clip.w;
    return (MopVec3){
        (nx * 0.5f + 0.5f) * vp->width,
        (1.0f - (ny * 0.5f + 0.5f)) * vp->height,
        clip.z / clip.w
    };
}

/* Screen-space direction of a local axis, accounting for gizmo rotation */
static MopVec3 axis_screen_dir(MopVec3 origin, int axis, MopVec3 rot,
                               const MopViewport *vp) {
    MopVec3 dir = rotated_axis_dir(axis, rot);
    MopVec3 tip = mop_vec3_add(origin, mop_vec3_scale(dir, 0.5f));
    MopVec3 s0 = world_to_screen(origin, vp);
    MopVec3 s1 = world_to_screen(tip, vp);
    float dx = s1.x-s0.x, dy = s1.y-s0.y;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) return (MopVec3){1,0,0};
    return (MopVec3){dx/len, dy/len, 0};
}

/* -------------------------------------------------------------------------
 * Internal: update handle transforms (translate + rotate)
 * ------------------------------------------------------------------------- */

static void update_handle_transforms(MopGizmo *g) {
    /* Scale gizmo to maintain roughly constant screen size.
     * Base geometry was designed for camera distance ~4.5 units. */
    MopViewport *vp = g->viewport;
    MopVec3 to_gizmo = mop_vec3_sub(g->position, vp->cam_eye);
    float dist = mop_vec3_length(to_gizmo);
    float s = dist * 0.18f;
    if (s < 0.05f) s = 0.05f;

    MopMat4 sc = mop_mat4_scale((MopVec3){s, s, s});
    MopMat4 r  = gizmo_rotation_matrix(g->rotation);
    MopMat4 t  = mop_mat4_translate(g->position);
    MopMat4 tr = mop_mat4_multiply(t, mop_mat4_multiply(r, sc));
    for (int a = 0; a < 4; a++)
        if (g->handles[a])
            mop_mesh_set_transform(g->handles[a], &tr);
}

/* -------------------------------------------------------------------------
 * Internal: create/destroy handle meshes in the viewport
 * ------------------------------------------------------------------------- */

static void create_handles(MopGizmo *g) {
    MopViewport *vp = g->viewport;

    /* Axis handles (X, Y, Z) */
    for (int a = 0; a < 3; a++) {
        const float *clr = GIZMO_COLORS[a];
        if (g->mode == MOP_GIZMO_TRANSLATE) {
            MopVertex verts[TRANSLATE_VERTS]; uint32_t indices[TRANSLATE_IDXS];
            gen_translate_handle(verts, indices, a, clr[0], clr[1], clr[2]);
            g->handles[a] = mop_viewport_add_mesh(vp, &(MopMeshDesc){
                .vertices=verts, .vertex_count=TRANSLATE_VERTS,
                .indices=indices, .index_count=TRANSLATE_IDXS,
                .object_id=g->handle_ids[a]});
        } else if (g->mode == MOP_GIZMO_SCALE) {
            MopVertex verts[SCALE_VERTS]; uint32_t indices[SCALE_IDXS];
            gen_scale_handle(verts, indices, a, clr[0], clr[1], clr[2]);
            g->handles[a] = mop_viewport_add_mesh(vp, &(MopMeshDesc){
                .vertices=verts, .vertex_count=SCALE_VERTS,
                .indices=indices, .index_count=SCALE_IDXS,
                .object_id=g->handle_ids[a]});
        } else {
            MopVertex verts[TORUS_VERTS]; uint32_t indices[TORUS_IDXS];
            gen_rotate_handle(verts, indices, a, clr[0], clr[1], clr[2]);
            g->handles[a] = mop_viewport_add_mesh(vp, &(MopMeshDesc){
                .vertices=verts, .vertex_count=TORUS_VERTS,
                .indices=indices, .index_count=TORUS_IDXS,
                .object_id=g->handle_ids[a]});
        }
    }

    /* Center handle — yellow octahedron + 3 semi-transparent plane quads */
    {
        MopVertex cv[CENTER_VERTS]; uint32_t ci[CENTER_IDXS];
        gen_octahedron(cv, ci, 0.12f,
                       GIZMO_COLORS[3][0], GIZMO_COLORS[3][1],
                       GIZMO_COLORS[3][2]);
        /* XY plane quad (yellow: red+green blend) */
        int v1 = OCTA_VERTS, i1 = OCTA_IDXS;
        gen_plane_quad(cv + v1, ci + i1, 0, 1,
                       0.25f, 0.20f, 0.9f, 0.9f, 0.15f);
        for (int i = i1; i < i1 + QUAD_IDXS; i++) ci[i] += v1;
        /* XZ plane quad (magenta: red+blue blend) */
        int v2 = v1 + QUAD_VERTS, i2 = i1 + QUAD_IDXS;
        gen_plane_quad(cv + v2, ci + i2, 0, 2,
                       0.25f, 0.20f, 0.9f, 0.15f, 0.9f);
        for (int i = i2; i < i2 + QUAD_IDXS; i++) ci[i] += v2;
        /* YZ plane quad (cyan: green+blue blend) */
        int v3 = v2 + QUAD_VERTS, i3 = i2 + QUAD_IDXS;
        gen_plane_quad(cv + v3, ci + i3, 1, 2,
                       0.25f, 0.20f, 0.15f, 0.9f, 0.9f);
        for (int i = i3; i < i3 + QUAD_IDXS; i++) ci[i] += v3;
        g->handles[3] = mop_viewport_add_mesh(vp, &(MopMeshDesc){
            .vertices=cv, .vertex_count=CENTER_VERTS,
            .indices=ci, .index_count=CENTER_IDXS,
            .object_id=g->handle_ids[3]});
        mop_mesh_set_blend_mode(g->handles[3], MOP_BLEND_ALPHA);
    }

    update_handle_transforms(g);
}

static void destroy_handles(MopGizmo *g) {
    for (int a = 0; a < 4; a++) {
        if (g->handles[a]) {
            mop_viewport_remove_mesh(g->viewport, g->handles[a]);
            g->handles[a] = NULL;
        }
    }
}

/* -------------------------------------------------------------------------
 * Public API — Lifecycle
 * ------------------------------------------------------------------------- */

MopGizmo *mop_gizmo_create(MopViewport *viewport) {
    if (!viewport) return NULL;

    MopGizmo *g = calloc(1, sizeof(*g));
    if (!g) return NULL;

    g->viewport = viewport;
    g->mode     = MOP_GIZMO_TRANSLATE;
    g->visible  = false;

    /* Allocate unique handle IDs for this instance */
    uint32_t base = MOP_GIZMO_ID_BASE + gizmo_instance_counter * 8;
    gizmo_instance_counter++;
    for (int a = 0; a < 4; a++)
        g->handle_ids[a] = base + 1 + (uint32_t)a;

    return g;
}

void mop_gizmo_destroy(MopGizmo *gizmo) {
    if (!gizmo) return;
    if (gizmo->visible) destroy_handles(gizmo);
    free(gizmo);
}

/* -------------------------------------------------------------------------
 * Public API — Visibility
 * ------------------------------------------------------------------------- */

void mop_gizmo_show(MopGizmo *gizmo, MopVec3 position, MopMesh *target) {
    if (!gizmo) return;
    if (gizmo->visible) {
        /* Restore previous target opacity before switching */
        if (gizmo->target)
            mop_mesh_set_opacity(gizmo->target, 1.0f);
        destroy_handles(gizmo);
    }
    gizmo->position = position;
    gizmo->target   = target;
    gizmo->visible  = true;
    if (target)
        mop_mesh_set_opacity(target, GIZMO_SELECTION_OPACITY);
    create_handles(gizmo);
}

void mop_gizmo_hide(MopGizmo *gizmo) {
    if (!gizmo || !gizmo->visible) return;
    if (gizmo->target) {
        mop_mesh_set_opacity(gizmo->target, 1.0f);
        gizmo->target = NULL;
    }
    destroy_handles(gizmo);
    gizmo->visible = false;
}

/* -------------------------------------------------------------------------
 * Public API — Configuration
 * ------------------------------------------------------------------------- */

void mop_gizmo_set_mode(MopGizmo *gizmo, MopGizmoMode mode) {
    if (!gizmo || gizmo->mode == mode) return;
    gizmo->mode = mode;
    if (gizmo->visible) {
        destroy_handles(gizmo);
        create_handles(gizmo);
    }
}

MopGizmoMode mop_gizmo_get_mode(const MopGizmo *gizmo) {
    return gizmo ? gizmo->mode : MOP_GIZMO_TRANSLATE;
}

void mop_gizmo_set_position(MopGizmo *gizmo, MopVec3 position) {
    if (!gizmo) return;
    gizmo->position = position;
    if (gizmo->visible) update_handle_transforms(gizmo);
}

void mop_gizmo_set_rotation(MopGizmo *gizmo, MopVec3 rotation) {
    if (!gizmo) return;
    gizmo->rotation = rotation;
    if (gizmo->visible) update_handle_transforms(gizmo);
}

void mop_gizmo_update(MopGizmo *gizmo) {
    if (!gizmo || !gizmo->visible) return;
    update_handle_transforms(gizmo);
}

/* -------------------------------------------------------------------------
 * Public API — Picking
 * ------------------------------------------------------------------------- */

MopGizmoAxis mop_gizmo_test_pick(const MopGizmo *gizmo, MopPickResult pick) {
    if (!gizmo || !pick.hit) return MOP_GIZMO_AXIS_NONE;
    for (int a = 0; a < 4; a++) {
        if (pick.object_id == gizmo->handle_ids[a]) {
            return (MopGizmoAxis)a;
        }
    }
    return MOP_GIZMO_AXIS_NONE;
}

/* -------------------------------------------------------------------------
 * Public API — Drag
 * ------------------------------------------------------------------------- */

MopGizmoDelta mop_gizmo_drag(const MopGizmo *gizmo, MopGizmoAxis axis,
                              float mouse_dx, float mouse_dy) {
    MopGizmoDelta d = {{0,0,0}, {0,0,0}, {0,0,0}};
    if (!gizmo || axis == MOP_GIZMO_AXIS_NONE) return d;

    const MopViewport *vp = gizmo->viewport;

    /* Approximate camera distance for scaling mouse motion */
    MopVec3 to_pos = mop_vec3_sub(gizmo->position, vp->cam_eye);
    float cam_dist = mop_vec3_length(to_pos);
    if (cam_dist < 0.01f) cam_dist = 0.01f;

    if (gizmo->mode == MOP_GIZMO_TRANSLATE) {
        if (axis == MOP_GIZMO_AXIS_CENTER) {
            /* Center: move on camera plane */
            MopVec3 fwd = mop_vec3_normalize(
                mop_vec3_sub(vp->cam_target, vp->cam_eye));
            MopVec3 cam_r = mop_vec3_normalize(
                mop_vec3_cross(fwd, (MopVec3){0,1,0}));
            MopVec3 cam_u = mop_vec3_cross(cam_r, fwd);
            float s = cam_dist * 0.003f;
            d.translate = mop_vec3_add(
                mop_vec3_scale(cam_r,  mouse_dx * s),
                mop_vec3_scale(cam_u, -mouse_dy * s));
        } else {
            /* Project mouse motion onto the rotated axis screen direction */
            MopVec3 adir = axis_screen_dir(gizmo->position, (int)axis,
                                           gizmo->rotation, vp);
            float proj = mouse_dx*adir.x + mouse_dy*adir.y;
            float delta = proj * cam_dist * 0.003f;
            /* Move along the rotated world-space axis */
            MopVec3 world_dir = rotated_axis_dir((int)axis, gizmo->rotation);
            d.translate = mop_vec3_scale(world_dir, delta);
        }
    } else if (gizmo->mode == MOP_GIZMO_ROTATE) {
        if (axis == MOP_GIZMO_AXIS_CENTER) {
            d.rotate.y = mouse_dx * 0.01f;
        } else {
            /* Project the rotation axis into screen space.  Mouse motion
             * perpendicular to the projected axis drives the rotation. */
            MopVec3 adir = axis_screen_dir(gizmo->position, (int)axis,
                                           gizmo->rotation, vp);
            float perp_x = -adir.y;
            float perp_y =  adir.x;
            float proj = mouse_dx * perp_x + mouse_dy * perp_y;
            float delta = proj * 0.01f;
            if (axis == MOP_GIZMO_AXIS_X) d.rotate.x = delta;
            else if (axis == MOP_GIZMO_AXIS_Y) d.rotate.y = delta;
            else d.rotate.z = delta;
        }
    } else {
        /* Scale mode — scale is always in local space */
        if (axis == MOP_GIZMO_AXIS_CENTER) {
            float delta = mouse_dx * 0.005f;
            d.scale = (MopVec3){delta, delta, delta};
        } else {
            MopVec3 adir = axis_screen_dir(gizmo->position, (int)axis,
                                           gizmo->rotation, vp);
            float proj = mouse_dx*adir.x + mouse_dy*adir.y;
            float delta = proj * 0.005f;
            if (axis == MOP_GIZMO_AXIS_X) d.scale.x = delta;
            else if (axis == MOP_GIZMO_AXIS_Y) d.scale.y = delta;
            else d.scale.z = delta;
        }
    }

    return d;
}
