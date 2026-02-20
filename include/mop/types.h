/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * types.h — Common value types
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_TYPES_H
#define MOP_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Vector types
 * ------------------------------------------------------------------------- */

typedef struct MopVec3 {
    float x, y, z;
} MopVec3;

typedef struct MopVec4 {
    float x, y, z, w;
} MopVec4;

/* -------------------------------------------------------------------------
 * Matrix — column-major 4x4, OpenGL convention
 *
 * Layout: m[col][row]
 *   m[0] = column 0, m[1] = column 1, ...
 * Flat index: m.d[col * 4 + row]
 * ------------------------------------------------------------------------- */

typedef struct MopMat4 {
    float d[16];
} MopMat4;

/* -------------------------------------------------------------------------
 * Color — linear RGBA, each component in [0, 1]
 * ------------------------------------------------------------------------- */

typedef struct MopColor {
    float r, g, b, a;
} MopColor;

/* -------------------------------------------------------------------------
 * Vertex — position + normal + color
 *
 * This is the fixed vertex format used throughout the engine.
 * All meshes must provide vertices in this layout.
 * ------------------------------------------------------------------------- */

typedef struct MopVertex {
    MopVec3 position;
    MopVec3 normal;
    MopColor color;
    float u, v;     /* texture coordinates */
} MopVertex;

/* -------------------------------------------------------------------------
 * Render mode
 * ------------------------------------------------------------------------- */

typedef enum MopRenderMode {
    MOP_RENDER_SOLID     = 0,
    MOP_RENDER_WIREFRAME = 1
} MopRenderMode;

/* -------------------------------------------------------------------------
 * Blend mode
 * ------------------------------------------------------------------------- */

typedef enum MopBlendMode {
    MOP_BLEND_OPAQUE   = 0,
    MOP_BLEND_ALPHA    = 1,
    MOP_BLEND_ADDITIVE = 2,
    MOP_BLEND_MULTIPLY = 3
} MopBlendMode;

/* -------------------------------------------------------------------------
 * Shading mode
 * ------------------------------------------------------------------------- */

typedef enum MopShadingMode {
    MOP_SHADING_FLAT   = 0,
    MOP_SHADING_SMOOTH = 1
} MopShadingMode;

/* -------------------------------------------------------------------------
 * Math utilities
 *
 * All matrix functions produce column-major matrices.
 * Angles are in radians unless noted otherwise.
 * ------------------------------------------------------------------------- */

MopMat4 mop_mat4_identity(void);
MopMat4 mop_mat4_perspective(float fov_radians, float aspect,
                             float near_plane, float far_plane);
MopMat4 mop_mat4_look_at(MopVec3 eye, MopVec3 center, MopVec3 up);
MopMat4 mop_mat4_rotate_y(float angle_radians);
MopMat4 mop_mat4_rotate_x(float angle_radians);
MopMat4 mop_mat4_rotate_z(float angle_radians);
MopMat4 mop_mat4_translate(MopVec3 offset);
MopMat4 mop_mat4_scale(MopVec3 s);
MopMat4 mop_mat4_multiply(MopMat4 a, MopMat4 b);
MopVec4 mop_mat4_mul_vec4(MopMat4 m, MopVec4 v);

/* Compose a TRS matrix: T * Rz * Ry * Rx * S.
 * rotation components are euler angles in radians. */
MopMat4 mop_mat4_compose_trs(MopVec3 position, MopVec3 rotation, MopVec3 scale);
MopMat4 mop_mat4_inverse(MopMat4 m);

MopVec3 mop_vec3_add(MopVec3 a, MopVec3 b);
MopVec3 mop_vec3_sub(MopVec3 a, MopVec3 b);
MopVec3 mop_vec3_scale(MopVec3 v, float s);
MopVec3 mop_vec3_cross(MopVec3 a, MopVec3 b);
float   mop_vec3_dot(MopVec3 a, MopVec3 b);
float   mop_vec3_length(MopVec3 v);
MopVec3 mop_vec3_normalize(MopVec3 v);

#endif /* MOP_TYPES_H */
