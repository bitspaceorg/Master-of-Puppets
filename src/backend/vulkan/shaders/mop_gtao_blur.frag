/*
 * Master of Puppets — Vulkan Backend
 * mop_gtao_blur.frag — Depth-aware bilateral blur for GTAO
 *
 * Gaussian-weighted blur that rejects samples across depth discontinuities
 * (object silhouette edges).  This prevents GTAO edge halos from bleeding
 * dark AO values onto neighboring surfaces or the background.
 *
 * Bindings:
 *   0 — raw GTAO/SSAO output (R8_UNORM)
 *   1 — depth buffer (for edge-aware rejection)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out float frag_ao;

layout(set = 0, binding = 0) uniform sampler2D u_ssao;
layout(set = 0, binding = 1) uniform sampler2D u_depth;

layout(push_constant) uniform PC {
    vec2 texel_size;
} pc;

void main() {
    float center_ao = texture(u_ssao, v_uv).r;
    float center_depth = texture(u_depth, v_uv).r;
    float result = center_ao;
    float total_weight = 1.0;

    float depth_threshold = 0.005;

    for (int x = -2; x <= 2; x++) {
        for (int y = -2; y <= 2; y++) {
            if (x == 0 && y == 0) continue;

            vec2 offset = vec2(float(x), float(y)) * pc.texel_size;
            vec2 sample_uv = v_uv + offset;
            float sample_ao = texture(u_ssao, sample_uv).r;
            float sample_depth = texture(u_depth, sample_uv).r;

            float dist2 = float(x * x + y * y);
            float spatial_w = exp(-0.5 * dist2 / 4.0);

            float depth_diff = abs(center_depth - sample_depth);
            float depth_w = step(depth_diff, depth_threshold);

            float w = spatial_w * depth_w;
            result += sample_ao * w;
            total_weight += w;
        }
    }

    frag_ao = result / total_weight;
}
