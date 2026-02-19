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

/* Ring geometry — 128 segments for very smooth circles */
#define RING_SEGS  128
#define RING_VERTS (RING_SEGS * 24)
#define RING_IDXS  (RING_SEGS * 36)

/* Static counter for unique gizmo IDs across multiple instances */
static uint32_t gizmo_instance_counter = 0;

/* -------------------------------------------------------------------------
 * Gizmo structure
 * ------------------------------------------------------------------------- */

struct MopGizmo {
    MopViewport  *viewport;
    MopGizmoMode  mode;
    MopVec3       position;
    MopVec3       rotation;        /* local-space euler angles */
    bool          visible;
    MopMesh      *handles[4];      /* X, Y, Z, Center */
    uint32_t      handle_ids[4];   /* unique per gizmo instance */
};

/* -------------------------------------------------------------------------
 * Handle colors:  X=red, Y=green, Z=blue, Center=white
 * ------------------------------------------------------------------------- */

static const float GIZMO_COLORS[4][3] = {
    { 0.9f, 0.15f, 0.15f },   /* X — red   */
    { 0.15f, 0.9f, 0.15f },   /* Y — green */
    { 0.15f, 0.15f, 0.9f },   /* Z — blue  */
    { 0.9f, 0.9f, 0.9f }      /* Center — white */
};

/* -------------------------------------------------------------------------
 * Rotation helpers
 * ------------------------------------------------------------------------- */

/* Build rotation matrix from euler angles (same order as compose_trs) */
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

static void gen_colored_box(MopVertex *v, uint32_t *idx,
                            float sx, float sy, float sz,
                            float cx, float cy, float cz,
                            float cr, float cg, float cb) {
    MopVec3 c[8] = {
        {cx-sx,cy-sy,cz-sz}, {cx+sx,cy-sy,cz-sz},
        {cx+sx,cy+sy,cz-sz}, {cx-sx,cy+sy,cz-sz},
        {cx-sx,cy-sy,cz+sz}, {cx+sx,cy-sy,cz+sz},
        {cx+sx,cy+sy,cz+sz}, {cx-sx,cy+sy,cz+sz}
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
            v[base+j] = (MopVertex){c[fi[f][j]],
                {fn[f][0],fn[f][1],fn[f][2]}, col};
        int ib = f * 6;
        idx[ib+0]=base; idx[ib+1]=base+1; idx[ib+2]=base+2;
        idx[ib+3]=base+2; idx[ib+4]=base+3; idx[ib+5]=base;
    }
}

static void gen_translate_handle(MopVertex *v, uint32_t *idx, int axis,
                                 float cr, float cg, float cb) {
    /* Shaft extends from 0.55 to 1.35, arrow head at 1.45 */
    float sc[3]={.02f,.02f,.02f}, cc[3]={0,0,0};
    float as[3]={.055f,.055f,.055f}, ac[3]={0,0,0};
    sc[axis]=.40f; cc[axis]=.95f;
    as[axis]=.10f; ac[axis]=1.45f;
    gen_colored_box(v, idx, sc[0],sc[1],sc[2], cc[0],cc[1],cc[2], cr,cg,cb);
    gen_colored_box(v+24, idx+36, as[0],as[1],as[2], ac[0],ac[1],ac[2],
                    cr,cg,cb);
    for (int i = 36; i < 72; i++) idx[i] += 24;
}

static void gen_scale_handle(MopVertex *v, uint32_t *idx, int axis,
                             float cr, float cg, float cb) {
    /* Shaft extends from 0.55 to 1.35, cube endpoint at 1.40 */
    float sc[3]={.02f,.02f,.02f}, cc[3]={0,0,0};
    float ec[3]={0,0,0};
    sc[axis]=.40f; cc[axis]=.95f; ec[axis]=1.40f;
    gen_colored_box(v, idx, sc[0],sc[1],sc[2], cc[0],cc[1],cc[2], cr,cg,cb);
    gen_colored_box(v+24, idx+36, .055f,.055f,.055f, ec[0],ec[1],ec[2],
                    cr,cg,cb);
    for (int i = 36; i < 72; i++) idx[i] += 24;
}

static void gen_rotate_handle(MopVertex *v, uint32_t *idx, int axis,
                              float cr, float cg, float cb) {
    float r_in = 1.00f, r_out = 1.08f, ht = 0.018f;
    MopColor col = {cr, cg, cb, 1};
    for (int i = 0; i < RING_SEGS; i++) {
        float a0 = i * 2.0f*PI/RING_SEGS;
        float a1 = (i+1) * 2.0f*PI/RING_SEGS;
        float c0=cosf(a0), s0=sinf(a0), c1=cosf(a1), s1=sinf(a1);
        MopVec3 p[8];
        if (axis == 0) {
            p[0]=(MopVec3){-ht, r_in*c0,  r_in*s0};
            p[1]=(MopVec3){-ht, r_out*c0, r_out*s0};
            p[2]=(MopVec3){-ht, r_out*c1, r_out*s1};
            p[3]=(MopVec3){-ht, r_in*c1,  r_in*s1};
            p[4]=(MopVec3){ ht, r_in*c0,  r_in*s0};
            p[5]=(MopVec3){ ht, r_out*c0, r_out*s0};
            p[6]=(MopVec3){ ht, r_out*c1, r_out*s1};
            p[7]=(MopVec3){ ht, r_in*c1,  r_in*s1};
        } else if (axis == 1) {
            p[0]=(MopVec3){r_in*c0,  -ht, r_in*s0};
            p[1]=(MopVec3){r_out*c0, -ht, r_out*s0};
            p[2]=(MopVec3){r_out*c1, -ht, r_out*s1};
            p[3]=(MopVec3){r_in*c1,  -ht, r_in*s1};
            p[4]=(MopVec3){r_in*c0,   ht, r_in*s0};
            p[5]=(MopVec3){r_out*c0,  ht, r_out*s0};
            p[6]=(MopVec3){r_out*c1,  ht, r_out*s1};
            p[7]=(MopVec3){r_in*c1,   ht, r_in*s1};
        } else {
            p[0]=(MopVec3){r_in*c0,  r_in*s0,  -ht};
            p[1]=(MopVec3){r_out*c0, r_out*s0, -ht};
            p[2]=(MopVec3){r_out*c1, r_out*s1, -ht};
            p[3]=(MopVec3){r_in*c1,  r_in*s1,  -ht};
            p[4]=(MopVec3){r_in*c0,  r_in*s0,   ht};
            p[5]=(MopVec3){r_out*c0, r_out*s0,  ht};
            p[6]=(MopVec3){r_out*c1, r_out*s1,  ht};
            p[7]=(MopVec3){r_in*c1,  r_in*s1,   ht};
        }
        static const int fi[6][4] = {
            {4,5,6,7}, {1,0,3,2}, {5,1,2,6}, {0,4,7,3}, {7,6,2,3}, {0,1,5,4}
        };
        int vb = i * 24, ib_base = i * 36;
        for (int f = 0; f < 6; f++) {
            MopVec3 e1 = mop_vec3_sub(p[fi[f][1]], p[fi[f][0]]);
            MopVec3 e2 = mop_vec3_sub(p[fi[f][3]], p[fi[f][0]]);
            MopVec3 n = mop_vec3_normalize(mop_vec3_cross(e1, e2));
            for (int j = 0; j < 4; j++)
                v[vb+f*4+j] = (MopVertex){p[fi[f][j]], n, col};
            int fb = vb+f*4, ib2 = ib_base+f*6;
            idx[ib2+0]=fb; idx[ib2+1]=fb+1; idx[ib2+2]=fb+2;
            idx[ib2+3]=fb+2; idx[ib2+4]=fb+3; idx[ib2+5]=fb;
        }
    }
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
    MopMat4 r  = gizmo_rotation_matrix(g->rotation);
    MopMat4 t  = mop_mat4_translate(g->position);
    MopMat4 tr = mop_mat4_multiply(t, r);
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
            MopVertex verts[48]; uint32_t indices[72];
            gen_translate_handle(verts, indices, a, clr[0], clr[1], clr[2]);
            g->handles[a] = mop_viewport_add_mesh(vp, &(MopMeshDesc){
                .vertices=verts, .vertex_count=48,
                .indices=indices, .index_count=72,
                .object_id=g->handle_ids[a]});
        } else if (g->mode == MOP_GIZMO_SCALE) {
            MopVertex verts[48]; uint32_t indices[72];
            gen_scale_handle(verts, indices, a, clr[0], clr[1], clr[2]);
            g->handles[a] = mop_viewport_add_mesh(vp, &(MopMeshDesc){
                .vertices=verts, .vertex_count=48,
                .indices=indices, .index_count=72,
                .object_id=g->handle_ids[a]});
        } else {
            MopVertex verts[RING_VERTS]; uint32_t indices[RING_IDXS];
            gen_rotate_handle(verts, indices, a, clr[0], clr[1], clr[2]);
            g->handles[a] = mop_viewport_add_mesh(vp, &(MopMeshDesc){
                .vertices=verts, .vertex_count=RING_VERTS,
                .indices=indices, .index_count=RING_IDXS,
                .object_id=g->handle_ids[a]});
        }
    }

    /* Center handle — three short bars crossing at origin, visible outside
     * typical objects.  Each bar extends ±0.70 along one axis. */
    {
        /* 3 bars × 24 verts = 72 verts, 3 bars × 36 idx = 108 idx */
        MopVertex cv[72]; uint32_t ci[108];
        float w = 0.03f, ext = 0.70f;
        gen_colored_box(cv,    ci,    ext, w, w,  0,0,0,
                        GIZMO_COLORS[3][0], GIZMO_COLORS[3][1],
                        GIZMO_COLORS[3][2]);
        gen_colored_box(cv+24, ci+36, w, ext, w,  0,0,0,
                        GIZMO_COLORS[3][0], GIZMO_COLORS[3][1],
                        GIZMO_COLORS[3][2]);
        for (int i = 36; i < 72; i++) ci[i] += 24;
        gen_colored_box(cv+48, ci+72, w, w, ext,  0,0,0,
                        GIZMO_COLORS[3][0], GIZMO_COLORS[3][1],
                        GIZMO_COLORS[3][2]);
        for (int i = 72; i < 108; i++) ci[i] += 48;
        g->handles[3] = mop_viewport_add_mesh(vp, &(MopMeshDesc){
            .vertices=cv, .vertex_count=72,
            .indices=ci, .index_count=108,
            .object_id=g->handle_ids[3]});
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

void mop_gizmo_show(MopGizmo *gizmo, MopVec3 position) {
    if (!gizmo) return;
    if (gizmo->visible) destroy_handles(gizmo);
    gizmo->position = position;
    gizmo->visible  = true;
    create_handles(gizmo);
}

void mop_gizmo_hide(MopGizmo *gizmo) {
    if (!gizmo || !gizmo->visible) return;
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
        float delta = mouse_dx * 0.01f;
        if (axis == MOP_GIZMO_AXIS_CENTER) {
            d.rotate.y = delta;
        } else {
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
