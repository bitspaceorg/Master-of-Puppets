#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_tonemap.frag — ACES Filmic tonemapping (HDR -> LDR)
 *
 * Samples the HDR color attachment and applies ACES Filmic tonemapping
 * with exposure control.  Output is written to an R8G8B8A8_SRGB target
 * which auto-applies linear -> sRGB conversion.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(set = 0, binding = 0) uniform sampler2D u_hdr;
layout(push_constant) uniform PC { float exposure; } pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

vec3 aces(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(u_hdr, v_uv).rgb * pc.exposure;
    frag_color = vec4(aces(hdr), 1.0);
}
