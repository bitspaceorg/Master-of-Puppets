#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_bloom_extract.frag — Extract bright pixels from HDR image for bloom
 *
 * Includes neighbor-based outlier rejection to suppress stale tile data
 * or MSAA resolve artifacts that produce wildly incorrect single-pixel
 * values in the HDR buffer (common on tile-based GPUs via MoltenVK).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_hdr;

layout(push_constant) uniform PushConstants {
    float threshold;
    float soft_knee;
} push;

layout(location = 0) out vec4 frag_color;

vec3 safe_color(vec3 c) {
    c.r = (c.r > 0.0) ? min(c.r, 65504.0) : 0.0;
    c.g = (c.g > 0.0) ? min(c.g, 65504.0) : 0.0;
    c.b = (c.b > 0.0) ? min(c.b, 65504.0) : 0.0;
    return c;
}

void main() {
    vec2 texel = 1.0 / vec2(textureSize(u_hdr, 0));

    /* Sample center + 4 cardinal neighbors */
    vec3 center = safe_color(texture(u_hdr, v_uv).rgb);
    vec3 n0 = safe_color(texture(u_hdr, v_uv + vec2(-texel.x, 0.0)).rgb);
    vec3 n1 = safe_color(texture(u_hdr, v_uv + vec2( texel.x, 0.0)).rgb);
    vec3 n2 = safe_color(texture(u_hdr, v_uv + vec2(0.0, -texel.y)).rgb);
    vec3 n3 = safe_color(texture(u_hdr, v_uv + vec2(0.0,  texel.y)).rgb);

    /* Outlier rejection: if center differs from neighbor average by >4x
     * in brightness, replace with the neighbor average. Catches stale tile
     * data and MSAA resolve artifacts (multicolored random pixels). */
    vec3 avg = (n0 + n1 + n2 + n3) * 0.25;
    float b_center = dot(center, vec3(0.2126, 0.7152, 0.0722));
    float b_avg = dot(avg, vec3(0.2126, 0.7152, 0.0722));

    /* Check both brightness AND color difference (catches same-brightness wrong-color) */
    float color_diff = length(center - avg);
    float ref = max(b_avg, 0.01);
    vec3 color = (color_diff > ref * 4.0) ? avg : center;

    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float soft = brightness - push.threshold + push.soft_knee;
    soft = clamp(soft, 0.0, 2.0 * push.soft_knee);
    soft = soft * soft / (4.0 * push.soft_knee + 1e-4);
    float contribution = max(soft, brightness - push.threshold);
    contribution /= max(brightness, 1e-4);

    frag_color = vec4(color * contribution, 1.0);
}
