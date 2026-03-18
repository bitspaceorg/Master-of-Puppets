/*
 * Master of Puppets — Vulkan Backend
 * mop_oit_accum.frag — Weighted Blended OIT accumulation (McGuire/Bavoil 2013)
 *
 * Same PBR lighting as mop_solid_bindless.frag, but writes to two MRTs:
 *   location 0: accum  (R16G16B16A16_SFLOAT) — weighted color + weighted alpha
 *   location 1: revealage (R8_UNORM) — product of (1 - alpha_i)
 *
 * Blending configured by the pipeline:
 *   accum:     ONE, ONE, ADD (additive accumulation)
 *   revealage: ZERO, ONE_MINUS_SRC_COLOR, ADD (multiplicative reveal)
 *
 * Same descriptor layout as solid_bindless (bindless_pipeline_layout).
 * Same vertex shader (solid_bindless.vert).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_texcoord;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) flat in uint v_draw_id;

/* Per-object data (std430, 144 bytes each). */
struct ObjectData {
    mat4  model;
    float ambient;
    float opacity;
    uint  object_id;
    int   blend_mode;
    float metallic;
    float roughness;
    int   base_tex_idx;
    int   normal_tex_idx;
    vec4  emissive;
    int   mr_tex_idx;
    int   ao_tex_idx;
    int   _pad0;
    int   _pad1;
    vec4  bound_sphere;
};

layout(std430, set = 0, binding = 0) readonly buffer ObjectSSBO {
    ObjectData objects[];
} obj_buf;

layout(std140, set = 0, binding = 1) uniform FrameGlobals {
    vec4  light_dir;
    vec4  cam_pos;
    int   shadows_enabled;
    int   cascade_count;
    int   num_lights;
    float exposure;
    mat4  cascade_vp[4];
    vec4  cascade_splits;
} frame;

struct Light {
    vec4 position;
    vec4 direction;
    vec4 color;
    vec4 params;
};

layout(std430, set = 0, binding = 2) readonly buffer LightSSBO {
    Light lights[];
} light_buf;

layout(set = 0, binding = 3) uniform sampler2DArrayShadow u_shadow_map;
layout(set = 0, binding = 4) uniform sampler2D u_irradiance_map;
layout(set = 0, binding = 5) uniform sampler2D u_prefiltered_map;
layout(set = 0, binding = 6) uniform sampler2D u_brdf_lut;
layout(set = 0, binding = 7) uniform sampler2D u_textures[];

/* OIT outputs: 2 MRTs */
layout(location = 0) out vec4 out_accum;
layout(location = 1) out vec4 out_revealage;

const float PI = 3.14159265359;

float distribution_ggx(float ndoth, float alpha2) {
    float denom = ndoth * ndoth * (alpha2 - 1.0) + 1.0;
    return alpha2 / (PI * denom * denom);
}

float geometry_schlick_g1(float ndotx, float k) {
    return ndotx / (ndotx * (1.0 - k) + k);
}

vec3 fresnel_schlick(float vdoth, vec3 f0) {
    float om = 1.0 - vdoth;
    float om2 = om * om;
    float om5 = om2 * om2 * om;
    return f0 + (1.0 - f0) * om5;
}

float compute_shadow(vec3 world_pos) {
    if (frame.shadows_enabled == 0) return 1.0;
    float view_z = length(frame.cam_pos.xyz - world_pos);
    int cascade = frame.cascade_count - 1;
    for (int i = 0; i < frame.cascade_count; i++) {
        if (view_z < frame.cascade_splits[i]) { cascade = i; break; }
    }
    vec4 light_clip = frame.cascade_vp[cascade] * vec4(world_pos, 1.0);
    vec3 proj_coords = light_clip.xyz / light_clip.w;
    vec2 shadow_uv = proj_coords.xy * 0.5 + 0.5;
    float compare_depth = proj_coords.z;
    if (shadow_uv.x < 0.0 || shadow_uv.x > 1.0 ||
        shadow_uv.y < 0.0 || shadow_uv.y > 1.0 ||
        compare_depth < 0.0 || compare_depth > 1.0) return 1.0;
    float texel_size = 1.0 / 2048.0;
    float shadow = 0.0;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            shadow += texture(u_shadow_map,
                vec4(shadow_uv + vec2(float(dx), float(dy)) * texel_size,
                     float(cascade), compare_depth));
    return shadow / 9.0;
}

vec2 dir_to_equirect(vec3 dir) {
    float phi = atan(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    return vec2(phi / (2.0 * PI) + 0.5, 0.5 - theta / PI);
}

vec3 perturb_normal(vec3 n, vec3 world_pos, vec2 uv, int normal_idx) {
    vec3 ts = texture(u_textures[nonuniformEXT(normal_idx)], uv).rgb * 2.0 - 1.0;
    vec3 dp1 = dFdx(world_pos), dp2 = dFdy(world_pos);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    return normalize(mat3(T * invmax, B * invmax, n) * ts);
}

/* McGuire/Bavoil 2013 weight function */
float oit_weight(float z, float alpha) {
    /* Depth-based weighting: closer fragments get higher weight.
     * Clamp to prevent overflow/underflow. */
    return alpha * clamp(0.03 / (1e-5 + pow(z / 200.0, 4.0)), 1e-2, 3e3);
}

void main() {
    ObjectData obj = obj_buf.objects[v_draw_id];
    vec3 n = normalize(v_normal);
    vec3 world_pos = v_world_pos;

    if (obj.normal_tex_idx >= 0)
        n = perturb_normal(n, world_pos, v_texcoord, obj.normal_tex_idx);

    vec4 base = v_color;
    if (obj.base_tex_idx >= 0)
        base *= texture(u_textures[nonuniformEXT(obj.base_tex_idx)], v_texcoord);

    float mtl = obj.metallic, rough = obj.roughness;
    if (obj.mr_tex_idx >= 0) {
        vec4 mr = texture(u_textures[nonuniformEXT(obj.mr_tex_idx)], v_texcoord);
        rough = mr.g; mtl = mr.b;
    }
    float ao = 1.0;
    if (obj.ao_tex_idx >= 0)
        ao = texture(u_textures[nonuniformEXT(obj.ao_tex_idx)], v_texcoord).r;

    float alpha = rough * rough;
    float alpha2 = max(alpha * alpha, 1e-7);
    vec3 view_dir = normalize(frame.cam_pos.xyz - world_pos);
    float ndotv = max(dot(n, view_dir), 1e-4);
    float k = alpha * 0.5;
    float g1v = geometry_schlick_g1(ndotv, k);
    vec3 f0 = mix(vec3(0.04), base.rgb, mtl);
    float diffuse_scale = 1.0 - mtl;

    vec3 lit;
    if (frame.num_lights > 0) {
        vec3 diffuse_accum = vec3(0.0), specular_accum = vec3(0.0);
        float shadow_factor = compute_shadow(world_pos);
        for (int i = 0; i < frame.num_lights; i++) {
            if (light_buf.lights[i].params.w < 0.5) continue;
            int lt = int(light_buf.lights[i].position.w + 0.5);
            float atten = 1.0, spot = 1.0;
            float intensity = light_buf.lights[i].color.w;
            vec3 lc = light_buf.lights[i].color.rgb;
            vec3 l_dir; float lshadow = 1.0;
            if (lt == 0) {
                l_dir = normalize(light_buf.lights[i].direction.xyz);
                lshadow = shadow_factor;
            } else {
                vec3 to_l = light_buf.lights[i].position.xyz - world_pos;
                float d = length(to_l);
                l_dir = to_l / max(d, 1e-6);
                float range = light_buf.lights[i].params.x;
                if (range > 0.0) { float r = d/range; atten = max(1.0-r,0.0); atten *= atten; }
                else { atten = 1.0 / max(d*d, 0.01); }
                if (lt == 2) {
                    vec3 sd = normalize(light_buf.lights[i].direction.xyz);
                    float ca = -dot(l_dir, sd);
                    float oc = light_buf.lights[i].params.z;
                    float ic = light_buf.lights[i].params.y;
                    if (ca < oc) spot = 0.0;
                    else if (ca < ic) { float rv = ic-oc; if(rv>1e-6){float t=(ca-oc)/rv; spot=t*t*(3.0-2.0*t);}}
                }
            }
            float ndotl = max(dot(n, l_dir), 0.0);
            if (ndotl <= 0.0) continue;
            float w = intensity * atten * spot * ndotl * lshadow;
            diffuse_accum += w * lc;
            if (w > 0.0) {
                vec3 h = normalize(l_dir + view_dir);
                float ndoth = max(dot(n, h), 0.0);
                float vdoth = max(dot(view_dir, h), 0.0);
                float D = distribution_ggx(ndoth, alpha2);
                float G = g1v * geometry_schlick_g1(ndotl, k);
                vec3 F = fresnel_schlick(vdoth, f0);
                float sd = max(4.0*ndotl*ndotv, 1e-6);
                specular_accum += min(D*G/sd, 100.0) * PI * w * F * lc;
            }
        }
        lit = base.rgb * diffuse_accum * diffuse_scale + specular_accum;
        vec2 irr_uv = dir_to_equirect(n);
        vec3 irr = texture(u_irradiance_map, irr_uv).rgb;
        if (dot(irr, vec3(0.299,0.587,0.114)) > 0.001)
            lit += base.rgb * irr * diffuse_scale;
        else
            lit += base.rgb * obj.ambient * diffuse_scale;
        vec3 rd = reflect(-view_dir, n);
        vec3 pf = texture(u_prefiltered_map, dir_to_equirect(rd)).rgb;
        vec2 bv = texture(u_brdf_lut, vec2(ndotv, rough)).rg;
        if (dot(pf, vec3(0.299,0.587,0.114)) > 0.001)
            lit += pf * (f0 * bv.x + bv.y);
        lit *= ao;
        lit += obj.emissive.rgb;
        lit *= frame.exposure;
        lit = clamp(lit, 0.0, 100.0);
    } else {
        vec3 l = normalize(frame.light_dir.xyz);
        float ndotl = max(dot(n, l), 0.0);
        float lighting = clamp(obj.ambient + (1.0 - obj.ambient) * ndotl, 0.0, 1.0);
        lit = (base.rgb * lighting + obj.emissive.rgb) * frame.exposure;
    }

    /* OIT weighting */
    float a = base.a * obj.opacity;
    float z = gl_FragCoord.z; /* 0..1 window-space depth */
    float w = oit_weight(gl_FragCoord.z / gl_FragCoord.w, a);

    /* Accumulation: weighted premultiplied color + weighted alpha */
    out_accum = vec4(lit * a * w, a * w);

    /* Revealage: output alpha so blend unit computes prev * (1 - alpha) */
    out_revealage = vec4(a, a, a, a);
}
