#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_ssao_blur.frag — Bilateral blur for SSAO (4x4 box, edge-aware)
 *
 * Blurs the raw R8_UNORM SSAO texture while preserving edges.
 *
 * Binding 0: Raw SSAO (R8_UNORM)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_ssao;

layout(push_constant) uniform PC {
    vec2 texel_size;
} pc;

layout(location = 0) out float frag_ao;

void main() {
    float result = 0.0;
    for (int x = -2; x < 2; x++) {
        for (int y = -2; y < 2; y++) {
            vec2 offset = vec2(float(x), float(y)) * pc.texel_size;
            result += texture(u_ssao, v_uv + offset).r;
        }
    }
    frag_ao = result / 16.0;
}
