#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_skybox.frag — Equirectangular HDR environment skybox
 *
 * Renders an equirectangular environment map as the scene background.
 * Uses the inverse view-projection matrix to reconstruct world-space
 * ray directions from screen-space UVs.
 *
 * Reuses mop_fullscreen.vert for the fullscreen triangle.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(set = 0, binding = 0) uniform SkyboxUBO {
    mat4 inv_view_proj;
    vec4 cam_pos;       /* xyz = camera eye position */
    float rotation;     /* Y-axis rotation in radians */
    float intensity;    /* brightness multiplier */
} ubo;

layout(set = 0, binding = 1) uniform sampler2D u_env_map;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;
layout(location = 1) out uint frag_object_id;

const float PI = 3.14159265359;

void main() {
    /* Reconstruct world-space ray direction from NDC */
    vec2 ndc = v_uv * 2.0 - 1.0;

    /* Unproject far plane point */
    vec4 world_far = ubo.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 world_pos = world_far.xyz / world_far.w;
    vec3 dir = normalize(world_pos - ubo.cam_pos.xyz);

    /* Equirectangular mapping */
    float phi = atan(dir.z, dir.x) + ubo.rotation;
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    vec2 env_uv = vec2(phi / (2.0 * PI) + 0.5, theta / PI + 0.5);

    vec3 color = texture(u_env_map, env_uv).rgb * ubo.intensity;

    frag_color = vec4(color, 1.0);
    frag_object_id = 0u; /* background */
}
