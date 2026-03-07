#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_bloom_blur.frag — Dual-direction Gaussian blur for bloom
 *
 * Used for both downsample (13-tap) and upsample (tent filter) passes.
 * Push constants control direction and texel size.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_input;

layout(push_constant) uniform PushConstants {
    vec2 texel_size;
    vec2 direction;  /* (1,0) for horizontal, (0,1) for vertical */
} push;

layout(location = 0) out vec4 frag_color;

void main() {
    /* 9-tap Gaussian kernel (sigma ~= 2.5):
     * weights: 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 */
    const float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    vec2 step = push.texel_size * push.direction;
    vec3 result = texture(u_input, v_uv).rgb * w[0];

    for (int i = 1; i < 5; i++) {
        vec2 offset = step * float(i);
        result += texture(u_input, v_uv + offset).rgb * w[i];
        result += texture(u_input, v_uv - offset).rgb * w[i];
    }

    frag_color = vec4(result, 1.0);
}
