#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_solid.frag — Fragment shader with multi-light support + texture
 *
 * Dynamic UBO (set=0, binding=0): per-draw fragment uniforms
 * Combined image sampler (set=0, binding=1): texture or 1x1 white fallback
 *
 * Outputs to two attachments:
 *   location 0: vec4  color    (R8G8B8A8_SRGB)
 *   location 1: uint  object_id (R32_UINT)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_texcoord;

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
    float _pad1;
    float _pad2;
    Light lights[4];
} frag;

layout(set = 0, binding = 1) uniform sampler2D u_texture;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out uint frag_object_id;

void main() {
    vec3 n = normalize(v_normal);

    float lighting = frag.ambient;

    if (frag.num_lights > 0) {
        /* Multi-light accumulation */
        for (int i = 0; i < frag.num_lights; i++) {
            if (frag.lights[i].params.w < 0.5) continue; /* inactive */

            int light_type = int(frag.lights[i].position.w + 0.5);
            float ndotl = 0.0;
            float attenuation = 1.0;
            float spot_factor = 1.0;
            float intensity = frag.lights[i].color.w;

            if (light_type == 0) {
                /* Directional */
                vec3 dir = normalize(frag.lights[i].direction.xyz);
                ndotl = max(dot(n, dir), 0.0);
            } else if (light_type == 1) {
                /* Point */
                vec3 to_light = frag.lights[i].position.xyz; /* world pos — approx */
                float dist = length(to_light);
                vec3 dir = to_light / max(dist, 0.000001);
                ndotl = max(dot(n, dir), 0.0);

                float range = frag.lights[i].params.x;
                if (range > 0.0) {
                    float r = dist / range;
                    attenuation = max(1.0 - r, 0.0);
                    attenuation *= attenuation;
                } else {
                    attenuation = 1.0 / (1.0 + dist * dist);
                }
            } else {
                /* Spot */
                vec3 to_light = frag.lights[i].position.xyz;
                float dist = length(to_light);
                vec3 dir = to_light / max(dist, 0.000001);
                ndotl = max(dot(n, dir), 0.0);

                vec3 spot_dir = normalize(frag.lights[i].direction.xyz);
                float cos_angle = -dot(dir, spot_dir);
                float outer_cos = frag.lights[i].params.z;
                float inner_cos = frag.lights[i].params.y;

                if (cos_angle < outer_cos) {
                    spot_factor = 0.0;
                } else if (cos_angle < inner_cos) {
                    float range_val = inner_cos - outer_cos;
                    if (range_val > 0.000001) {
                        spot_factor = (cos_angle - outer_cos) / range_val;
                    }
                }

                float range = frag.lights[i].params.x;
                if (range > 0.0) {
                    float r = dist / range;
                    attenuation = max(1.0 - r, 0.0);
                    attenuation *= attenuation;
                } else {
                    attenuation = 1.0 / (1.0 + dist * dist);
                }
            }

            lighting += ndotl * intensity * attenuation * spot_factor;
        }
        lighting = clamp(lighting, 0.0, 1.0);
    } else {
        /* Legacy single-light fallback */
        vec3 l = normalize(frag.light_dir.xyz);
        float ndotl = max(dot(n, l), 0.0);
        lighting = clamp(frag.ambient + (1.0 - frag.ambient) * ndotl, 0.0, 1.0);
    }

    vec4 base = v_color;
    if (frag.has_texture != 0) {
        base *= texture(u_texture, v_texcoord);
    }

    vec3 lit = base.rgb * lighting;
    float alpha = base.a * frag.opacity;

    frag_color = vec4(lit, alpha);
    frag_object_id = frag.object_id;
}
