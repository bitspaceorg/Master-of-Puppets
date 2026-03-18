/*
 * Master of Puppets — Vulkan Backend
 * mop_oit_composite.frag — Weighted Blended OIT resolve (McGuire/Bavoil 2013)
 *
 * Reads the OIT accumulation and revealage buffers, computes the average
 * transparent color, and outputs it for hardware alpha blending with the
 * existing opaque framebuffer.
 *
 * Pipeline blend: srcColor=SRC_ALPHA, dstColor=ONE_MINUS_SRC_ALPHA
 *   → result = avg_color * (1 - revealage) + opaque * revealage
 *
 * Bindings:
 *   0 — accumulation buffer (R16G16B16A16_SFLOAT)
 *   1 — revealage buffer (R8_UNORM)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform sampler2D u_accum;
layout(set = 0, binding = 1) uniform sampler2D u_revealage;

void main() {
    vec4 accum = texture(u_accum, v_uv);
    float revealage = texture(u_revealage, v_uv).r;

    /* Fully opaque background (no transparent fragments) — skip */
    if (revealage > 0.999) {
        discard;
    }

    /* Average transparent color */
    vec3 avg_color = accum.rgb / max(accum.a, 1e-5);

    /* Output: alpha = (1 - revealage) for hardware blending.
     * Pipeline does: result = avg_color * alpha + opaque * (1-alpha)
     *              = avg_color * (1-revealage) + opaque * revealage  ✓ */
    frag_color = vec4(avg_color, 1.0 - revealage);
}
