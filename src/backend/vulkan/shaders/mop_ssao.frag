#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_ssao.frag — Screen-Space Ambient Occlusion
 *
 * Reconstructs view-space position from depth buffer, samples
 * a hemisphere kernel with random rotation from a 4x4 noise
 * texture, and outputs an AO factor in R8_UNORM.
 *
 * Binding 0: Depth buffer (D32_SFLOAT, sampled as R32_SFLOAT)
 * Binding 1: 4x4 noise texture (RG16_SFLOAT — random rotation vectors)
 * Binding 2: Kernel UBO (64 hemisphere sample positions)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_depth;
layout(set = 0, binding = 1) uniform sampler2D u_noise;

layout(set = 0, binding = 2) uniform KernelUBO {
    vec4 samples[64];
} kernel;

layout(push_constant) uniform PC {
    mat4 projection;
    float radius;
    float bias;
    int kernel_size;
    int reverse_z;
    vec2 noise_scale;  /* viewport_size / 4.0 for 4x4 noise tiling */
    vec2 _pad;
} pc;

layout(location = 0) out float frag_ao;

/* Reconstruct view-space position from depth + UV */
vec3 view_pos_from_depth(vec2 uv, float depth) {
    /* Linearize depth: map from [0,1] to NDC [-1,1] */
    float z_ndc = pc.reverse_z != 0 ? (1.0 - depth) * 2.0 - 1.0
                                     : depth * 2.0 - 1.0;

    /* Reconstruct clip-space position */
    vec4 clip = vec4(uv * 2.0 - 1.0, z_ndc, 1.0);

    /* Inverse projection */
    float a = pc.projection[0][0]; /* 1 / (aspect * tan(fov/2)) */
    float b = pc.projection[1][1]; /* 1 / tan(fov/2) */
    float c = pc.projection[2][2];
    float d = pc.projection[3][2];

    float view_z = d / (clip.z - c);
    vec3 view_pos;
    view_pos.x = clip.x * view_z / a;
    view_pos.y = clip.y * view_z / b;
    view_pos.z = view_z;
    return view_pos;
}

void main() {
    float depth = texture(u_depth, v_uv).r;

    /* Skip far plane */
    if (pc.reverse_z != 0) {
        if (depth < 0.0001) { frag_ao = 1.0; return; }
    } else {
        if (depth > 0.9999) { frag_ao = 1.0; return; }
    }

    vec3 frag_pos = view_pos_from_depth(v_uv, depth);

    /* Reconstruct normal from depth derivatives */
    vec3 dpdx = dFdx(frag_pos);
    vec3 dpdy = dFdy(frag_pos);
    vec3 normal = normalize(cross(dpdx, dpdy));

    /* Random rotation from noise texture */
    vec3 random_vec = vec3(texture(u_noise, v_uv * pc.noise_scale).rg, 0.0);

    /* Create TBN matrix to orient hemisphere along surface normal */
    vec3 tangent = normalize(random_vec - normal * dot(random_vec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    /* Accumulate occlusion */
    float occlusion = 0.0;
    int ks = min(pc.kernel_size, 64);
    for (int i = 0; i < ks; i++) {
        /* Get sample position in view space */
        vec3 sample_pos = TBN * kernel.samples[i].xyz;
        sample_pos = frag_pos + sample_pos * pc.radius;

        /* Project sample to screen space */
        vec4 offset = pc.projection * vec4(sample_pos, 1.0);
        offset.xy /= offset.w;
        offset.xy = offset.xy * 0.5 + 0.5;

        /* Sample depth at projected position */
        float sample_depth = texture(u_depth, offset.xy).r;
        vec3 sample_view = view_pos_from_depth(offset.xy, sample_depth);

        /* Range check and accumulate */
        float range_check = smoothstep(0.0, 1.0,
            pc.radius / abs(frag_pos.z - sample_view.z));
        occlusion += (sample_view.z >= sample_pos.z + pc.bias ? 1.0 : 0.0)
                     * range_check;
    }

    frag_ao = 1.0 - (occlusion / float(ks));
}
