#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_bloom_blur.frag — Cross-filter Gaussian blur for bloom
 *
 * Samples in both horizontal and vertical directions simultaneously
 * (cross filter) for proper 2D blur in a single pass. Push constants
 * provide texel size and an output weight (1.0 for downsample, <1 for
 * upsample to control accumulation).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_input;

layout(push_constant) uniform PushConstants {
    vec2 texel_size;
    vec2 direction;  /* .x = output weight (1.0 = full, 0.5 = half) */
} push;

layout(location = 0) out vec4 frag_color;

/* Sanitize half-float sample: NaN/inf/negative → 0, clamp to half-float max */
vec3 safe_sample(vec2 uv) {
    vec3 c = texture(u_input, uv).rgb;
    c.r = (c.r > 0.0) ? min(c.r, 65504.0) : 0.0;
    c.g = (c.g > 0.0) ? min(c.g, 65504.0) : 0.0;
    c.b = (c.b > 0.0) ? min(c.b, 65504.0) : 0.0;
    return c;
}

void main() {
    /* 9-tap Gaussian kernel (sigma ~= 2.5):
     * weights: 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216
     * Cross filter: sample ±N in both H and V, each direction gets half
     * the off-center weight so total still sums to ~1.0 */
    const float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    vec2 step_h = vec2(push.texel_size.x, 0.0);
    vec2 step_v = vec2(0.0, push.texel_size.y);

    vec3 result = safe_sample(v_uv) * w[0];

    for (int i = 1; i < 5; i++) {
        float hw = w[i] * 0.5;
        result += safe_sample(v_uv + step_h * float(i)) * hw;
        result += safe_sample(v_uv - step_h * float(i)) * hw;
        result += safe_sample(v_uv + step_v * float(i)) * hw;
        result += safe_sample(v_uv - step_v * float(i)) * hw;
    }

    /* direction.x = output weight. For upsample passes with alpha blending,
     * the alpha controls lerp factor: lerp(existing, result, weight). */
    float weight = push.direction.x;
    frag_color = vec4(result, weight);
}
