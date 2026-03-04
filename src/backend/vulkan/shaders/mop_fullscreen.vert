#version 450

/*
 * Master of Puppets — Vulkan Backend
 * mop_fullscreen.vert — Fullscreen triangle (no vertex buffer needed)
 *
 * Generates a single triangle that covers the entire viewport.
 * Draw with 3 vertices and no vertex buffer bound.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
    v_uv.y = 1.0 - v_uv.y; /* flip Y for Vulkan */
}
