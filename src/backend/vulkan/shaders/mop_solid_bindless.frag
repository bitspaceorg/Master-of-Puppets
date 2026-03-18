#version 450
#extension GL_EXT_nonuniform_qualifier : require

/*
 * Master of Puppets — Vulkan Backend
 * mop_solid_bindless.frag — Bindless fragment shader (Phase 2A)
 *
 * Replaces per-draw UBO + individual texture bindings with:
 *   - Object SSBO: per-object material data indexed by draw_id
 *   - Frame globals UBO: camera, shadow, exposure (shared across all draws)
 *   - Light SSBO: all scene lights (shared)
 *   - Bindless texture array: sampler2D textures[] indexed by texture ID
 *
 * Single descriptor set per frame — no per-draw descriptor allocation.
 *
 * Descriptor layout (set 0):
 *   binding 0: readonly buffer ObjectSSBO { ObjectData objects[]; }
 *   binding 1: uniform FrameGlobals { ... }
 *   binding 2: readonly buffer LightSSBO { Light lights[]; }
 *   binding 3: sampler2DArrayShadow shadow map
 *   binding 4: sampler2D irradiance map
 *   binding 5: sampler2D prefiltered map
 *   binding 6: sampler2D BRDF LUT
 *   binding 7: sampler2D textures[] (bindless array, variable count)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) centroid in vec3 v_normal;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_texcoord;
layout(location = 3) centroid in vec3 v_world_pos;
layout(location = 4) flat in uint v_draw_id;

/* Per-object data (std430, 144 bytes each).  Indexed by v_draw_id. */
struct ObjectData {
    mat4  model;          /* offset  0 */
    float ambient;        /* offset 64 */
    float opacity;        /* offset 68 */
    uint  object_id;      /* offset 72 */
    int   blend_mode;     /* offset 76 */
    float metallic;       /* offset 80 */
    float roughness;      /* offset 84 */
    int   base_tex_idx;   /* offset 88, -1 = none */
    int   normal_tex_idx; /* offset 92, -1 = none */
    vec4  emissive;       /* offset 96 */
    int   mr_tex_idx;     /* offset 112, -1 = none */
    int   ao_tex_idx;     /* offset 116, -1 = none */
    int   _pad0;          /* offset 120 */
    int   _pad1;          /* offset 124 */
    vec4  bound_sphere;   /* offset 128: local-space center xyz + radius */
};

layout(std430, set = 0, binding = 0) readonly buffer ObjectSSBO {
    ObjectData objects[];
} obj_buf;

/* Per-frame global data (shared across all draws) */
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

/* Light SSBO */
struct Light {
    vec4 position;    /* xyz + type */
    vec4 direction;   /* xyz + padding */
    vec4 color;       /* rgb + intensity */
    vec4 params;      /* range, spot_inner_cos, spot_outer_cos, active */
};

layout(std430, set = 0, binding = 2) readonly buffer LightSSBO {
    Light lights[];
} light_buf;

/* Shadow map (cascade array, comparison sampler) */
layout(set = 0, binding = 3) uniform sampler2DArrayShadow u_shadow_map;

/* IBL textures */
layout(set = 0, binding = 4) uniform sampler2D u_irradiance_map;
layout(set = 0, binding = 5) uniform sampler2D u_prefiltered_map;
layout(set = 0, binding = 6) uniform sampler2D u_brdf_lut;

/* Bindless texture array — indexed by per-object texture indices.
 * Uses nonuniform qualifier since different fragments may access
 * different array elements. */
layout(set = 0, binding = 7) uniform sampler2D u_textures[];

layout(location = 0) out vec4 frag_color;
layout(location = 1) out uint frag_object_id;

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

float compute_shadow(vec3 world_pos, vec3 normal) {
    if (frame.shadows_enabled == 0) return 1.0;

    float view_z = length(frame.cam_pos.xyz - world_pos);
    int cascade = frame.cascade_count - 1;
    for (int i = 0; i < frame.cascade_count; i++) {
        if (view_z < frame.cascade_splits[i]) {
            cascade = i;
            break;
        }
    }

    vec4 light_clip = frame.cascade_vp[cascade] * vec4(world_pos, 1.0);
    vec3 proj_coords = light_clip.xyz / light_clip.w;
    vec2 shadow_uv = proj_coords.xy * 0.5 + 0.5;

    /* Normal-offset bias: surfaces facing away from the light need more bias */
    vec3 l = normalize(frame.light_dir.xyz);
    float cos_theta = max(dot(normal, l), 0.0);
    float bias = mix(0.005, 0.001, cos_theta);
    float compare_depth = proj_coords.z - bias;

    if (shadow_uv.x < 0.0 || shadow_uv.x > 1.0 ||
        shadow_uv.y < 0.0 || shadow_uv.y > 1.0 ||
        compare_depth < 0.0 || compare_depth > 1.0) {
        return 1.0;
    }

    float texel_size = 1.0 / 2048.0;
    float shadow = 0.0;
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            vec2 offset = vec2(float(dx), float(dy)) * texel_size;
            vec2 sample_uv = shadow_uv + offset;
            shadow += texture(u_shadow_map,
                              vec4(sample_uv, float(cascade), compare_depth));
        }
    }
    shadow /= 9.0;
    return shadow;
}

vec2 dir_to_equirect(vec3 dir) {
    float phi = atan(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    return vec2(phi / (2.0 * PI) + 0.5, 0.5 - theta / PI);
}

vec3 perturb_normal(vec3 n, vec3 world_pos, vec2 uv, int normal_idx) {
    vec3 ts = texture(u_textures[nonuniformEXT(normal_idx)], uv).rgb * 2.0 - 1.0;

    vec3 dp1 = dFdx(world_pos);
    vec3 dp2 = dFdy(world_pos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, n);
    vec3 dp1perp = cross(n, dp1);

    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    mat3 TBN = mat3(T * invmax, B * invmax, n);
    return normalize(TBN * ts);
}

void main() {
    ObjectData obj = obj_buf.objects[v_draw_id];

    vec3 n = normalize(v_normal);
    vec3 world_pos = v_world_pos;

    /* Normal map */
    if (obj.normal_tex_idx >= 0) {
        n = perturb_normal(n, world_pos, v_texcoord, obj.normal_tex_idx);
    }

    /* Base color from vertex color * optional texture */
    vec4 base = v_color;
    if (obj.base_tex_idx >= 0) {
        base *= texture(u_textures[nonuniformEXT(obj.base_tex_idx)], v_texcoord);
    }

    float mtl = obj.metallic;
    float rough = obj.roughness;

    /* Metallic-roughness map */
    if (obj.mr_tex_idx >= 0) {
        vec4 mr = texture(u_textures[nonuniformEXT(obj.mr_tex_idx)], v_texcoord);
        rough = mr.g;
        mtl = mr.b;
    }

    /* AO map */
    float ao = 1.0;
    if (obj.ao_tex_idx >= 0) {
        ao = texture(u_textures[nonuniformEXT(obj.ao_tex_idx)], v_texcoord).r;
    }

    float alpha = rough * rough;
    float alpha2 = alpha * alpha;
    if (alpha2 < 1e-7) alpha2 = 1e-7;

    vec3 view_dir = normalize(frame.cam_pos.xyz - world_pos);
    float ndotv = max(dot(n, view_dir), 1e-4);
    float k = alpha * 0.5;
    float g1v = geometry_schlick_g1(ndotv, k);
    vec3 f0 = mix(vec3(0.04), base.rgb, mtl);
    float diffuse_scale = 1.0 - mtl;

    vec3 diffuse_accum = vec3(0.0);
    vec3 specular_accum = vec3(0.0);

    if (frame.num_lights > 0) {
        float shadow_factor = compute_shadow(world_pos, n);

        for (int i = 0; i < frame.num_lights; i++) {
            if (light_buf.lights[i].params.w < 0.5) continue;

            int light_type = int(light_buf.lights[i].position.w + 0.5);
            float attenuation = 1.0;
            float spot_factor = 1.0;
            float intensity = light_buf.lights[i].color.w;
            vec3 light_color = light_buf.lights[i].color.rgb;
            vec3 l_dir;
            float light_shadow = 1.0;

            if (light_type == 0) {
                /* Negate: direction = where light shines, need surface-to-light */
                l_dir = -normalize(light_buf.lights[i].direction.xyz);
                light_shadow = shadow_factor;
            } else if (light_type == 1) {
                vec3 to_light = light_buf.lights[i].position.xyz - world_pos;
                float dist = length(to_light);
                l_dir = to_light / max(dist, 1e-6);
                float range = light_buf.lights[i].params.x;
                if (range > 0.0) {
                    float r = dist / range;
                    attenuation = max(1.0 - r, 0.0);
                    attenuation *= attenuation;
                } else {
                    float d2 = max(dist * dist, 0.01);
                    attenuation = 1.0 / d2;
                }
            } else {
                vec3 to_light = light_buf.lights[i].position.xyz - world_pos;
                float dist = length(to_light);
                l_dir = to_light / max(dist, 1e-6);
                vec3 spot_dir = normalize(light_buf.lights[i].direction.xyz);
                float cos_angle = -dot(l_dir, spot_dir);
                float outer_cos = light_buf.lights[i].params.z;
                float inner_cos = light_buf.lights[i].params.y;
                if (cos_angle < outer_cos) {
                    spot_factor = 0.0;
                } else if (cos_angle < inner_cos) {
                    float range_val = inner_cos - outer_cos;
                    if (range_val > 1e-6) {
                        float t = (cos_angle - outer_cos) / range_val;
                        spot_factor = t * t * (3.0 - 2.0 * t);
                    }
                }
                float range = light_buf.lights[i].params.x;
                if (range > 0.0) {
                    float r = dist / range;
                    attenuation = max(1.0 - r, 0.0);
                    attenuation *= attenuation;
                } else {
                    float d2 = max(dist * dist, 0.01);
                    attenuation = 1.0 / d2;
                }
            }

            float ndotl = max(dot(n, l_dir), 0.0);
            if (ndotl <= 0.0) continue;

            float weight = intensity * attenuation * spot_factor * ndotl *
                           light_shadow;

            diffuse_accum += weight * light_color;

            if (weight > 0.0) {
                vec3 h = normalize(l_dir + view_dir);
                float ndoth = max(dot(n, h), 0.0);
                float vdoth = max(dot(view_dir, h), 0.0);
                float D = distribution_ggx(ndoth, alpha2);
                float g1l = geometry_schlick_g1(ndotl, k);
                float G = g1v * g1l;
                vec3 F = fresnel_schlick(vdoth, f0);
                float spec_denom = max(4.0 * ndotl * ndotv, 1e-6);
                float spec_term = min(D * G / spec_denom, 8.0);
                specular_accum += spec_term * PI * weight * F * light_color;
            }
        }

        vec3 lit = base.rgb * diffuse_accum * diffuse_scale + specular_accum;

        vec2 irr_uv = dir_to_equirect(n);
        vec3 irr_sample = texture(u_irradiance_map, irr_uv).rgb;
        float irr_lum = dot(irr_sample, vec3(0.299, 0.587, 0.114));
        if (irr_lum > 0.001) {
            lit += base.rgb * irr_sample * diffuse_scale;
        } else {
            lit += base.rgb * obj.ambient * diffuse_scale;
        }

        vec3 reflect_dir = reflect(-view_dir, n);
        vec2 pf_uv = dir_to_equirect(reflect_dir);
        vec3 pf_sample = texture(u_prefiltered_map, pf_uv).rgb;
        vec2 brdf_val = texture(u_brdf_lut, vec2(ndotv, rough)).rg;
        float pf_lum = dot(pf_sample, vec3(0.299, 0.587, 0.114));
        if (pf_lum > 0.001) {
            lit += pf_sample * (f0 * brdf_val.x + brdf_val.y);
        }

        lit *= ao;
        lit += obj.emissive.rgb;

        vec3 final_color = lit * frame.exposure;
        final_color = clamp(final_color, vec3(0.0), vec3(16.0));
        frag_color = vec4(final_color, base.a * obj.opacity);
    } else {
        vec3 l = normalize(frame.light_dir.xyz);
        float ndotl = max(dot(n, l), 0.0);
        float lighting = clamp(obj.ambient + (1.0 - obj.ambient) * ndotl, 0.0, 1.0);
        vec3 lit = base.rgb * lighting + obj.emissive.rgb;
        frag_color = vec4(lit * frame.exposure, base.a * obj.opacity);
    }

    frag_object_id = obj.object_id;
}
