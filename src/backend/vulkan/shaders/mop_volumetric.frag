#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_volumetric.frag — Fullscreen raymarched volumetric fog
 *
 * Raymarches from camera through scene, accumulating in-scattered light
 * from all active lights and computing transmittance.
 *
 * Output: vec4(in_scattered_rgb, transmittance)
 * Hardware blending: srcColor*ONE + dstColor*srcAlpha
 * Result: scene_color * transmittance + in_scattered_light
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) out vec4 out_color;

/* Depth buffer for world position reconstruction */
layout(set = 0, binding = 0) uniform sampler2D depth_tex;

/* Light SSBO — same layout as mop_solid.frag */
struct Light {
  vec4 position;  /* xyz + type (0=dir, 1=point, 2=spot) */
  vec4 direction; /* xyz + padding */
  vec4 color;     /* rgb + intensity */
  vec4 params;    /* range, spot_inner_cos, spot_outer_cos, active */
};

layout(std430, set = 0, binding = 1) readonly buffer LightSSBO {
  Light lights[];
};

/* Per-frame volumetric parameters */
layout(std140, set = 0, binding = 2) uniform VolumetricUBO {
  mat4 inv_vp;
  vec4 cam_pos;    /* xyz + pad */
  vec4 fog_params; /* rgb = scattering_color, a = density */
  float anisotropy;  /* Henyey-Greenstein g parameter [-1,1] */
  int num_lights;
  int num_steps;     /* raymarch steps (16-64) */
  int reverse_z;
};

/* Henyey-Greenstein phase function — describes how light scatters
 * in a participating medium.  g=0: isotropic, g>0: forward-scatter,
 * g<0: back-scatter. */
float hg_phase(float cos_theta, float g) {
  float g2 = g * g;
  float denom = 1.0 + g2 - 2.0 * g * cos_theta;
  return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 0.0001), 1.5));
}

void main() {
  vec2 tex_size = vec2(textureSize(depth_tex, 0));
  vec2 uv = gl_FragCoord.xy / tex_size;
  float depth = texture(depth_tex, uv).r;

  /* Skip background pixels (no geometry to fog towards) */
  bool is_bg = (reverse_z != 0) ? (depth < 0.0001) : (depth > 0.9999);
  if (is_bg) {
    out_color = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }

  /* Reconstruct world position from depth */
  vec2 ndc = uv * 2.0 - 1.0;
  vec4 clip = vec4(ndc, depth, 1.0);
  vec4 world = inv_vp * clip;
  vec3 world_pos = world.xyz / world.w;

  vec3 cam = cam_pos.xyz;
  vec3 ray = world_pos - cam;
  float ray_len = length(ray);
  vec3 ray_dir = ray / max(ray_len, 0.0001);

  float density = fog_params.a;
  vec3 scatter_color = fog_params.rgb;
  int steps = clamp(num_steps, 4, 64);
  float step_size = ray_len / float(steps);

  vec3 accumulated = vec3(0.0);
  float transmittance = 1.0;

  for (int i = 0; i < steps && transmittance > 0.001; i++) {
    float t = (float(i) + 0.5) / float(steps);
    vec3 pos = cam + ray * t;

    /* Evaluate lighting contribution from all active lights */
    vec3 lighting = vec3(0.0);
    int lc = min(num_lights, 4096);

    for (int l = 0; l < lc; l++) {
      Light light = lights[l];
      if (light.params.w < 0.5) continue;

      float light_type = light.position.w;
      vec3 contrib = vec3(0.0);

      if (light_type < 0.5) {
        /* Directional light */
        vec3 L = normalize(-light.direction.xyz);
        float cos_theta = dot(ray_dir, L);
        float phase = hg_phase(cos_theta, anisotropy);
        contrib = light.color.rgb * light.color.a * phase;
      } else {
        /* Point / spot light */
        vec3 to_light = light.position.xyz - pos;
        float dist2 = dot(to_light, to_light);
        float dist = sqrt(dist2);
        float range = light.params.x;

        if (range > 0.0 && dist > range) continue;

        vec3 L = to_light / max(dist, 0.001);
        float atten = 1.0 / max(dist2, 0.01);

        /* Spot cone attenuation */
        if (light_type > 1.5) {
          float cos_angle = dot(-L, normalize(light.direction.xyz));
          float inner = light.params.y;
          float outer = light.params.z;
          float spot = clamp((cos_angle - outer) / max(inner - outer, 0.001),
                             0.0, 1.0);
          atten *= spot * spot;
        }

        float cos_theta = dot(ray_dir, L);
        float phase = hg_phase(cos_theta, anisotropy);
        contrib = light.color.rgb * light.color.a * atten * phase;
      }

      lighting += contrib;
    }

    /* Accumulate scattering: Beer-Lambert extinction + in-scattering */
    float extinction = density * step_size;
    float step_trans = exp(-extinction);
    accumulated += transmittance * lighting * scatter_color * density * step_size;
    transmittance *= step_trans;
  }

  /* Output: RGB = in-scattered light, A = remaining transmittance.
   * Hardware blend: scene_color * transmittance + in_scattered */
  out_color = vec4(accumulated, transmittance);
}
