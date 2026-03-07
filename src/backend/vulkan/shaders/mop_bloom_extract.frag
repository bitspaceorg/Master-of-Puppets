#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_bloom_extract.frag — Extract bright pixels from HDR image for bloom
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

void main() {
    vec3 color = texture(u_hdr, v_uv).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));

    /* Soft threshold with knee */
    float soft = brightness - push.threshold + push.soft_knee;
    soft = clamp(soft, 0.0, 2.0 * push.soft_knee);
    soft = soft * soft / (4.0 * push.soft_knee + 1e-4);
    float contribution = max(soft, brightness - push.threshold);
    contribution /= max(brightness, 1e-4);

    frag_color = vec4(color * contribution, 1.0);
}
