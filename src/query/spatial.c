/*
 * Master of Puppets — Spatial Query Utilities
 * spatial.c — AABB, frustum, ray intersection, and CPU raycast
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"

#include <math.h>
#include <float.h>

/* -------------------------------------------------------------------------
 * Filter — same as query.c
 * ------------------------------------------------------------------------- */

static bool is_scene_mesh(const struct MopMesh *m) {
    return m->active && m->object_id != 0 && m->object_id < 0xFFFF0000u;
}

/* -------------------------------------------------------------------------
 * AABB — local space
 * ------------------------------------------------------------------------- */

MopAABB mop_mesh_get_aabb_local(const MopMesh *mesh, const MopViewport *vp) {
    MopAABB box = { .min = {0,0,0}, .max = {0,0,0} };
    if (!mesh || !vp || !mesh->vertex_buffer) return box;

    const void *raw = vp->rhi->buffer_read(mesh->vertex_buffer);
    if (!raw) return box;

    uint32_t count = mesh->vertex_count;
    if (count == 0) return box;

    if (mesh->vertex_format) {
        /* Flex format — find POSITION attribute */
        const MopVertexAttrib *pos_attr = mop_vertex_format_find(
            mesh->vertex_format, MOP_ATTRIB_POSITION);
        if (!pos_attr) return box;

        const uint8_t *bytes = (const uint8_t *)raw;
        uint32_t stride = mesh->vertex_format->stride;
        const float *first = (const float *)(bytes + pos_attr->offset);
        box.min = box.max = (MopVec3){ first[0], first[1], first[2] };

        for (uint32_t i = 1; i < count; i++) {
            const float *p = (const float *)(bytes + (size_t)i * stride
                                              + pos_attr->offset);
            if (p[0] < box.min.x) box.min.x = p[0];
            if (p[1] < box.min.y) box.min.y = p[1];
            if (p[2] < box.min.z) box.min.z = p[2];
            if (p[0] > box.max.x) box.max.x = p[0];
            if (p[1] > box.max.y) box.max.y = p[1];
            if (p[2] > box.max.z) box.max.z = p[2];
        }
    } else {
        /* Standard MopVertex layout */
        const MopVertex *verts = (const MopVertex *)raw;
        box.min = box.max = verts[0].position;
        for (uint32_t i = 1; i < count; i++) {
            MopVec3 p = verts[i].position;
            if (p.x < box.min.x) box.min.x = p.x;
            if (p.y < box.min.y) box.min.y = p.y;
            if (p.z < box.min.z) box.min.z = p.z;
            if (p.x > box.max.x) box.max.x = p.x;
            if (p.y > box.max.y) box.max.y = p.y;
            if (p.z > box.max.z) box.max.z = p.z;
        }
    }

    return box;
}

/* -------------------------------------------------------------------------
 * AABB — world space (transform 8 corners, re-fit)
 * ------------------------------------------------------------------------- */

MopAABB mop_mesh_get_aabb_world(const MopMesh *mesh, const MopViewport *vp) {
    MopAABB local = mop_mesh_get_aabb_local(mesh, vp);
    if (!mesh) return local;

    MopMat4 w = mesh->world_transform;

    /* 8 corners of the local AABB */
    MopVec3 corners[8] = {
        { local.min.x, local.min.y, local.min.z },
        { local.max.x, local.min.y, local.min.z },
        { local.min.x, local.max.y, local.min.z },
        { local.max.x, local.max.y, local.min.z },
        { local.min.x, local.min.y, local.max.z },
        { local.max.x, local.min.y, local.max.z },
        { local.min.x, local.max.y, local.max.z },
        { local.max.x, local.max.y, local.max.z },
    };

    MopVec4 first = mop_mat4_mul_vec4(w,
        (MopVec4){ corners[0].x, corners[0].y, corners[0].z, 1.0f });
    MopAABB result = {
        .min = { first.x, first.y, first.z },
        .max = { first.x, first.y, first.z }
    };

    for (int i = 1; i < 8; i++) {
        MopVec4 tp = mop_mat4_mul_vec4(w,
            (MopVec4){ corners[i].x, corners[i].y, corners[i].z, 1.0f });
        if (tp.x < result.min.x) result.min.x = tp.x;
        if (tp.y < result.min.y) result.min.y = tp.y;
        if (tp.z < result.min.z) result.min.z = tp.z;
        if (tp.x > result.max.x) result.max.x = tp.x;
        if (tp.y > result.max.y) result.max.y = tp.y;
        if (tp.z > result.max.z) result.max.z = tp.z;
    }

    return result;
}

/* -------------------------------------------------------------------------
 * AABB utilities
 * ------------------------------------------------------------------------- */

MopAABB mop_aabb_union(MopAABB a, MopAABB b) {
    MopAABB r;
    r.min.x = (a.min.x < b.min.x) ? a.min.x : b.min.x;
    r.min.y = (a.min.y < b.min.y) ? a.min.y : b.min.y;
    r.min.z = (a.min.z < b.min.z) ? a.min.z : b.min.z;
    r.max.x = (a.max.x > b.max.x) ? a.max.x : b.max.x;
    r.max.y = (a.max.y > b.max.y) ? a.max.y : b.max.y;
    r.max.z = (a.max.z > b.max.z) ? a.max.z : b.max.z;
    return r;
}

bool mop_aabb_overlaps(MopAABB a, MopAABB b) {
    if (a.max.x < b.min.x || a.min.x > b.max.x) return false;
    if (a.max.y < b.min.y || a.min.y > b.max.y) return false;
    if (a.max.z < b.min.z || a.min.z > b.max.z) return false;
    return true;
}

MopVec3 mop_aabb_center(MopAABB box) {
    return (MopVec3){
        (box.min.x + box.max.x) * 0.5f,
        (box.min.y + box.max.y) * 0.5f,
        (box.min.z + box.max.z) * 0.5f
    };
}

MopVec3 mop_aabb_extents(MopAABB box) {
    return (MopVec3){
        (box.max.x - box.min.x) * 0.5f,
        (box.max.y - box.min.y) * 0.5f,
        (box.max.z - box.min.z) * 0.5f
    };
}

float mop_aabb_surface_area(MopAABB box) {
    float dx = box.max.x - box.min.x;
    float dy = box.max.y - box.min.y;
    float dz = box.max.z - box.min.z;
    return 2.0f * (dx * dy + dy * dz + dz * dx);
}

/* -------------------------------------------------------------------------
 * Frustum — Gribb-Hartmann plane extraction
 * ------------------------------------------------------------------------- */

MopFrustum mop_viewport_get_frustum(const MopViewport *vp) {
    MopFrustum f = {{{0}}};
    if (!vp) return f;

    MopMat4 vp_mat = mop_mat4_multiply(vp->projection_matrix,
                                        vp->view_matrix);

    /* Column-major: M(row, col) = d[col * 4 + row] */
    #define R(r, c) vp_mat.d[(c) * 4 + (r)]

    /* Left:   row3 + row0 */
    f.planes[0] = (MopVec4){ R(3,0)+R(0,0), R(3,1)+R(0,1),
                              R(3,2)+R(0,2), R(3,3)+R(0,3) };
    /* Right:  row3 - row0 */
    f.planes[1] = (MopVec4){ R(3,0)-R(0,0), R(3,1)-R(0,1),
                              R(3,2)-R(0,2), R(3,3)-R(0,3) };
    /* Bottom: row3 + row1 */
    f.planes[2] = (MopVec4){ R(3,0)+R(1,0), R(3,1)+R(1,1),
                              R(3,2)+R(1,2), R(3,3)+R(1,3) };
    /* Top:    row3 - row1 */
    f.planes[3] = (MopVec4){ R(3,0)-R(1,0), R(3,1)-R(1,1),
                              R(3,2)-R(1,2), R(3,3)-R(1,3) };
    /* Near:   row3 + row2 */
    f.planes[4] = (MopVec4){ R(3,0)+R(2,0), R(3,1)+R(2,1),
                              R(3,2)+R(2,2), R(3,3)+R(2,3) };
    /* Far:    row3 - row2 */
    f.planes[5] = (MopVec4){ R(3,0)-R(2,0), R(3,1)-R(2,1),
                              R(3,2)-R(2,2), R(3,3)-R(2,3) };

    #undef R

    /* Normalize each plane */
    for (int i = 0; i < 6; i++) {
        float len = sqrtf(f.planes[i].x * f.planes[i].x +
                          f.planes[i].y * f.planes[i].y +
                          f.planes[i].z * f.planes[i].z);
        if (len > 1e-8f) {
            float inv = 1.0f / len;
            f.planes[i].x *= inv;
            f.planes[i].y *= inv;
            f.planes[i].z *= inv;
            f.planes[i].w *= inv;
        }
    }

    return f;
}

int mop_frustum_test_aabb(const MopFrustum *frustum, MopAABB box) {
    if (!frustum) return -1;

    bool all_inside = true;

    for (int i = 0; i < 6; i++) {
        float nx = frustum->planes[i].x;
        float ny = frustum->planes[i].y;
        float nz = frustum->planes[i].z;
        float d  = frustum->planes[i].w;

        /* Positive vertex: the corner most in the direction of the normal */
        MopVec3 pv = {
            (nx >= 0) ? box.max.x : box.min.x,
            (ny >= 0) ? box.max.y : box.min.y,
            (nz >= 0) ? box.max.z : box.min.z,
        };

        /* Negative vertex: the opposite corner */
        MopVec3 nv = {
            (nx >= 0) ? box.min.x : box.max.x,
            (ny >= 0) ? box.min.y : box.max.y,
            (nz >= 0) ? box.min.z : box.max.z,
        };

        /* If positive vertex is outside, entire AABB is outside */
        if (nx * pv.x + ny * pv.y + nz * pv.z + d < 0) return -1;

        /* If negative vertex is outside, AABB intersects this plane */
        if (nx * nv.x + ny * nv.y + nz * nv.z + d < 0) all_inside = false;
    }

    return all_inside ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Ray-AABB intersection — slab method
 * ------------------------------------------------------------------------- */

bool mop_ray_intersect_aabb(MopRay ray, MopAABB box,
                             float *t_near, float *t_far) {
    float tmin = -FLT_MAX;
    float tmax =  FLT_MAX;

    float dir[3] = { ray.direction.x, ray.direction.y, ray.direction.z };
    float org[3] = { ray.origin.x, ray.origin.y, ray.origin.z };
    float bmin[3] = { box.min.x, box.min.y, box.min.z };
    float bmax[3] = { box.max.x, box.max.y, box.max.z };

    for (int i = 0; i < 3; i++) {
        if (fabsf(dir[i]) < 1e-8f) {
            /* Ray parallel to slab — check if origin inside */
            if (org[i] < bmin[i] || org[i] > bmax[i]) return false;
        } else {
            float inv_d = 1.0f / dir[i];
            float t1 = (bmin[i] - org[i]) * inv_d;
            float t2 = (bmax[i] - org[i]) * inv_d;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }

    if (t_near) *t_near = tmin;
    if (t_far)  *t_far  = tmax;
    return true;
}

/* -------------------------------------------------------------------------
 * Ray-triangle intersection — Moller-Trumbore
 * ------------------------------------------------------------------------- */

bool mop_ray_intersect_triangle(MopRay ray,
                                 MopVec3 v0, MopVec3 v1, MopVec3 v2,
                                 float *out_t, float *out_u, float *out_v) {
    MopVec3 e1 = mop_vec3_sub(v1, v0);
    MopVec3 e2 = mop_vec3_sub(v2, v0);
    MopVec3 h  = mop_vec3_cross(ray.direction, e2);
    float a    = mop_vec3_dot(e1, h);

    if (a > -1e-7f && a < 1e-7f) return false;

    float f = 1.0f / a;
    MopVec3 s = mop_vec3_sub(ray.origin, v0);
    float u = f * mop_vec3_dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;

    MopVec3 q = mop_vec3_cross(s, e1);
    float v = f * mop_vec3_dot(ray.direction, q);
    if (v < 0.0f || u + v > 1.0f) return false;

    float t = f * mop_vec3_dot(e2, q);
    if (t < 1e-6f) return false;  /* intersection behind ray origin */

    if (out_t) *out_t = t;
    if (out_u) *out_u = u;
    if (out_v) *out_v = v;
    return true;
}

/* -------------------------------------------------------------------------
 * Scene AABB
 * ------------------------------------------------------------------------- */

MopAABB mop_viewport_get_scene_aabb(const MopViewport *vp) {
    MopAABB scene = { .min = {0,0,0}, .max = {0,0,0} };
    if (!vp) return scene;

    bool first = true;
    for (uint32_t i = 0; i < vp->mesh_count; i++) {
        if (!is_scene_mesh(&vp->meshes[i])) continue;
        MopAABB mb = mop_mesh_get_aabb_world(&vp->meshes[i], vp);
        if (first) {
            scene = mb;
            first = false;
        } else {
            scene = mop_aabb_union(scene, mb);
        }
    }
    return scene;
}

uint32_t mop_viewport_visible_mesh_count(const MopViewport *vp) {
    if (!vp) return 0;
    MopFrustum frustum = mop_viewport_get_frustum(vp);
    uint32_t count = 0;
    for (uint32_t i = 0; i < vp->mesh_count; i++) {
        if (!is_scene_mesh(&vp->meshes[i])) continue;
        MopAABB wb = mop_mesh_get_aabb_world(&vp->meshes[i], vp);
        if (mop_frustum_test_aabb(&frustum, wb) >= 0) count++;
    }
    return count;
}

/* -------------------------------------------------------------------------
 * CPU raycast — AABB broadphase + triangle narrowphase
 * ------------------------------------------------------------------------- */

MopRayHit mop_viewport_raycast_ray(const MopViewport *vp, MopRay ray) {
    MopRayHit result = { .hit = false };
    if (!vp) return result;

    float closest_t = FLT_MAX;

    for (uint32_t mi = 0; mi < vp->mesh_count; mi++) {
        const struct MopMesh *mesh = &vp->meshes[mi];
        if (!is_scene_mesh(mesh)) continue;

        /* Broadphase: ray vs world AABB */
        MopAABB wb = mop_mesh_get_aabb_world(mesh, vp);
        float aabb_t_near, aabb_t_far;
        if (!mop_ray_intersect_aabb(ray, wb, &aabb_t_near, &aabb_t_far))
            continue;
        if (aabb_t_near > closest_t) continue;  /* can't beat current best */

        /* Narrowphase: ray vs each triangle in world space */
        if (!mesh->vertex_buffer || !mesh->index_buffer) continue;
        if (mesh->vertex_format) continue;  /* skip flex-format for now */

        const MopVertex *verts =
            (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
        const uint32_t *indices =
            (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
        if (!verts || !indices) continue;

        MopMat4 w = mesh->world_transform;
        uint32_t tri_count = mesh->index_count / 3;

        for (uint32_t ti = 0; ti < tri_count; ti++) {
            uint32_t i0 = indices[ti * 3 + 0];
            uint32_t i1 = indices[ti * 3 + 1];
            uint32_t i2 = indices[ti * 3 + 2];
            if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count ||
                i2 >= mesh->vertex_count)
                continue;

            /* Transform to world space */
            MopVec4 wp0 = mop_mat4_mul_vec4(w,
                (MopVec4){ verts[i0].position.x, verts[i0].position.y,
                           verts[i0].position.z, 1.0f });
            MopVec4 wp1 = mop_mat4_mul_vec4(w,
                (MopVec4){ verts[i1].position.x, verts[i1].position.y,
                           verts[i1].position.z, 1.0f });
            MopVec4 wp2 = mop_mat4_mul_vec4(w,
                (MopVec4){ verts[i2].position.x, verts[i2].position.y,
                           verts[i2].position.z, 1.0f });

            MopVec3 p0 = { wp0.x, wp0.y, wp0.z };
            MopVec3 p1 = { wp1.x, wp1.y, wp1.z };
            MopVec3 p2 = { wp2.x, wp2.y, wp2.z };

            float t, u, v;
            if (mop_ray_intersect_triangle(ray, p0, p1, p2, &t, &u, &v)) {
                if (t < closest_t) {
                    closest_t = t;
                    result.hit            = true;
                    result.object_id      = mesh->object_id;
                    result.distance       = t;
                    result.position       = mop_vec3_add(ray.origin,
                        mop_vec3_scale(ray.direction, t));
                    result.u              = u;
                    result.v              = v;
                    result.triangle_index = ti;

                    /* Compute face normal from world-space triangle */
                    MopVec3 e1 = mop_vec3_sub(p1, p0);
                    MopVec3 e2 = mop_vec3_sub(p2, p0);
                    result.normal = mop_vec3_normalize(
                        mop_vec3_cross(e1, e2));
                }
            }
        }
    }

    return result;
}

MopRayHit mop_viewport_raycast(const MopViewport *vp,
                                float pixel_x, float pixel_y) {
    MopRayHit result = { .hit = false };
    if (!vp) return result;
    MopRay ray = mop_viewport_pixel_to_ray(vp, pixel_x, pixel_y);
    return mop_viewport_raycast_ray(vp, ray);
}
