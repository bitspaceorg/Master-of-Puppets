/*
 * Master of Puppets — Vulkan Backend
 * mop_ssr.frag — Screen-Space Reflections via linear ray march
 *
 * Reconstructs view-space position and normal from the depth buffer,
 * computes the reflection direction, and traces in screen space with
 * binary refinement.  On hit, samples the HDR color buffer.  On miss,
 * outputs zero (main-pass IBL handles the fallback).
 *
 * Bindings:
 *   0 — depth buffer (D32_SFLOAT, sampled as R32_SFLOAT)
 *   1 — HDR color buffer (R16G16B16A16_SFLOAT)
 *
 * Push constants:
 *   mat4 projection        — view → clip
 *   mat4 inv_projection    — clip → view
 *   vec2 inv_resolution    — 1/width, 1/height
 *   int  reverse_z         — 1 if reverse-Z depth
 *   float max_distance     — max ray distance in view space
 *   float thickness        — depth comparison threshold
 *   float intensity        — SSR blend weight
 *   float _pad             — align to 16
 *
 * Output: R16G16B16A16_SFLOAT — reflection RGB + confidence alpha
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_depth;
layout(set = 0, binding = 1) uniform sampler2D u_hdr;

layout(push_constant) uniform SsrPush {
    mat4 projection;       /* offset   0 */
    mat4 inv_projection;   /* offset  64 */
    vec2 inv_resolution;   /* offset 128 */
    int  reverse_z;        /* offset 136 */
    float max_distance;    /* offset 140 */
    float thickness;       /* offset 144 */
    float intensity;       /* offset 148 */
    float _pad;            /* offset 152, total 156 — align to 4 */
} ssr;

/* ----- Depth → View position reconstruction ----- */

float linearize_depth(float raw) {
    float z_ndc = ssr.reverse_z != 0 ? (1.0 - raw) * 2.0 - 1.0
                                      : raw * 2.0 - 1.0;
    /* d = M[3][2], c = M[2][2]  (column-major) */
    float c = ssr.projection[2][2];
    float d = ssr.projection[3][2];
    return d / (z_ndc - c);
}

vec3 view_pos_from_depth(vec2 uv, float depth) {
    float z_ndc = ssr.reverse_z != 0 ? (1.0 - depth) * 2.0 - 1.0
                                      : depth * 2.0 - 1.0;
    vec4 clip = vec4(uv * 2.0 - 1.0, z_ndc, 1.0);
    vec4 view = ssr.inv_projection * clip;
    return view.xyz / view.w;
}

/* ----- Project view position to screen UV ----- */

vec3 project_to_uv(vec3 view_pos) {
    vec4 clip = ssr.projection * vec4(view_pos, 1.0);
    clip.xy /= clip.w;
    vec2 uv = clip.xy * 0.5 + 0.5;
    /* Encode linear depth in Z for comparison */
    return vec3(uv, -view_pos.z);
}

/* ----- Screen-space ray march ----- */

#define MAX_STEPS 64
#define BINARY_STEPS 8

bool ray_march(vec3 origin, vec3 dir, out vec2 hit_uv) {
    /* Step size: divide max_distance by step count */
    float step_size = ssr.max_distance / float(MAX_STEPS);
    vec3 step_delta = normalize(dir) * step_size;

    vec3 ray = origin;

    for (int i = 0; i < MAX_STEPS; i++) {
        ray += step_delta;

        /* Project to screen */
        vec3 proj = project_to_uv(ray);

        /* Out of screen? */
        if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
            return false;

        /* Sample depth at projected position */
        float scene_depth = texture(u_depth, proj.xy).r;
        float scene_z = -linearize_depth(scene_depth);

        /* Hit test: ray is behind the surface and within thickness */
        float diff = proj.z - scene_z;
        if (diff > 0.0 && diff < ssr.thickness) {
            /* Binary refinement */
            vec3 back_step = step_delta * 0.5;
            vec3 refine = ray;
            for (int j = 0; j < BINARY_STEPS; j++) {
                refine -= back_step;
                vec3 rp = project_to_uv(refine);
                float rd = texture(u_depth, rp.xy).r;
                float rz = -linearize_depth(rd);
                float rdiff = rp.z - rz;
                if (rdiff > 0.0 && rdiff < ssr.thickness)
                    refine += back_step;
                back_step *= 0.5;
            }
            hit_uv = project_to_uv(refine).xy;
            return true;
        }
    }
    return false;
}

/* ----- Edge fade: attenuate near screen borders ----- */

float screen_edge_fade(vec2 uv) {
    vec2 fade = smoothstep(0.0, 0.1, uv) * (1.0 - smoothstep(0.9, 1.0, uv));
    return fade.x * fade.y;
}

void main() {
    float depth = texture(u_depth, v_uv).r;

    /* Skip far plane (no geometry) */
    if (ssr.reverse_z != 0) {
        if (depth < 0.0001) { out_color = vec4(0.0); return; }
    } else {
        if (depth > 0.9999) { out_color = vec4(0.0); return; }
    }

    /* Reconstruct view position */
    vec3 view_pos = view_pos_from_depth(v_uv, depth);

    /* Reconstruct normal from depth derivatives */
    vec3 dpdx = dFdx(view_pos);
    vec3 dpdy = dFdy(view_pos);
    vec3 normal = normalize(cross(dpdx, dpdy));

    /* Compute view direction and reflection */
    vec3 view_dir = normalize(view_pos);
    vec3 reflect_dir = reflect(view_dir, normal);

    /* Skip if reflecting towards the camera (behind surface) */
    if (reflect_dir.z > 0.0) {
        out_color = vec4(0.0);
        return;
    }

    /* Ray march in view space */
    vec2 hit_uv;
    if (ray_march(view_pos, reflect_dir, hit_uv)) {
        vec3 reflection = texture(u_hdr, hit_uv).rgb;
        float fade = screen_edge_fade(hit_uv);
        out_color = vec4(reflection * ssr.intensity * fade, fade);
    } else {
        out_color = vec4(0.0);
    }
}
