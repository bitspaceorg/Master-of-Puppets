/*
 * Master of Puppets — Vulkan Backend
 * mop_decal.frag — Deferred decal projection fragment shader
 *
 * Reconstructs world position from the depth buffer and the inv_vp UBO,
 * projects it into the decal's local UV space via push-constant inv_decal,
 * and applies the decal texture with alpha blending and edge fade.
 *
 * Descriptors:
 *   set 0, binding 0 — depth buffer (R32_SFLOAT copy)
 *   set 0, binding 1 — decal texture (RGBA)
 *   set 0, binding 2 — UBO: { mat4 inv_vp; int reverse_z; pad[3]; } = 80 bytes
 *
 * Push constants (128 bytes, shared with vertex):
 *   mat4 mvp       — VP * decal_model (used by vertex shader)
 *   mat4 inv_decal — inverse(decal_model) (maps world → decal local space)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#version 450

layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform sampler2D u_depth;
layout(set = 0, binding = 1) uniform sampler2D u_decal_tex;

layout(set = 0, binding = 2) uniform DecalUBO {
    mat4 inv_vp;
    int reverse_z;
    float opacity;
    float _pad[2];
} ubo;

layout(push_constant) uniform PC {
    mat4 mvp;       /* offset 0 */
    mat4 inv_decal; /* offset 64 */
} pc;

void main() {
    /* Screen UV from fragment position */
    vec2 screen_uv = gl_FragCoord.xy / vec2(textureSize(u_depth, 0));

    /* Sample depth */
    float depth = texture(u_depth, screen_uv).r;

    /* Reconstruct NDC */
    float z_ndc = ubo.reverse_z != 0 ? (1.0 - depth) * 2.0 - 1.0
                                       : depth * 2.0 - 1.0;
    vec4 ndc = vec4(screen_uv * 2.0 - 1.0, z_ndc, 1.0);

    /* World position from inv_vp */
    vec4 world = ubo.inv_vp * ndc;
    world.xyz /= world.w;

    /* Project into decal local space */
    vec4 local = pc.inv_decal * vec4(world.xyz, 1.0);

    /* Discard if outside unit cube [-0.5, 0.5]³ */
    if (abs(local.x) > 0.5 || abs(local.y) > 0.5 || abs(local.z) > 0.5) {
        discard;
    }

    /* UV from local XY */
    vec2 decal_uv = local.xy + 0.5;

    /* Sample decal texture */
    vec4 decal = texture(u_decal_tex, decal_uv);

    /* Edge fade: soft falloff near cube boundaries */
    float fade_x = 1.0 - smoothstep(0.4, 0.5, abs(local.x));
    float fade_y = 1.0 - smoothstep(0.4, 0.5, abs(local.y));
    float fade_z = 1.0 - smoothstep(0.4, 0.5, abs(local.z));
    float fade = fade_x * fade_y * fade_z;

    frag_color = vec4(decal.rgb, decal.a * ubo.opacity * fade);
}
