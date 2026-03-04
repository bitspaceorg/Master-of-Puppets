#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_shadow.vert — Depth-only vertex shader for cascade shadow mapping
 *
 * Push constant: mat4 light_vp (64 bytes)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(push_constant) uniform PC { mat4 light_vp; } pc;

layout(location = 0) in vec3 a_position;
/* Declared to match vertex binding but unused */
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_color;

void main() {
    gl_Position = pc.light_vp * vec4(a_position, 1.0);
}
