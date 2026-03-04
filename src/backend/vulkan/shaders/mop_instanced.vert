#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_instanced.vert — Vertex shader for GPU-instanced rendering
 *
 * Per-vertex data (binding 0, rate = vertex):
 *   location 0: vec3 position  (offset  0)
 *   location 1: vec3 normal    (offset 12)
 *   location 2: vec4 color     (offset 24)
 *   location 3: vec2 texcoord  (offset 40)
 *
 * Per-instance data (binding 1, rate = instance):
 *   location 4-7: mat4 model   (4 × vec4, 64 bytes per instance)
 *
 * Push constants: mat4 vp (view × projection), 64 bytes
 *   Second mat4 slot is unused in instanced path (reserved for compat).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec2 a_texcoord;

/* Per-instance model matrix (4 vec4 columns = 4 locations) */
layout(location = 4) in vec4 inst_col0;
layout(location = 5) in vec4 inst_col1;
layout(location = 6) in vec4 inst_col2;
layout(location = 7) in vec4 inst_col3;

layout(push_constant) uniform PushConstants {
    mat4 vp;
    mat4 _unused;
} pc;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_texcoord;
layout(location = 3) out vec3 v_world_pos;

void main() {
    mat4 model = mat4(inst_col0, inst_col1, inst_col2, inst_col3);
    vec4 world = model * vec4(a_position, 1.0);
    gl_Position = pc.vp * world;
    v_normal    = mat3(model) * a_normal;
    v_color     = a_color;
    v_texcoord  = a_texcoord;
    v_world_pos = world.xyz;
}
