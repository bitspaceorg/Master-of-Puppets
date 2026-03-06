#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_grid.frag — Analytical infinite grid (Y=0 plane)
 *
 * Fullscreen fragment shader that computes grid lines per-pixel using
 * screen-space derivatives for scale-aware anti-aliasing.  Matches the
 * CPU implementation (Blender's GPU Gems 2 disc-radius AA technique).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform sampler2D u_depth;

layout(push_constant) uniform PC {
    /* Inverse homography: maps NDC (skip Y) to world XZ on Y=0 plane */
    float Hi[9];
    /* VP matrix rows for depth reconstruction */
    float vp_z0, vp_z2, vp_z3;
    float vp_w0, vp_w2, vp_w3;
    /* Grid parameters */
    float grid_half;
    uint reverse_z;
    float axis_half_width; /* half-width in pixels for axis lines */
    float _pad;
    /* Grid colors (linear RGB) */
    vec4 minor_color;
    vec4 major_color;
    vec4 axis_x_color;
    vec4 axis_z_color;
} pc;

/* Blender's GPU Gems 2 disc-radius AA */
#define DISC_RADIUS 0.5923 /* M_1_SQRTPI * 1.05 */
#define SMOOTH_START (0.5 + DISC_RADIUS)
#define SMOOTH_END   (0.5 - DISC_RADIUS)

float grid_line_step(float dist) {
    if (dist <= SMOOTH_END) return 1.0;
    if (dist >= SMOOTH_START) return 0.0;
    float t = (dist - SMOOTH_END) / (SMOOTH_START - SMOOTH_END);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

float edge_fade(float coord, float half_extent) {
    float edge = abs(coord);
    float fade_start = half_extent * 0.8;
    if (edge <= fade_start) return 1.0;
    if (edge >= half_extent) return 0.0;
    float t = (edge - fade_start) / (half_extent - fade_start);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

void main() {
    vec2 pixel = gl_FragCoord.xy;
    vec2 res = vec2(textureSize(u_depth, 0));

    /* NDC from pixel coordinates */
    float nx = pixel.x * 2.0 / res.x - 1.0;
    float ny = 1.0 - pixel.y * 2.0 / res.y;

    /* Homography: NDC → world XZ on Y=0 */
    float s = pc.Hi[6] * nx + pc.Hi[7] * ny + pc.Hi[8];
    if (s < 1e-8) discard;

    float inv_s = 1.0 / s;
    float wx = (pc.Hi[0] * nx + pc.Hi[1] * ny + pc.Hi[2]) * inv_s;
    float wz = (pc.Hi[3] * nx + pc.Hi[4] * ny + pc.Hi[5]) * inv_s;

    if (abs(wx) > pc.grid_half || abs(wz) > pc.grid_half) discard;

    /* Screen-space derivatives (fwidth) */
    float inv_w2 = 2.0 / res.x;
    float inv_h2 = 2.0 / res.y;
    float dwx_dpx = (pc.Hi[0] - wx * pc.Hi[6]) * inv_s * inv_w2;
    float dwx_dpy = (pc.Hi[1] - wx * pc.Hi[7]) * inv_s * inv_h2;
    float dwz_dpx = (pc.Hi[3] - wz * pc.Hi[6]) * inv_s * inv_w2;
    float dwz_dpy = (pc.Hi[4] - wz * pc.Hi[7]) * inv_s * inv_h2;
    float fw_x = max(abs(dwx_dpx) + abs(dwx_dpy), 1e-7);
    float fw_z = max(abs(dwz_dpx) + abs(dwz_dpy), 1e-7);

    /* Subgrid: every 1/3 unit */
    float sub3x = wx * 3.0, sub3z = wz * 3.0;
    float sub_px_x = abs(sub3x - round(sub3x)) / (3.0 * fw_x);
    float sub_px_z = abs(sub3z - round(sub3z)) / (3.0 * fw_z);
    float a_sub = grid_line_step(min(sub_px_x, sub_px_z));

    /* Major grid: every 1 unit */
    float maj_px_x = abs(wx - round(wx)) / fw_x;
    float maj_px_z = abs(wz - round(wz)) / fw_z;
    float a_maj = grid_line_step(min(maj_px_x, maj_px_z));

    /* Axes: X-axis (wz=0), Z-axis (wx=0) — use theme half-width */
    float ax_px_x = abs(wz) / fw_z;
    float ax_px_z = abs(wx) / fw_x;
    float a_ax_x = grid_line_step(ax_px_x - pc.axis_half_width);
    float a_ax_z = grid_line_step(ax_px_z - pc.axis_half_width);
    float a_ax = max(a_ax_x, a_ax_z);

    if (a_sub < 0.01 && a_maj < 0.01 && a_ax < 0.01) discard;

    /* Depth test: discard grid behind scene geometry */
    {
        vec2 depth_uv = gl_FragCoord.xy / res;
        float scene_d = texture(u_depth, depth_uv).r;

        /* Reverse-Z: clear=0, closer=larger. Has geometry if depth > 0 */
        bool has_geometry = (pc.reverse_z != 0u)
            ? (scene_d > 0.0001)
            : (scene_d < 0.9999);
        if (has_geometry) discard;
    }

    /* Pick dominant level: axis > major > subgrid */
    vec3 color;
    float alpha;
    if (a_ax > 0.01) {
        alpha = a_ax;
        color = (a_ax_x >= a_ax_z) ? pc.axis_x_color.rgb : pc.axis_z_color.rgb;
    } else if (a_maj > 0.01) {
        alpha = a_maj * 0.7;
        color = pc.major_color.rgb;
    } else {
        alpha = a_sub * 0.45;
        color = pc.minor_color.rgb;
    }

    if (alpha < 0.005) discard;

    /* Input colors are sRGB. Framebuffer is R8G8B8A8_SRGB (hardware applies
     * linear→sRGB on write), so convert sRGB→linear to preserve original. */
    frag_color = vec4(pow(color, vec3(2.2)), alpha);
}
