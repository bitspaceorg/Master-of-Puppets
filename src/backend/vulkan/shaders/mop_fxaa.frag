#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_fxaa.frag — Simplified FXAA 3.11 post-processing filter
 *
 * Input:  sampler2D u_color (scene color from main render pass)
 * Push constant: vec2 inv_resolution (1/width, 1/height)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(set = 0, binding = 0) uniform sampler2D u_color;

layout(push_constant) uniform PC { vec2 inv_resolution; } pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

float luminance(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec3 rgbM  = texture(u_color, v_uv).rgb;
    vec3 rgbNW = texture(u_color, v_uv + vec2(-1.0, -1.0) * pc.inv_resolution).rgb;
    vec3 rgbNE = texture(u_color, v_uv + vec2( 1.0, -1.0) * pc.inv_resolution).rgb;
    vec3 rgbSW = texture(u_color, v_uv + vec2(-1.0,  1.0) * pc.inv_resolution).rgb;
    vec3 rgbSE = texture(u_color, v_uv + vec2( 1.0,  1.0) * pc.inv_resolution).rgb;

    float lumM  = luminance(rgbM);
    float lumNW = luminance(rgbNW);
    float lumNE = luminance(rgbNE);
    float lumSW = luminance(rgbSW);
    float lumSE = luminance(rgbSE);

    float lumMin = min(lumM, min(min(lumNW, lumNE), min(lumSW, lumSE)));
    float lumMax = max(lumM, max(max(lumNW, lumNE), max(lumSW, lumSE)));
    float lumRange = lumMax - lumMin;

    if (lumRange < max(0.0312, lumMax * 0.125)) {
        frag_color = vec4(rgbM, 1.0);
        return;
    }

    vec2 dir;
    dir.x = -((lumNW + lumNE) - (lumSW + lumSE));
    dir.y =  ((lumNW + lumSW) - (lumNE + lumSE));
    float dirReduce = max((lumNW + lumNE + lumSW + lumSE) * 0.25 * 0.25, 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * pc.inv_resolution;

    vec3 rgbA = 0.5 * (texture(u_color, v_uv + dir * (1.0/3.0 - 0.5)).rgb +
                        texture(u_color, v_uv + dir * (2.0/3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(u_color, v_uv + dir * -0.5).rgb +
                                       texture(u_color, v_uv + dir *  0.5).rgb);

    float lumB = luminance(rgbB);
    if (lumB < lumMin || lumB > lumMax) {
        frag_color = vec4(rgbA, 1.0);
    } else {
        frag_color = vec4(rgbB, 1.0);
    }
}
