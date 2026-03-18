/*
 * Master of Puppets — Vulkan Backend
 * mop_decal.vert — Deferred decal projection vertex shader
 *
 * Transforms a unit cube [-0.5, 0.5]³ by the combined
 * view-projection * decal_model matrix to clip space.
 *
 * Push constants (shared with fragment, 128 bytes total):
 *   mat4 mvp       — VP * decal_model (transforms cube to clip)
 *   mat4 inv_decal — inverse(decal_model) (maps world → decal local)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#version 450

layout(push_constant) uniform PC {
    mat4 mvp;       /* offset 0:  VP * decal_model */
    mat4 inv_decal; /* offset 64: inverse(decal_model) */
} pc;

/* Procedural unit cube (36 vertices for 12 tris) */
vec3 cube_pos[8] = vec3[](
    vec3(-0.5, -0.5, -0.5),
    vec3( 0.5, -0.5, -0.5),
    vec3( 0.5,  0.5, -0.5),
    vec3(-0.5,  0.5, -0.5),
    vec3(-0.5, -0.5,  0.5),
    vec3( 0.5, -0.5,  0.5),
    vec3( 0.5,  0.5,  0.5),
    vec3(-0.5,  0.5,  0.5)
);

int cube_idx[36] = int[](
    0,1,2, 2,3,0,  /* Front  */
    5,4,7, 7,6,5,  /* Back   */
    3,2,6, 6,7,3,  /* Top    */
    4,5,1, 1,0,4,  /* Bottom */
    1,5,6, 6,2,1,  /* Right  */
    4,0,3, 3,7,4   /* Left   */
);

void main() {
    vec3 local_pos = cube_pos[cube_idx[gl_VertexIndex]];
    gl_Position = pc.mvp * vec4(local_pos, 1.0);
}
