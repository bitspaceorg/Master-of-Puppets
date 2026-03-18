/*
 * Master of Puppets — Vulkan Backend
 * mop_gtao.frag — Ground Truth Ambient Occlusion (GTAO)
 *
 * Horizon-based screen-space AO with cosine-weighted integration.
 * Based on "Practical Realtime Strategies for Accurate Indirect Occlusion"
 * (Jimenez et al. 2016) and XeGTAO (Intel 2021).
 *
 * For each pixel:
 *   1. Reconstruct view-space position + normal from depth
 *   2. For N slice directions (rotated by noise):
 *      - March screen-space rays in +/- direction
 *      - Track maximum horizon angles h1, h2
 *      - Integrate visible arc using cosine-weighted formula
 *   3. Average visibility across all slices
 *
 * Drop-in replacement for mop_ssao.frag — same descriptor layout
 * and push constant size (96 bytes), same R8_UNORM output.
 *
 * Bindings:
 *   0 — depth buffer (R32_SFLOAT copy of D32_SFLOAT)
 *   1 — 4x4 noise texture (RG16_SFLOAT)
 *   2 — kernel UBO (unused by GTAO, kept for layout compatibility)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out float frag_ao;

layout(set = 0, binding = 0) uniform sampler2D u_depth;
layout(set = 0, binding = 1) uniform sampler2D u_noise;

/* Binding 2 kept for descriptor layout compatibility with SSAO */
layout(set = 0, binding = 2) uniform KernelUBO {
    vec4 samples[64];
} kernel;

layout(push_constant) uniform PC {
    mat4 projection;
    float radius;         /* world-space AO radius */
    float intensity;      /* AO strength (1.0 = default) */
    int num_steps;        /* ray march steps per direction (4-12) */
    int reverse_z;
    vec2 noise_scale;     /* viewport_size / 4.0 */
    vec2 inv_resolution;  /* 1/width, 1/height */
} pc;

#define NUM_DIRECTIONS 4
#define PI 3.14159265

/* Reconstruct view-space position from depth + UV */
vec3 view_pos_from_depth(vec2 uv, float depth) {
    float z_ndc = pc.reverse_z != 0 ? (1.0 - depth) * 2.0 - 1.0
                                     : depth * 2.0 - 1.0;
    vec4 clip = vec4(uv * 2.0 - 1.0, z_ndc, 1.0);
    float a = pc.projection[0][0];
    float b = pc.projection[1][1];
    float c = pc.projection[2][2];
    float d = pc.projection[3][2];
    float view_z = d / (clip.z - c);
    return vec3(clip.x * view_z / a, clip.y * view_z / b, view_z);
}

/* GTAO inner integral: integrate cosine-weighted visibility over a hemisphere
 * slice between horizon angles h1, h2 given the projected normal angle n.
 *
 * V = 0.25 * (-cos(2h - n) + cos(n) + 2h*sin(n))  evaluated at h1 and h2 */
float integrate_arc(float h1, float h2, float n) {
    float sn = sin(n);
    float cn = cos(n);
    return 0.25 * (-cos(2.0 * h1 - n) + cn + 2.0 * h1 * sn)
         + 0.25 * (-cos(2.0 * h2 - n) + cn + 2.0 * h2 * sn);
}

void main() {
    float depth = texture(u_depth, v_uv).r;

    /* Skip far plane */
    if (pc.reverse_z != 0) {
        if (depth < 0.0001) { frag_ao = 1.0; return; }
    } else {
        if (depth > 0.9999) { frag_ao = 1.0; return; }
    }

    vec3 P = view_pos_from_depth(v_uv, depth);
    float view_z = abs(P.z);

    /* Reconstruct view-space normal from depth cross-derivatives */
    vec3 dpdx = dFdx(P);
    vec3 dpdy = dFdy(P);

    /* Silhouette edge detection: if the depth derivative is large relative
     * to view distance, this pixel straddles a depth discontinuity (e.g.
     * sphere edge against background).  The reconstructed normal is unreliable
     * here and AO would produce a dark halo artifact — skip it. */
    float depth_deriv = max(abs(dFdx(depth)), abs(dFdy(depth)));
    if (depth_deriv > 0.002 * view_z) {
        frag_ao = 1.0;
        return;
    }

    vec3 N = normalize(cross(dpdx, dpdy));

    /* Project AO radius from world-space to screen-space pixels.
     * Screen radius ≈ radius * projection[0][0] / |view_z| * 0.5 * viewport_width
     * But we work in UV space, so: radius_uv = radius * proj[0][0] / |view_z| * 0.5 */
    float radius_ss = pc.radius * pc.projection[0][0] / view_z * 0.5;

    /* Clamp minimum screen radius to avoid precision issues */
    if (radius_ss < 2.0 * max(pc.inv_resolution.x, pc.inv_resolution.y)) {
        frag_ao = 1.0;
        return;
    }
    /* Clamp maximum to avoid sampling too far */
    radius_ss = min(radius_ss, 0.1);

    /* Per-pixel random rotation from noise texture */
    vec2 noise = texture(u_noise, v_uv * pc.noise_scale).rg;
    float noise_angle = noise.x * PI; /* rotation offset */
    float noise_step = noise.y;       /* step jitter [0,1) */

    float visibility = 0.0;
    int steps = clamp(pc.num_steps, 2, 12);

    for (int dir = 0; dir < NUM_DIRECTIONS; dir++) {
        /* Slice direction (evenly spaced + noise rotation) */
        float angle = (float(dir) + noise_angle) * (PI / float(NUM_DIRECTIONS));
        vec2 slice_dir = vec2(cos(angle), sin(angle));

        /* Scale direction to account for aspect ratio in UV space */
        vec2 step_uv = slice_dir * radius_ss / float(steps);

        /* Project normal onto the slice plane.
         * slice_dir is in UV space → corresponds to view-space (x,y) direction.
         * We need the view-space direction for the angle computation. */
        vec3 slice_view_dir = normalize(vec3(slice_dir / vec2(pc.projection[0][0],
                                                               pc.projection[1][1]),
                                             0.0));
        /* Normal angle in the slice plane */
        vec3 ortho_dir = slice_view_dir - dot(slice_view_dir, N) * N;
        vec3 proj_N = N - dot(N, slice_view_dir) * slice_view_dir;
        float n_angle = atan(dot(proj_N, cross(slice_view_dir, vec3(0, 0, -1))),
                             dot(proj_N, vec3(0, 0, -1)));

        /* Search for horizon angles in both directions */
        float max_h1 = -PI * 0.5; /* positive direction */
        float max_h2 = -PI * 0.5; /* negative direction */

        for (int s = 1; s <= steps; s++) {
            float t = (float(s) + noise_step) / float(steps);

            /* Positive direction */
            vec2 uv_pos = v_uv + step_uv * float(s);
            if (uv_pos.x >= 0.0 && uv_pos.x <= 1.0 &&
                uv_pos.y >= 0.0 && uv_pos.y <= 1.0) {
                float d_pos = texture(u_depth, uv_pos).r;
                /* Skip samples across depth discontinuities (background behind
                 * the object).  A sample much farther than the center pixel is
                 * on a different surface and must not contribute occlusion. */
                float depth_delta = abs(d_pos - depth);
                if (depth_delta < 0.05) {
                    vec3 S_pos = view_pos_from_depth(uv_pos, d_pos);
                    vec3 diff = S_pos - P;
                    float dist = length(diff);
                    if (dist > 1e-5) {
                        /* Elevation angle from tangent plane toward sample */
                        float h = atan(diff.z, length(diff.xy));
                        /* Fade out samples at the edge of the radius */
                        float falloff = 1.0 - t * t;
                        h = mix(-PI * 0.5, h, falloff);
                        max_h1 = max(max_h1, h);
                    }
                }
            }

            /* Negative direction */
            vec2 uv_neg = v_uv - step_uv * float(s);
            if (uv_neg.x >= 0.0 && uv_neg.x <= 1.0 &&
                uv_neg.y >= 0.0 && uv_neg.y <= 1.0) {
                float d_neg = texture(u_depth, uv_neg).r;
                float depth_delta = abs(d_neg - depth);
                if (depth_delta < 0.05) {
                    vec3 S_neg = view_pos_from_depth(uv_neg, d_neg);
                    vec3 diff = S_neg - P;
                    float dist = length(diff);
                    if (dist > 1e-5) {
                        float h = atan(diff.z, length(diff.xy));
                        float falloff = 1.0 - t * t;
                        h = mix(-PI * 0.5, h, falloff);
                        max_h2 = max(max_h2, h);
                    }
                }
            }
        }

        /* Clamp horizons to hemisphere */
        max_h1 = clamp(max_h1, -PI * 0.5, PI * 0.5);
        max_h2 = clamp(max_h2, -PI * 0.5, PI * 0.5);

        /* Integrate visible arc */
        visibility += integrate_arc(max_h1, max_h2, n_angle);
    }

    visibility /= float(NUM_DIRECTIONS);
    visibility = clamp(visibility, 0.0, 1.0);

    /* Apply intensity and output */
    frag_ao = mix(1.0, visibility, pc.intensity);
}
