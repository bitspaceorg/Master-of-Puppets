#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_overlay.frag — SDF overlay fragment shader
 *
 * Renders 2D UI overlays (lines, circles, diamonds) using signed distance
 * fields for crisp analytical anti-aliasing.  All primitives are passed as
 * an SSBO and iterated per-pixel in a single fullscreen draw call.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

/* Depth buffer for depth-tested overlays */
layout(set = 0, binding = 0) uniform sampler2D u_depth;

/* Primitive data packed as vec4 array (3 vec4s per prim = 12 floats) */
layout(std430, set = 0, binding = 1) readonly buffer Prims {
    vec4 data[];
} prims;

layout(push_constant) uniform PC {
    uint prim_count;
    float fb_width;
    float fb_height;
    uint reverse_z;
} pc;

/* -------------------------------------------------------------------------
 * SDF functions
 * ------------------------------------------------------------------------- */

/* Capsule SDF: distance from point p to line segment a→b */
float sdf_line(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    return length(p - (a + t * ab));
}

/* Circle SDF: distance from point p to circle boundary */
float sdf_circle(vec2 p, vec2 c, float r) {
    return length(p - c) - r;
}

/* Diamond (rotated square) SDF */
float sdf_diamond(vec2 p, vec2 c, float s) {
    vec2 d = abs(p - c);
    return (d.x + d.y) / 1.41421356 - s / 1.41421356;
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

void main() {
    vec2 pixel = gl_FragCoord.xy;
    vec4 result = vec4(0.0);

    float scene_depth = texture(u_depth, v_uv).r;

    for (uint i = 0; i < pc.prim_count; i++) {
        /* Unpack: 3 vec4s per prim
         * [0] = x0, y0, x1, y1
         * [1] = r, g, b, a (opacity)
         * [2] = width, radius, type (as float), depth */
        vec4 d0 = prims.data[i * 3 + 0];
        vec4 d1 = prims.data[i * 3 + 1];
        vec4 d2 = prims.data[i * 3 + 2];

        vec2 p0 = d0.xy;
        vec2 p1 = d0.zw;
        vec3 prim_color = pow(d1.rgb, vec3(2.2)); /* sRGB → linear */
        float prim_opacity = d1.a;
        float prim_width = d2.x;
        float prim_radius = d2.y;
        int prim_type = int(d2.z);
        float prim_depth = d2.w;

        /* Depth test: skip if overlay is behind scene geometry.
         * Reverse-Z depth = near/distance, so values are tiny for far objects
         * (0.005–0.02 at typical distances).  A fixed bias would be too large;
         * use a proportional bias instead. */
        if (prim_depth >= 0.0) {
            bool occluded;
            if (pc.reverse_z != 0u) {
                float bias = max(0.00005, prim_depth * 0.02);
                occluded = (scene_depth > 0.0001) &&
                           (prim_depth + bias < scene_depth);
            } else {
                float bias = max(0.0001, prim_depth * 0.02);
                occluded = (scene_depth < 0.9999) &&
                           (prim_depth - bias > scene_depth);
            }
            if (occluded) continue;
        }

        float d;
        if (prim_type == 0) {
            /* LINE: capsule SDF with half-width */
            d = sdf_line(pixel, p0, p1) - prim_width * 0.5;
        } else if (prim_type == 1) {
            /* FILLED_CIRCLE: negative inside, positive outside */
            d = sdf_circle(pixel, p0, prim_radius);
        } else {
            /* DIAMOND: rotated square SDF with half-width for stroke */
            d = abs(sdf_diamond(pixel, p0, prim_radius)) - prim_width * 0.5;
        }

        /* Crisp anti-aliased alpha from SDF: tight smoothstep for sharp edges */
        float aa = 1.0 - smoothstep(-0.2, 0.2, d);
        float alpha = aa * prim_opacity;

        if (alpha < 0.004) continue;

        /* Alpha-blend primitive over accumulated result */
        result.rgb = mix(result.rgb, prim_color, alpha);
        result.a = result.a + alpha * (1.0 - result.a);
    }

    /* Result is premultiplied linear — blending uses ONE, ONE_MINUS_SRC_ALPHA.
     * The SRGB framebuffer auto-converts linear→sRGB on write. */
    frag_color = result;
}
