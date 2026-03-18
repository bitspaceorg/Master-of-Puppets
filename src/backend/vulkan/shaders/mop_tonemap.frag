#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_tonemap.frag — ACES Filmic tonemapping (HDR -> LDR) with bloom + AO
 *
 * Samples the HDR color attachment and applies ACES Filmic tonemapping
 * with exposure control.  Output is written to an R8G8B8A8_SRGB target
 * which auto-applies linear -> sRGB conversion.
 *
 * Bloom is combined directly in this pass: the two largest downsample
 * levels (half-res + quarter-res) are bilinearly upsampled by the texture
 * unit and blended into the HDR color.  Deeper mip levels are allocated
 * for layout transitions but NOT sampled here — MoltenVK/Apple TBDR
 * corrupts small render targets (< ~128px) at the Vulkan-to-Metal
 * translation layer.
 *
 * Binding 0: HDR color
 * Binding 1-5: Bloom levels 0-4 (only 0-1 sampled; 2-4 for layout compat)
 * Binding 6: SSAO (or 1x1 white if disabled)
 * Binding 7: SSR  (or 1x1 black if disabled) — reflection RGB + confidence A
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(set = 0, binding = 0) uniform sampler2D u_hdr;
layout(set = 0, binding = 1) uniform sampler2D u_bloom0;
layout(set = 0, binding = 2) uniform sampler2D u_bloom1;
layout(set = 0, binding = 3) uniform sampler2D u_bloom2;
layout(set = 0, binding = 4) uniform sampler2D u_bloom3;
layout(set = 0, binding = 5) uniform sampler2D u_bloom4;
layout(set = 0, binding = 6) uniform sampler2D u_ssao;
layout(set = 0, binding = 7) uniform sampler2D u_ssr;

layout(push_constant) uniform PC {
    float exposure;
    float bloom_intensity;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

vec3 aces(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec4 hdr_sample = texture(u_hdr, v_uv);
    vec3 hdr = hdr_sample.rgb;
    float alpha = hdr_sample.a;

    /* Alpha=0 marks background pixels (gradient / clear color).
     * Background brightness must stay constant regardless of exposure —
     * only scene geometry and skybox (alpha=1) get tonemapped. */
    if (alpha < 0.5) {
        frag_color = vec4(hdr, 1.0);
        return;
    }

    float ao = texture(u_ssao, v_uv).r;
    /* Guard against broken AO binding (white fallback reads 0 on some drivers) */
    if (ao < 0.01) ao = 1.0;

    /* Two-level bloom: half-res (level 0) + quarter-res (level 1).
     * Bilinear upsampling by the texture unit gives natural soft bloom. */
    vec3 bloom = texture(u_bloom0, v_uv).rgb * 0.6
               + texture(u_bloom1, v_uv).rgb * 0.4;

    /* SSR: add reflections weighted by confidence (alpha channel) */
    vec4 ssr = texture(u_ssr, v_uv);
    hdr += ssr.rgb * ssr.a;

    /* Combine: HDR + bloom, modulated by AO */
    vec3 combined = (hdr + bloom * pc.bloom_intensity) * ao;

    /* Exposure then ACES filmic tonemap */
    combined *= pc.exposure;
    vec3 ldr = aces(combined);

    frag_color = vec4(ldr, 1.0);
}
