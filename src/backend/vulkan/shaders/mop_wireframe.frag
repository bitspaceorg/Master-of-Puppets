#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_wireframe.frag — Flat-color wireframe fragment shader
 *
 * No lighting — just passes vertex color through.
 * Uses the same UBO layout as the solid shader for object_id.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_texcoord;
layout(location = 3) in vec3 v_world_pos;

struct Light {
    vec4 position;
    vec4 direction;
    vec4 color;
    vec4 params;
};

layout(set = 0, binding = 0) uniform FragUniforms {
    vec4  light_dir;
    float ambient;
    float opacity;
    uint  object_id;
    int   blend_mode;
    int   has_texture;
    int   num_lights;
    float metallic;
    float roughness;
    vec4  cam_pos;
    Light lights[8];

    /* Shadow mapping (cascade) — must match solid shader UBO layout */
    int   shadows_enabled;
    int   cascade_count;
    float _pad_shadow[2];
    mat4  cascade_vp[4];
    vec4  cascade_splits;
} frag;

layout(set = 0, binding = 1) uniform sampler2D u_texture;
layout(set = 0, binding = 2) uniform sampler2DArrayShadow u_shadow_map;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out uint frag_object_id;

void main() {
    frag_color = v_color;
    frag_object_id = frag.object_id;
}
