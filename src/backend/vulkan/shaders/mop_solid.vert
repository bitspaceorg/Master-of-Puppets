#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_solid.vert — Vertex shader for solid and wireframe rendering
 *
 * Vertex layout matches MopVertex (stride = 48 bytes):
 *   location 0: vec3 position  (offset  0)
 *   location 1: vec3 normal    (offset 12)
 *   location 2: vec4 color     (offset 24)
 *   location 3: vec2 texcoord  (offset 40)
 *
 * Push constants: mat4 mvp + mat4 model (128 bytes)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec2 a_texcoord;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_texcoord;

void main() {
    gl_Position = pc.mvp * vec4(a_position, 1.0);
    v_normal    = mat3(pc.model) * a_normal;
    v_color     = a_color;
    v_texcoord  = a_texcoord;
}
