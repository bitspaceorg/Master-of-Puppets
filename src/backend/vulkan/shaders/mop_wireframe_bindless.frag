#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_wireframe_bindless.frag — Bindless wireframe fragment shader (Phase 2A)
 *
 * Flat-color wireframe — no lighting. Fetches object_id from the
 * object SSBO via draw_id. Must match the bindless descriptor layout.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_texcoord;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) flat in uint v_draw_id;

struct ObjectData {
    mat4  model;
    float ambient;
    float opacity;
    uint  object_id;
    int   blend_mode;
    float metallic;
    float roughness;
    int   base_tex_idx;
    int   normal_tex_idx;
    vec4  emissive;
    int   mr_tex_idx;
    int   ao_tex_idx;
    int   _pad0;
    int   _pad1;
    vec4  bound_sphere;
};

layout(std430, set = 0, binding = 0) readonly buffer ObjectSSBO {
    ObjectData objects[];
} obj_buf;

/* These must be declared to match the bindless descriptor layout,
 * even though wireframe doesn't use them. */
struct Light {
    vec4 position;
    vec4 direction;
    vec4 color;
    vec4 params;
};

layout(std140, set = 0, binding = 1) uniform FrameGlobals {
    vec4  light_dir;
    vec4  cam_pos;
    int   shadows_enabled;
    int   cascade_count;
    int   num_lights;
    float exposure;
    mat4  cascade_vp[4];
    vec4  cascade_splits;
} frame;

layout(std430, set = 0, binding = 2) readonly buffer LightSSBO {
    Light lights[];
} light_buf;

layout(set = 0, binding = 3) uniform sampler2DArrayShadow u_shadow_map;
layout(set = 0, binding = 4) uniform sampler2D u_irradiance_map;
layout(set = 0, binding = 5) uniform sampler2D u_prefiltered_map;
layout(set = 0, binding = 6) uniform sampler2D u_brdf_lut;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out uint frag_object_id;

void main() {
    frag_color = v_color;
    frag_object_id = obj_buf.objects[v_draw_id].object_id;
}
