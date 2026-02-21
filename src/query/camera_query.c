/*
 * Master of Puppets — Camera Query API
 * camera_query.c — Camera matrix export and ray generation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"

#include <math.h>

/* -------------------------------------------------------------------------
 * Camera state snapshot
 * ------------------------------------------------------------------------- */

MopCameraState mop_viewport_get_camera_state(const MopViewport *vp) {
    MopCameraState s = {0};
    if (!vp) return s;
    s.eye               = vp->cam_eye;
    s.target            = vp->cam_target;
    s.up                = vp->cam_up;
    s.fov_radians       = vp->cam_fov_radians;
    s.near_plane        = vp->cam_near;
    s.far_plane         = vp->cam_far;
    s.aspect_ratio      = (vp->height > 0)
                          ? (float)vp->width / (float)vp->height
                          : 1.0f;
    s.view_matrix       = vp->view_matrix;
    s.projection_matrix = vp->projection_matrix;
    return s;
}

/* -------------------------------------------------------------------------
 * Individual getters
 * ------------------------------------------------------------------------- */

MopMat4 mop_viewport_get_view_matrix(const MopViewport *vp) {
    return vp ? vp->view_matrix : mop_mat4_identity();
}

MopMat4 mop_viewport_get_projection_matrix(const MopViewport *vp) {
    return vp ? vp->projection_matrix : mop_mat4_identity();
}

float mop_viewport_get_fov(const MopViewport *vp) {
    return vp ? vp->cam_fov_radians : 0.0f;
}

float mop_viewport_get_near_plane(const MopViewport *vp) {
    return vp ? vp->cam_near : 0.0f;
}

float mop_viewport_get_far_plane(const MopViewport *vp) {
    return vp ? vp->cam_far : 0.0f;
}

float mop_viewport_get_aspect_ratio(const MopViewport *vp) {
    if (!vp || vp->height <= 0) return 1.0f;
    return (float)vp->width / (float)vp->height;
}

MopVec3 mop_viewport_get_camera_up(const MopViewport *vp) {
    return vp ? vp->cam_up : (MopVec3){0, 1, 0};
}

/* -------------------------------------------------------------------------
 * Ray generation — unproject pixel to world-space ray
 * ------------------------------------------------------------------------- */

MopRay mop_viewport_pixel_to_ray(const MopViewport *vp, float x, float y) {
    MopRay ray = { .origin = {0,0,0}, .direction = {0,0,-1} };
    if (!vp || vp->width <= 0 || vp->height <= 0) return ray;

    /* Pixel to NDC */
    float ndc_x = (2.0f * x / (float)vp->width)  - 1.0f;
    float ndc_y = 1.0f - (2.0f * y / (float)vp->height);

    /* Inverse VP matrix */
    MopMat4 vp_mat = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);
    MopMat4 inv_vp = mop_mat4_inverse(vp_mat);

    /* Unproject near and far points */
    MopVec4 near_ndc = { ndc_x, ndc_y, -1.0f, 1.0f };
    MopVec4 far_ndc  = { ndc_x, ndc_y,  1.0f, 1.0f };

    MopVec4 near_world = mop_mat4_mul_vec4(inv_vp, near_ndc);
    MopVec4 far_world  = mop_mat4_mul_vec4(inv_vp, far_ndc);

    /* Perspective divide */
    if (fabsf(near_world.w) < 1e-8f || fabsf(far_world.w) < 1e-8f) return ray;

    float inv_nw = 1.0f / near_world.w;
    float inv_fw = 1.0f / far_world.w;

    MopVec3 near_pt = { near_world.x * inv_nw, near_world.y * inv_nw, near_world.z * inv_nw };
    MopVec3 far_pt  = { far_world.x * inv_fw,  far_world.y * inv_fw,  far_world.z * inv_fw };

    ray.origin    = near_pt;
    ray.direction = mop_vec3_normalize(mop_vec3_sub(far_pt, near_pt));
    return ray;
}
