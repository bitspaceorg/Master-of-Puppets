#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_solid.frag — Fragment shader with GGX Cook-Torrance PBR + multi-light
 *                  + cascade shadow mapping + PBR texture maps
 *
 * Dynamic UBO (set=0, binding=0): per-draw fragment uniforms
 * Combined image sampler (set=0, binding=1): texture or 1x1 white fallback
 * Shadow sampler (set=0, binding=2): cascade shadow map array (comparison)
 * Bindings 3-5: IBL (irradiance, prefiltered, BRDF LUT)
 * Binding 6: normal map
 * Binding 7: metallic-roughness map (glTF: G=roughness, B=metallic)
 * Binding 8: ambient occlusion map (R channel)
 *
 * Outputs to two attachments:
 *   location 0: vec4  color    (R8G8B8A8_SRGB)
 *   location 1: uint  object_id (R32_UINT)
 *
 * Shading model matches CPU rasterizer (rasterizer.c smooth_ml path):
 *   Diffuse:  Lambert (no 1/pi — cancels with sun energy = intensity*pi)
 *   Specular: GGX Cook-Torrance (D=Trowbridge-Reitz, G=Smith-Schlick, F=Schlick)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_texcoord;
layout(location = 3) in vec3 v_world_pos;

struct Light {
    vec4 position;    /* xyz + type (0=dir, 1=point, 2=spot) */
    vec4 direction;   /* xyz + padding */
    vec4 color;       /* rgb + intensity */
    vec4 params;      /* range, spot_inner_cos, spot_outer_cos, active */
};

layout(set = 0, binding = 0) uniform FragUniforms {
    vec4  light_dir;     /* xyz = direction, w = unused */
    float ambient;
    float opacity;
    uint  object_id;
    int   blend_mode;
    int   has_texture;
    int   num_lights;
    float metallic;
    float roughness;
    int   has_normal_map;
    int   has_mr_map;
    int   has_ao_map;
    int   _pad_maps;
    vec4  cam_pos;       /* xyz = camera eye position, w = unused */
    vec4  emissive;      /* rgb = emissive color, w = unused */
    Light lights[8];

    /* Shadow mapping (cascade) */
    int   shadows_enabled;       /* bool: 1 = shadows active, 0 = off */
    int   cascade_count;         /* number of active cascades (1-4) */
    float _pad_s0;               /* align to vec4 (scalars, not arrays — */
    float _pad_s1;               /* std140 pads array elements to 16B!) */
    mat4  cascade_vp[4];         /* light-space VP matrix per cascade */
    vec4  cascade_splits;        /* view-space Z split distances */
    float exposure;              /* HDR exposure (scene objects only) */
    float _pad_e0;
    float _pad_e1;
    float _pad_e2;
} frag;

layout(set = 0, binding = 1) uniform sampler2D u_texture;
layout(set = 0, binding = 2) uniform sampler2DArrayShadow u_shadow_map;
layout(set = 0, binding = 3) uniform sampler2D u_irradiance_map;
layout(set = 0, binding = 4) uniform sampler2D u_prefiltered_map;
layout(set = 0, binding = 5) uniform sampler2D u_brdf_lut;
layout(set = 0, binding = 6) uniform sampler2D u_normal_map;
layout(set = 0, binding = 7) uniform sampler2D u_mr_map;
layout(set = 0, binding = 8) uniform sampler2D u_ao_map;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out uint frag_object_id;

const float PI = 3.14159265359;

/* GGX Normal Distribution (Trowbridge-Reitz)
 * D = alpha^2 / (pi * ((NdotH)^2 * (alpha^2 - 1) + 1)^2) */
float distribution_ggx(float ndoth, float alpha2) {
    float denom = ndoth * ndoth * (alpha2 - 1.0) + 1.0;
    return alpha2 / (PI * denom * denom);
}

/* Smith-Schlick G1 term
 * G1(x) = NdotX / (NdotX * (1 - k) + k) */
float geometry_schlick_g1(float ndotx, float k) {
    return ndotx / (ndotx * (1.0 - k) + k);
}

/* Schlick Fresnel approximation (per-channel)
 * F = F0 + (1 - F0) * (1 - VdotH)^5 */
vec3 fresnel_schlick(float vdoth, vec3 f0) {
    float om = 1.0 - vdoth;
    float om2 = om * om;
    float om5 = om2 * om2 * om;
    return f0 + (1.0 - f0) * om5;
}

/* Compute shadow factor for the given world position using cascade shadow maps.
 * Returns 1.0 = fully lit, 0.0 = fully shadowed. */
float compute_shadow(vec3 world_pos) {
    if (frag.shadows_enabled == 0) return 1.0;

    /* Determine cascade based on view-space Z (approximated from camera distance) */
    float view_z = length(frag.cam_pos.xyz - world_pos);
    int cascade = frag.cascade_count - 1;
    for (int i = 0; i < frag.cascade_count; i++) {
        if (view_z < frag.cascade_splits[i]) {
            cascade = i;
            break;
        }
    }

    /* Transform to light clip space */
    vec4 light_clip = frag.cascade_vp[cascade] * vec4(world_pos, 1.0);
    vec3 proj_coords = light_clip.xyz / light_clip.w;

    /* Remap from [-1,1] to [0,1] */
    vec2 shadow_uv = proj_coords.xy * 0.5 + 0.5;
    float compare_depth = proj_coords.z;

    /* Out-of-range = fully lit */
    if (shadow_uv.x < 0.0 || shadow_uv.x > 1.0 ||
        shadow_uv.y < 0.0 || shadow_uv.y > 1.0 ||
        compare_depth < 0.0 || compare_depth > 1.0) {
        return 1.0;
    }

    /* PCF 3x3: 9 samples averaged for soft shadow edges */
    float texel_size = 1.0 / 2048.0; /* MOP_VK_SHADOW_MAP_SIZE */
    float shadow = 0.0;
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            vec2 offset = vec2(float(dx), float(dy)) * texel_size;
            vec2 sample_uv = shadow_uv + offset;
            /* sampler2DArrayShadow: texture(sampler, vec4(uv, layer, compare)) */
            shadow += texture(u_shadow_map,
                              vec4(sample_uv, float(cascade), compare_depth));
        }
    }
    shadow /= 9.0;

    return shadow;
}

/* Sample equirectangular map at a direction.
 * Standard equirect: top = north pole, bottom = south pole.
 * phi maps to u, theta maps to v (flipped so top = +Y). */
vec2 dir_to_equirect(vec3 dir) {
    float phi = atan(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    return vec2(phi / (2.0 * PI) + 0.5, 0.5 - theta / PI);
}

/* Cotangent-frame TBN reconstruction (Mikkelsen 2010).
 * Computes tangent space from screen-space derivatives of world position
 * and UVs — no vertex tangent attribute needed. */
vec3 perturb_normal(vec3 n, vec3 world_pos, vec2 uv) {
    vec3 ts = texture(u_normal_map, uv).rgb * 2.0 - 1.0;

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
    vec3 n = normalize(v_normal);
    vec3 world_pos = v_world_pos;

    /* Normal map perturbation (cotangent-frame TBN) */
    if (frag.has_normal_map != 0) {
        n = perturb_normal(n, world_pos, v_texcoord);
    }

    /* Sample base color */
    vec4 base = v_color;
    if (frag.has_texture != 0) {
        base *= texture(u_texture, v_texcoord);
    }

    float mtl = frag.metallic;
    float rough = frag.roughness;

    /* Metallic-roughness map override (glTF convention: G=roughness, B=metallic) */
    if (frag.has_mr_map != 0) {
        vec4 mr = texture(u_mr_map, v_texcoord);
        rough = mr.g;
        mtl = mr.b;
    }

    /* Ambient occlusion map */
    float ao = 1.0;
    if (frag.has_ao_map != 0) {
        ao = texture(u_ao_map, v_texcoord).r;
    }

    /* GGX alpha mapping: alpha = roughness^2 */
    float alpha = rough * rough;
    float alpha2 = alpha * alpha;
    if (alpha2 < 1e-7) alpha2 = 1e-7;

    /* View direction */
    vec3 view_dir = normalize(frag.cam_pos.xyz - world_pos);
    float ndotv = max(dot(n, view_dir), 1e-4);

    /* Smith-Schlick k = alpha / 2 (Disney/Unreal mapping) */
    float k = alpha * 0.5;
    float g1v = geometry_schlick_g1(ndotv, k);

    /* F0: Fresnel at normal incidence */
    vec3 f0 = mix(vec3(0.04), base.rgb, mtl);

    /* Diffuse scale: metals have no diffuse */
    float diffuse_scale = 1.0 - mtl;

    /* Accumulate lighting */
    vec3 diffuse_accum = vec3(0.0);
    vec3 specular_accum = vec3(0.0);

    if (frag.num_lights > 0) {
        /* Compute shadow factor once for directional lights */
        float shadow_factor = compute_shadow(world_pos);

        for (int i = 0; i < frag.num_lights; i++) {
            if (frag.lights[i].params.w < 0.5) continue; /* inactive */

            int light_type = int(frag.lights[i].position.w + 0.5);
            float attenuation = 1.0;
            float spot_factor = 1.0;
            float intensity = frag.lights[i].color.w;
            vec3 light_color = frag.lights[i].color.rgb;
            vec3 l_dir;

            /* Apply shadow only to directional lights (type 0) */
            float light_shadow = 1.0;

            if (light_type == 0) {
                /* Directional */
                l_dir = normalize(frag.lights[i].direction.xyz);
                light_shadow = shadow_factor;
            } else if (light_type == 1) {
                /* Point */
                vec3 to_light = frag.lights[i].position.xyz - world_pos;
                float dist = length(to_light);
                l_dir = to_light / max(dist, 1e-6);

                float range = frag.lights[i].params.x;
                if (range > 0.0) {
                    float r = dist / range;
                    attenuation = max(1.0 - r, 0.0);
                    attenuation *= attenuation;
                } else {
                    float d2 = max(dist * dist, 0.01);
                    attenuation = 1.0 / d2;
                }
            } else {
                /* Spot */
                vec3 to_light = frag.lights[i].position.xyz - world_pos;
                float dist = length(to_light);
                l_dir = to_light / max(dist, 1e-6);

                vec3 spot_dir = normalize(frag.lights[i].direction.xyz);
                float cos_angle = -dot(l_dir, spot_dir);
                float outer_cos = frag.lights[i].params.z;
                float inner_cos = frag.lights[i].params.y;

                if (cos_angle < outer_cos) {
                    spot_factor = 0.0;
                } else if (cos_angle < inner_cos) {
                    float range_val = inner_cos - outer_cos;
                    if (range_val > 1e-6) {
                        float t = (cos_angle - outer_cos) / range_val;
                        spot_factor = t * t * (3.0 - 2.0 * t); /* smoothstep */
                    }
                }

                float range = frag.lights[i].params.x;
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

            /* Lambert diffuse (per-channel light color) */
            diffuse_accum += weight * light_color;

            /* GGX Cook-Torrance specular */
            vec3 h = normalize(l_dir + view_dir);
            float ndoth = max(dot(n, h), 0.0);
            float vdoth = max(dot(view_dir, h), 0.0);

            float D = distribution_ggx(ndoth, alpha2);
            float g1l = geometry_schlick_g1(ndotl, k);
            float G = g1v * g1l;
            vec3 F = fresnel_schlick(vdoth, f0);

            float spec_denom = max(4.0 * ndotl * ndotv, 1e-6);
            float spec_term = D * G / spec_denom;

            /* Multiply by PI to match energy scale (diffuse omits 1/PI) */
            specular_accum += spec_term * PI * intensity * attenuation *
                              spot_factor * ndotl * light_shadow *
                              F * light_color;
        }

        /* Final composition */
        vec3 lit = base.rgb * diffuse_accum * diffuse_scale + specular_accum;

        /* IBL diffuse: sample irradiance map at surface normal.
         * Falls back to flat ambient if irradiance map is 1x1 fallback. */
        vec2 irr_uv = dir_to_equirect(n);
        vec3 irr_sample = texture(u_irradiance_map, irr_uv).rgb;
        /* If irradiance > 0 (real env map loaded), use it instead of flat ambient */
        float irr_lum = dot(irr_sample, vec3(0.299, 0.587, 0.114));
        if (irr_lum > 0.001) {
            lit += base.rgb * irr_sample * diffuse_scale;
        } else {
            lit += base.rgb * frag.ambient * diffuse_scale;
        }

        /* IBL specular: split-sum approximation */
        vec3 reflect_dir = reflect(-view_dir, n);
        vec2 pf_uv = dir_to_equirect(reflect_dir);
        vec3 pf_sample = texture(u_prefiltered_map, pf_uv).rgb;
        vec2 brdf_val = texture(u_brdf_lut, vec2(ndotv, rough)).rg;
        float pf_lum = dot(pf_sample, vec3(0.299, 0.587, 0.114));
        if (pf_lum > 0.001) {
            lit += pf_sample * (f0 * brdf_val.x + brdf_val.y);
        }

        /* Apply AO and emissive */
        lit *= ao;
        lit += frag.emissive.rgb;

        frag_color = vec4(max(lit, vec3(0.0)) * frag.exposure, base.a * frag.opacity);
    } else {
        /* Legacy single-light fallback (no PBR) */
        vec3 l = normalize(frag.light_dir.xyz);
        float ndotl = max(dot(n, l), 0.0);
        float lighting = clamp(frag.ambient + (1.0 - frag.ambient) * ndotl, 0.0, 1.0);
        vec3 lit = base.rgb * lighting + frag.emissive.rgb;
        frag_color = vec4(lit * frag.exposure, base.a * frag.opacity);
    }

    frag_object_id = frag.object_id;
}
