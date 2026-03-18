/*
 * Master of Puppets — Vulkan Backend
 * mop_taa.frag — Temporal Anti-Aliasing resolve shader
 *
 * Reads the current jittered LDR frame and the previous accumulated history.
 * Reprojects via depth to compute per-pixel motion, then blends with
 * neighborhood clamping to reduce ghosting.
 *
 * Bindings:
 *   0 — current frame (LDR, jittered)
 *   1 — history (previous TAA output)
 *   2 — depth buffer (R32_SFLOAT copy)
 *
 * Push constants:
 *   mat4 inv_vp_jittered   — inverse of current (jittered) VP matrix
 *   mat4 prev_vp           — previous frame's (unjittered) VP matrix
 *   vec2 inv_resolution    — 1/width, 1/height
 *   vec2 jitter            — current frame jitter in pixels
 *   float feedback         — blend weight for history (e.g., 0.9)
 *   float first_frame      — 1.0 on first frame (no history), 0.0 otherwise
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_current;
layout(set = 0, binding = 1) uniform sampler2D u_history;
layout(set = 0, binding = 2) uniform sampler2D u_depth;

layout(push_constant) uniform TaaPush {
    mat4 inv_vp_jittered;  /* offset 0 */
    mat4 prev_vp;          /* offset 64 */
    vec2 inv_resolution;   /* offset 128 */
    vec2 jitter;           /* offset 136 */
    float feedback;        /* offset 144 */
    float first_frame;     /* offset 148 */
} taa;

/* ----- Catmull-Rom 5-tap filter on history texture ----- */

vec4 sample_history_catmull_rom(vec2 uv) {
    vec2 texel_size = taa.inv_resolution;
    vec2 tex_pos = uv / texel_size - 0.5;
    vec2 f = fract(tex_pos);
    vec2 tex_base = (floor(tex_pos) + 0.5) * texel_size;

    /* Simplified Catmull-Rom weights for B-spline approximation */
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    /* Bilinear groups: combine w1+w2 into a single tap */
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / max(w12, 1e-6);

    vec2 tc0 = tex_base - texel_size;
    vec2 tc12 = tex_base + offset12 * texel_size;
    vec2 tc3 = tex_base + 2.0 * texel_size;

    /* 5-tap pattern (cross shape) instead of full 4x4 */
    vec4 result = vec4(0.0);
    float total_weight = 0.0;

    float wc = w12.x * w12.y;
    result += texture(u_history, tc12) * wc;
    total_weight += wc;

    float wl = w0.x * w12.y;
    result += texture(u_history, vec2(tc0.x, tc12.y)) * wl;
    total_weight += wl;

    float wr = w3.x * w12.y;
    result += texture(u_history, vec2(tc3.x, tc12.y)) * wr;
    total_weight += wr;

    float wt = w12.x * w0.y;
    result += texture(u_history, vec2(tc12.x, tc0.y)) * wt;
    total_weight += wt;

    float wb = w12.x * w3.y;
    result += texture(u_history, vec2(tc12.x, tc3.y)) * wb;
    total_weight += wb;

    return result / max(total_weight, 1e-6);
}

/* ----- Neighbourhood clamping (3x3 min/max in AABB) ----- */

void neighborhood_minmax(out vec3 cmin, out vec3 cmax) {
    vec2 ts = taa.inv_resolution;
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);

    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec3 s = texture(u_current, v_uv + vec2(x, y) * ts).rgb;
            m1 += s;
            m2 += s * s;
        }
    }

    /* Variance clipping: mean ± gamma * stddev */
    m1 /= 9.0;
    m2 /= 9.0;
    vec3 sigma = sqrt(max(m2 - m1 * m1, vec3(0.0)));
    float gamma = 1.25; /* wider = less ghosting rejection, less flicker */
    cmin = m1 - gamma * sigma;
    cmax = m1 + gamma * sigma;
}

/* ----- Depth reprojection for motion vector ----- */

vec2 compute_motion_vector() {
    float depth = texture(u_depth, v_uv).r;

    /* Reconstruct clip-space position */
    vec2 ndc = v_uv * 2.0 - 1.0;
    ndc.y = -ndc.y; /* Vulkan Y-flip */
    vec4 clip = vec4(ndc, depth, 1.0);

    /* World position via inverse jittered VP */
    vec4 world = taa.inv_vp_jittered * clip;
    world /= world.w;

    /* Reproject to previous frame's clip space */
    vec4 prev_clip = taa.prev_vp * world;
    vec2 prev_ndc = prev_clip.xy / prev_clip.w;
    vec2 prev_uv = prev_ndc * 0.5 + 0.5;
    prev_uv.y = 1.0 - prev_uv.y; /* Vulkan Y-flip */

    return prev_uv - v_uv;
}

void main() {
    vec4 current = texture(u_current, v_uv);

    /* First frame — no history available, passthrough */
    if (taa.first_frame > 0.5) {
        out_color = current;
        return;
    }

    /* Compute motion via depth reprojection */
    vec2 motion = compute_motion_vector();
    vec2 history_uv = v_uv + motion;

    /* Reject history samples outside the screen */
    if (history_uv.x < 0.0 || history_uv.x > 1.0 ||
        history_uv.y < 0.0 || history_uv.y > 1.0) {
        out_color = current;
        return;
    }

    /* Sample history with Catmull-Rom filter for sub-pixel quality */
    vec4 history = sample_history_catmull_rom(history_uv);

    /* Neighbourhood clamping — clip history to current frame's local colors */
    vec3 cmin, cmax;
    neighborhood_minmax(cmin, cmax);
    history.rgb = clamp(history.rgb, cmin, cmax);

    /* Adaptive feedback: reduce history contribution for fast-moving pixels */
    float motion_len = length(motion / taa.inv_resolution); /* in pixels */
    float adaptive_feedback = mix(taa.feedback, 0.5, clamp(motion_len / 20.0, 0.0, 1.0));

    /* Blend: accumulate history with current frame */
    out_color = vec4(mix(current.rgb, history.rgb, adaptive_feedback), current.a);
}
