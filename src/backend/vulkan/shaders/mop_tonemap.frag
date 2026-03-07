#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_tonemap.frag — ACES Filmic tonemapping (HDR -> LDR) with bloom + AO
 *
 * Samples the HDR color attachment and applies ACES Filmic tonemapping
 * with exposure control.  Output is written to an R8G8B8A8_SRGB target
 * which auto-applies linear -> sRGB conversion.
 *
 * Binding 0: HDR color
 * Binding 1: Bloom (or 1x1 black if disabled)
 * Binding 2: SSAO (or 1x1 white if disabled)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(set = 0, binding = 0) uniform sampler2D u_hdr;
layout(set = 0, binding = 1) uniform sampler2D u_bloom;
layout(set = 0, binding = 2) uniform sampler2D u_ssao;

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
    vec4 hdr_pixel = texture(u_hdr, v_uv);

    /* Alpha=0 marks background (clear color) pixels.  These should not
     * receive exposure, bloom, AO, or ACES tonemapping — the background
     * brightness must stay constant regardless of the exposure slider.
     * Skybox pixels render with alpha>0 and get full treatment. */
    if (hdr_pixel.a > 0.0) {
        vec3 hdr = hdr_pixel.rgb;

        /* Apply SSAO */
        float ao = texture(u_ssao, v_uv).r;
        hdr *= ao;

        /* Add bloom */
        vec3 bloom = texture(u_bloom, v_uv).rgb;
        hdr += bloom * pc.bloom_intensity;

        /* Apply exposure and tonemap */
        hdr *= pc.exposure;
        frag_color = vec4(aces(hdr), 1.0);
    } else {
        /* Background pixel — pass through unchanged.
         * The sRGB output format handles linear→sRGB conversion. */
        frag_color = vec4(hdr_pixel.rgb, 1.0);
    }
}
