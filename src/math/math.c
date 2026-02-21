/*
 * Master of Puppets — Math Library
 * math.c — Vector and matrix operations
 *
 * All matrices are column-major.  Flat index: d[col * 4 + row].
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/types.h>
#include <mop/log.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Vec3
 * ------------------------------------------------------------------------- */

MopVec3 mop_vec3_add(MopVec3 a, MopVec3 b) {
    return (MopVec3){ a.x + b.x, a.y + b.y, a.z + b.z };
}

MopVec3 mop_vec3_sub(MopVec3 a, MopVec3 b) {
    return (MopVec3){ a.x - b.x, a.y - b.y, a.z - b.z };
}

MopVec3 mop_vec3_scale(MopVec3 v, float s) {
    return (MopVec3){ v.x * s, v.y * s, v.z * s };
}

MopVec3 mop_vec3_cross(MopVec3 a, MopVec3 b) {
    return (MopVec3){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

float mop_vec3_dot(MopVec3 a, MopVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float mop_vec3_length(MopVec3 v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

MopVec3 mop_vec3_normalize(MopVec3 v) {
    float len = mop_vec3_length(v);
    if (len < 1e-8f) {
        return (MopVec3){ 0.0f, 0.0f, 0.0f };
    }
    float inv = 1.0f / len;
    return (MopVec3){ v.x * inv, v.y * inv, v.z * inv };
}

/* -------------------------------------------------------------------------
 * Mat4 — column-major
 *
 * Index macro: element at row r, column c = d[c * 4 + r]
 * ------------------------------------------------------------------------- */

#define M(mat, r, c) ((mat).d[(c) * 4 + (r)])

MopMat4 mop_mat4_identity(void) {
    MopMat4 m = {0};
    M(m, 0, 0) = 1.0f;
    M(m, 1, 1) = 1.0f;
    M(m, 2, 2) = 1.0f;
    M(m, 3, 3) = 1.0f;
    return m;
}

MopMat4 mop_mat4_perspective(float fov_radians, float aspect,
                             float near_plane, float far_plane) {
    float tan_half_fov = tanf(fov_radians * 0.5f);
    MopMat4 m = {0};
    M(m, 0, 0) = 1.0f / (aspect * tan_half_fov);
    M(m, 1, 1) = 1.0f / tan_half_fov;
    M(m, 2, 2) = -(far_plane + near_plane) / (far_plane - near_plane);
    M(m, 3, 2) = -1.0f;
    M(m, 2, 3) = -(2.0f * far_plane * near_plane) / (far_plane - near_plane);
    return m;
}

MopMat4 mop_mat4_look_at(MopVec3 eye, MopVec3 center, MopVec3 up) {
    MopVec3 f = mop_vec3_normalize(mop_vec3_sub(center, eye));
    MopVec3 s = mop_vec3_normalize(mop_vec3_cross(f, up));
    MopVec3 u = mop_vec3_cross(s, f);

    MopMat4 m = mop_mat4_identity();
    M(m, 0, 0) =  s.x;
    M(m, 0, 1) =  s.y;
    M(m, 0, 2) =  s.z;
    M(m, 1, 0) =  u.x;
    M(m, 1, 1) =  u.y;
    M(m, 1, 2) =  u.z;
    M(m, 2, 0) = -f.x;
    M(m, 2, 1) = -f.y;
    M(m, 2, 2) = -f.z;
    M(m, 0, 3) = -mop_vec3_dot(s, eye);
    M(m, 1, 3) = -mop_vec3_dot(u, eye);
    M(m, 2, 3) =  mop_vec3_dot(f, eye);
    return m;
}

MopMat4 mop_mat4_rotate_y(float angle_radians) {
    float c = cosf(angle_radians);
    float s = sinf(angle_radians);
    MopMat4 m = mop_mat4_identity();
    M(m, 0, 0) =  c;
    M(m, 0, 2) =  s;
    M(m, 2, 0) = -s;
    M(m, 2, 2) =  c;
    return m;
}

MopMat4 mop_mat4_rotate_x(float angle_radians) {
    float c = cosf(angle_radians);
    float s = sinf(angle_radians);
    MopMat4 m = mop_mat4_identity();
    M(m, 1, 1) =  c;
    M(m, 1, 2) = -s;
    M(m, 2, 1) =  s;
    M(m, 2, 2) =  c;
    return m;
}

MopMat4 mop_mat4_rotate_z(float angle_radians) {
    float c = cosf(angle_radians);
    float s = sinf(angle_radians);
    MopMat4 m = mop_mat4_identity();
    M(m, 0, 0) =  c;
    M(m, 0, 1) = -s;
    M(m, 1, 0) =  s;
    M(m, 1, 1) =  c;
    return m;
}

MopMat4 mop_mat4_translate(MopVec3 offset) {
    MopMat4 m = mop_mat4_identity();
    M(m, 0, 3) = offset.x;
    M(m, 1, 3) = offset.y;
    M(m, 2, 3) = offset.z;
    return m;
}

MopMat4 mop_mat4_scale(MopVec3 s) {
    MopMat4 m = {0};
    M(m, 0, 0) = s.x;
    M(m, 1, 1) = s.y;
    M(m, 2, 2) = s.z;
    M(m, 3, 3) = 1.0f;
    return m;
}

MopMat4 mop_mat4_multiply(MopMat4 a, MopMat4 b) {
    MopMat4 r = {0};
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += M(a, row, k) * M(b, k, c);
            }
            M(r, row, c) = sum;
        }
    }
    return r;
}

MopVec4 mop_mat4_mul_vec4(MopMat4 m, MopVec4 v) {
    return (MopVec4){
        M(m, 0, 0) * v.x + M(m, 0, 1) * v.y + M(m, 0, 2) * v.z + M(m, 0, 3) * v.w,
        M(m, 1, 0) * v.x + M(m, 1, 1) * v.y + M(m, 1, 2) * v.z + M(m, 1, 3) * v.w,
        M(m, 2, 0) * v.x + M(m, 2, 1) * v.y + M(m, 2, 2) * v.z + M(m, 2, 3) * v.w,
        M(m, 3, 0) * v.x + M(m, 3, 1) * v.y + M(m, 3, 2) * v.z + M(m, 3, 3) * v.w
    };
}

MopMat4 mop_mat4_compose_trs(MopVec3 position, MopVec3 rotation, MopVec3 scale) {
    MopMat4 s  = mop_mat4_scale(scale);
    MopMat4 rx = mop_mat4_rotate_x(rotation.x);
    MopMat4 ry = mop_mat4_rotate_y(rotation.y);
    MopMat4 rz = mop_mat4_rotate_z(rotation.z);
    MopMat4 t  = mop_mat4_translate(position);
    return mop_mat4_multiply(t,
           mop_mat4_multiply(rz,
           mop_mat4_multiply(ry,
           mop_mat4_multiply(rx, s))));
}

/* -------------------------------------------------------------------------
 * 4x4 matrix inverse — cofactor expansion
 *
 * Returns identity if the matrix is singular (determinant near zero).
 * ------------------------------------------------------------------------- */

MopMat4 mop_mat4_inverse(MopMat4 m) {
    float *a = m.d;
    MopMat4 inv;
    float *o = inv.d;

    /* Cofactors for each element */
    o[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15]
           + a[9]*a[7]*a[14]  + a[13]*a[6]*a[11]  - a[13]*a[7]*a[10];
    o[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14]  + a[8]*a[6]*a[15]
           - a[8]*a[7]*a[14]  - a[12]*a[6]*a[11]  + a[12]*a[7]*a[10];
    o[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13]  - a[8]*a[5]*a[15]
           + a[8]*a[7]*a[13]  + a[12]*a[5]*a[11]  - a[12]*a[7]*a[9];
    o[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13]  + a[8]*a[5]*a[14]
           - a[8]*a[6]*a[13]  - a[12]*a[5]*a[10]  + a[12]*a[6]*a[9];

    o[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14]  + a[9]*a[2]*a[15]
           - a[9]*a[3]*a[14]  - a[13]*a[2]*a[11]  + a[13]*a[3]*a[10];
    o[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14]  - a[8]*a[2]*a[15]
           + a[8]*a[3]*a[14]  + a[12]*a[2]*a[11]  - a[12]*a[3]*a[10];
    o[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13]  + a[8]*a[1]*a[15]
           - a[8]*a[3]*a[13]  - a[12]*a[1]*a[11]  + a[12]*a[3]*a[9];
    o[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13]  - a[8]*a[1]*a[14]
           + a[8]*a[2]*a[13]  + a[12]*a[1]*a[10]  - a[12]*a[2]*a[9];

    o[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14]   - a[5]*a[2]*a[15]
           + a[5]*a[3]*a[14]  + a[13]*a[2]*a[7]   - a[13]*a[3]*a[6];
    o[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14]   + a[4]*a[2]*a[15]
           - a[4]*a[3]*a[14]  - a[12]*a[2]*a[7]   + a[12]*a[3]*a[6];
    o[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13]   - a[4]*a[1]*a[15]
           + a[4]*a[3]*a[13]  + a[12]*a[1]*a[7]   - a[12]*a[3]*a[5];
    o[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13]   + a[4]*a[1]*a[14]
           - a[4]*a[2]*a[13]  - a[12]*a[1]*a[6]   + a[12]*a[2]*a[5];

    o[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10]   + a[5]*a[2]*a[11]
           - a[5]*a[3]*a[10]  - a[9]*a[2]*a[7]    + a[9]*a[3]*a[6];
    o[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10]   - a[4]*a[2]*a[11]
           + a[4]*a[3]*a[10]  + a[8]*a[2]*a[7]    - a[8]*a[3]*a[6];
    o[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9]    + a[4]*a[1]*a[11]
           - a[4]*a[3]*a[9]   - a[8]*a[1]*a[7]    + a[8]*a[3]*a[5];
    o[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9]    - a[4]*a[1]*a[10]
           + a[4]*a[2]*a[9]   + a[8]*a[1]*a[6]    - a[8]*a[2]*a[5];

    float det = a[0]*o[0] + a[1]*o[4] + a[2]*o[8] + a[3]*o[12];
    if (fabsf(det) < 1e-8f) {
        MOP_WARN("singular matrix in inverse (det=%.2e)", det);
        return mop_mat4_identity();
    }

    float inv_det = 1.0f / det;
    for (int i = 0; i < 16; i++) o[i] *= inv_det;

    return inv;
}

#undef M
