#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_solid_bindless.vert — Bindless vertex shader (Phase 2A)
 *
 * Identical to mop_solid.vert but additionally passes draw_id
 * (from gl_InstanceIndex / firstInstance) to the fragment shader.
 * The fragment shader uses draw_id to index into the object SSBO
 * for per-object material data.
 *
 * Push constants: mat4 mvp + mat4 model (128 bytes) — unchanged
 * draw_id is passed via firstInstance parameter of vkCmdDrawIndexed.
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

layout(location = 0) centroid out vec3 v_normal;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_texcoord;
layout(location = 3) centroid out vec3 v_world_pos;
layout(location = 4) flat out uint v_draw_id;

void main() {
    gl_Position = pc.mvp * vec4(a_position, 1.0);
    v_normal    = mat3(pc.model) * a_normal;
    v_color     = a_color;
    v_texcoord  = a_texcoord;
    v_world_pos = (pc.model * vec4(a_position, 1.0)).xyz;
    v_draw_id   = uint(gl_InstanceIndex);
}
