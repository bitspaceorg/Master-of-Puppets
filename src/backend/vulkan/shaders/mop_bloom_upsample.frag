#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_bloom_upsample.frag — Two-texture bloom upsample (no LOAD_OP_LOAD)
 *
 * Samples the smaller bloom mip (binding 0), applies a cross-filter
 * Gaussian blur, and additively combines with the current level's
 * downsample content (binding 1).  Writes to a separate output image
 * using LOAD_OP_CLEAR — avoids LOAD_OP_LOAD which fails on MoltenVK
 * tile-based GPUs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_smaller;   /* bloom[i+1] or bloom_up[i+1] */
layout(set = 0, binding = 1) uniform sampler2D u_current;   /* bloom[i] downsample content  */

layout(push_constant) uniform PushConstants {
    vec2 texel_size;   /* texel size of u_smaller */
    vec2 direction;    /* .x = upsample weight */
} push;

layout(location = 0) out vec4 frag_color;

/* Sanitize half-float sample: NaN/inf/negative → 0, clamp to half-float max */
vec3 safe_sample(sampler2D tex, vec2 uv) {
    vec3 c = texture(tex, uv).rgb;
    c.r = (c.r > 0.0) ? min(c.r, 65504.0) : 0.0;
    c.g = (c.g > 0.0) ? min(c.g, 65504.0) : 0.0;
    c.b = (c.b > 0.0) ? min(c.b, 65504.0) : 0.0;
    return c;
}

void main() {
    /* DIAGNOSTIC: pass through u_current (bloom[i] downsample content) only.
     * If green artifacts appear, downsample chain data is corrupt. */
    frag_color = vec4(texture(u_current, v_uv).rgb, 1.0);
}
