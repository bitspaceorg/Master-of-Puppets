/*
 * Master of Puppets — Built-in Overlay Implementations
 * overlay_builtin.c — Wireframe-on-shaded, normals, bounds, selection
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "viewport/viewport_internal.h"
#include "rhi/rhi.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * RHI buffer data accessor
 *
 * The overlay code needs to read vertex data from RHI buffers.  Since
 * MopRhiBuffer is opaque per-backend, we call through the backend's
 * buffer_read function pointer.  CPU returns buf->data, Vulkan returns
 * buf->shadow.
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Wireframe-on-shaded overlay
 *
 * For each active scene mesh, re-issue the draw call with wireframe=true,
 * using the overlay wireframe color and reduced opacity (alpha blend).
 * The wireframe is drawn on top of the shaded surface.
 * ------------------------------------------------------------------------- */

void mop_overlay_builtin_wireframe(MopViewport *vp, void *user_data) {
    (void)user_data;
    if (!vp) return;

    MopColor wf_color   = vp->display.wireframe_color;
    float    wf_opacity = vp->display.wireframe_opacity;

    for (uint32_t i = 0; i < vp->mesh_count; i++) {
        struct MopMesh *m = &vp->meshes[i];
        if (!m->active) continue;
        if (m->object_id == 0) continue;            /* skip grid/bg */
        if (m->object_id >= 0xFFFF0000u) continue;  /* skip gizmo */

        MopMat4 mvp = mop_mat4_multiply(
            vp->projection_matrix,
            mop_mat4_multiply(vp->view_matrix, m->world_transform)
        );

        MopRhiDrawCall call = {
            .vertex_buffer = m->vertex_buffer,
            .index_buffer  = m->index_buffer,
            .vertex_count  = m->vertex_count,
            .index_count   = m->index_count,
            .object_id     = 0,              /* don't write to pick buffer */
            .model         = m->world_transform,
            .view          = vp->view_matrix,
            .projection    = vp->projection_matrix,
            .mvp           = mvp,
            .base_color    = wf_color,
            .opacity       = wf_opacity,
            .light_dir     = vp->light_dir,
            .ambient       = 1.0f,           /* unlit wireframe */
            .shading_mode  = MOP_SHADING_FLAT,
            .wireframe     = true,
            .depth_test    = true,
            .backface_cull = false,
            .texture       = NULL,
            .blend_mode    = MOP_BLEND_ALPHA,
            .metallic      = 0.0f,
            .roughness     = 0.5f,
            .emissive      = (MopVec3){0,0,0},
            .lights        = NULL,
            .light_count   = 0,
            .vertex_format = NULL,
        };
        vp->rhi->draw(vp->device, vp->framebuffer, &call);
    }
}

/* -------------------------------------------------------------------------
 * Vertex normals overlay
 *
 * For each active scene mesh, read vertex positions and normals from the
 * buffer, generate line geometry (position -> position + normal * length),
 * and draw as wireframe lines colored by normal direction (RGB = XYZ).
 * ------------------------------------------------------------------------- */

void mop_overlay_builtin_normals(MopViewport *vp, void *user_data) {
    (void)user_data;
    if (!vp) return;

    float length = vp->display.normal_display_length;

    for (uint32_t mi = 0; mi < vp->mesh_count; mi++) {
        struct MopMesh *m = &vp->meshes[mi];
        if (!m->active) continue;
        if (m->object_id == 0) continue;
        if (m->object_id >= 0xFFFF0000u) continue;

        const MopVertex *verts = (const MopVertex *)vp->rhi->buffer_read(m->vertex_buffer);
        uint32_t vc = m->vertex_count;
        if (vc == 0) continue;

        /* Generate line geometry: 2 vertices per normal line, 2 indices */
        uint32_t line_vc = vc * 2;
        uint32_t line_ic = vc * 2;
        MopVertex *line_v = malloc(line_vc * sizeof(MopVertex));
        uint32_t  *line_i = malloc(line_ic * sizeof(uint32_t));
        if (!line_v || !line_i) { free(line_v); free(line_i); continue; }

        for (uint32_t j = 0; j < vc; j++) {
            MopVec3 p = verts[j].position;
            MopVec3 n = verts[j].normal;
            /* Color = normal direction mapped to [0,1] */
            MopColor nc = {
                fabsf(n.x), fabsf(n.y), fabsf(n.z), 1.0f
            };
            MopVec3 tip = {
                p.x + n.x * length,
                p.y + n.y * length,
                p.z + n.z * length
            };
            line_v[j * 2 + 0] = (MopVertex){ p, n, nc, 0, 0 };
            line_v[j * 2 + 1] = (MopVertex){ tip, n, nc, 0, 0 };
            line_i[j * 2 + 0] = j * 2;
            line_i[j * 2 + 1] = j * 2 + 1;
        }

        /* Create temp buffers */
        MopRhiBufferDesc vb_desc = {
            .data = line_v, .size = line_vc * sizeof(MopVertex)
        };
        MopRhiBufferDesc ib_desc = {
            .data = line_i, .size = line_ic * sizeof(uint32_t)
        };
        MopRhiBuffer *vb = vp->rhi->buffer_create(vp->device, &vb_desc);
        MopRhiBuffer *ib = vp->rhi->buffer_create(vp->device, &ib_desc);
        free(line_v);
        free(line_i);

        if (!vb || !ib) {
            if (vb) vp->rhi->buffer_destroy(vp->device, vb);
            if (ib) vp->rhi->buffer_destroy(vp->device, ib);
            continue;
        }

        MopMat4 mvp = mop_mat4_multiply(
            vp->projection_matrix,
            mop_mat4_multiply(vp->view_matrix, m->world_transform)
        );

        MopRhiDrawCall call = {
            .vertex_buffer = vb,
            .index_buffer  = ib,
            .vertex_count  = line_vc,
            .index_count   = line_ic,
            .object_id     = 0,
            .model         = m->world_transform,
            .view          = vp->view_matrix,
            .projection    = vp->projection_matrix,
            .mvp           = mvp,
            .base_color    = (MopColor){1,1,1,1},
            .opacity       = 1.0f,
            .light_dir     = vp->light_dir,
            .ambient       = 1.0f,
            .shading_mode  = MOP_SHADING_FLAT,
            .wireframe     = true,
            .depth_test    = true,
            .backface_cull = false,
            .texture       = NULL,
            .blend_mode    = MOP_BLEND_OPAQUE,
            .metallic      = 0.0f,
            .roughness     = 0.5f,
            .emissive      = (MopVec3){0,0,0},
            .lights        = NULL,
            .light_count   = 0,
            .vertex_format = NULL,
        };
        vp->rhi->draw(vp->device, vp->framebuffer, &call);

        vp->rhi->buffer_destroy(vp->device, vb);
        vp->rhi->buffer_destroy(vp->device, ib);
    }
}

/* -------------------------------------------------------------------------
 * Bounding box overlay
 *
 * For each active scene mesh, compute the AABB from the vertex buffer
 * (in local space), transform to world, and draw a 12-line wireframe box.
 * ------------------------------------------------------------------------- */

void mop_overlay_builtin_bounds(MopViewport *vp, void *user_data) {
    (void)user_data;
    if (!vp) return;

    for (uint32_t mi = 0; mi < vp->mesh_count; mi++) {
        struct MopMesh *m = &vp->meshes[mi];
        if (!m->active) continue;
        if (m->object_id == 0) continue;
        if (m->object_id >= 0xFFFF0000u) continue;

        const MopVertex *verts = (const MopVertex *)vp->rhi->buffer_read(m->vertex_buffer);
        uint32_t vc = m->vertex_count;
        if (vc == 0) continue;

        /* Compute local-space AABB */
        MopVec3 bmin = verts[0].position;
        MopVec3 bmax = verts[0].position;
        for (uint32_t j = 1; j < vc; j++) {
            MopVec3 p = verts[j].position;
            if (p.x < bmin.x) bmin.x = p.x;
            if (p.y < bmin.y) bmin.y = p.y;
            if (p.z < bmin.z) bmin.z = p.z;
            if (p.x > bmax.x) bmax.x = p.x;
            if (p.y > bmax.y) bmax.y = p.y;
            if (p.z > bmax.z) bmax.z = p.z;
        }

        /* 8 corners of AABB */
        MopVec3 corners[8] = {
            { bmin.x, bmin.y, bmin.z },
            { bmax.x, bmin.y, bmin.z },
            { bmax.x, bmax.y, bmin.z },
            { bmin.x, bmax.y, bmin.z },
            { bmin.x, bmin.y, bmax.z },
            { bmax.x, bmin.y, bmax.z },
            { bmax.x, bmax.y, bmax.z },
            { bmin.x, bmax.y, bmax.z },
        };

        /* 12 edges as index pairs */
        static const uint32_t edges[24] = {
            0,1, 1,2, 2,3, 3,0,   /* bottom face */
            4,5, 5,6, 6,7, 7,4,   /* top face */
            0,4, 1,5, 2,6, 3,7,   /* verticals */
        };

        MopColor box_color = { 0.8f, 0.8f, 0.2f, 1.0f };
        MopVec3 n_up = { 0, 1, 0 };

        MopVertex box_v[8];
        for (int j = 0; j < 8; j++) {
            box_v[j] = (MopVertex){ corners[j], n_up, box_color, 0, 0 };
        }

        MopRhiBufferDesc vb_desc = {
            .data = box_v, .size = sizeof(box_v)
        };
        MopRhiBufferDesc ib_desc = {
            .data = edges, .size = sizeof(edges)
        };
        MopRhiBuffer *vb = vp->rhi->buffer_create(vp->device, &vb_desc);
        MopRhiBuffer *ib = vp->rhi->buffer_create(vp->device, &ib_desc);
        if (!vb || !ib) {
            if (vb) vp->rhi->buffer_destroy(vp->device, vb);
            if (ib) vp->rhi->buffer_destroy(vp->device, ib);
            continue;
        }

        MopMat4 mvp = mop_mat4_multiply(
            vp->projection_matrix,
            mop_mat4_multiply(vp->view_matrix, m->world_transform)
        );

        MopRhiDrawCall call = {
            .vertex_buffer = vb,
            .index_buffer  = ib,
            .vertex_count  = 8,
            .index_count   = 24,
            .object_id     = 0,
            .model         = m->world_transform,
            .view          = vp->view_matrix,
            .projection    = vp->projection_matrix,
            .mvp           = mvp,
            .base_color    = box_color,
            .opacity       = 1.0f,
            .light_dir     = vp->light_dir,
            .ambient       = 1.0f,
            .shading_mode  = MOP_SHADING_FLAT,
            .wireframe     = true,
            .depth_test    = true,
            .backface_cull = false,
            .texture       = NULL,
            .blend_mode    = MOP_BLEND_OPAQUE,
            .metallic      = 0.0f,
            .roughness     = 0.5f,
            .emissive      = (MopVec3){0,0,0},
            .lights        = NULL,
            .light_count   = 0,
            .vertex_format = NULL,
        };
        vp->rhi->draw(vp->device, vp->framebuffer, &call);

        vp->rhi->buffer_destroy(vp->device, vb);
        vp->rhi->buffer_destroy(vp->device, ib);
    }
}

/* -------------------------------------------------------------------------
 * Selection highlight overlay
 *
 * If a mesh is selected (viewport->selected_id matches mesh->object_id),
 * redraw it with additive blend at a highlight color.
 * ------------------------------------------------------------------------- */

void mop_overlay_builtin_selection(MopViewport *vp, void *user_data) {
    (void)user_data;
    if (!vp || vp->selected_id == 0) return;

    for (uint32_t i = 0; i < vp->mesh_count; i++) {
        struct MopMesh *m = &vp->meshes[i];
        if (!m->active) continue;
        if (m->object_id != vp->selected_id) continue;

        MopMat4 mvp = mop_mat4_multiply(
            vp->projection_matrix,
            mop_mat4_multiply(vp->view_matrix, m->world_transform)
        );

        MopRhiDrawCall call = {
            .vertex_buffer = m->vertex_buffer,
            .index_buffer  = m->index_buffer,
            .vertex_count  = m->vertex_count,
            .index_count   = m->index_count,
            .object_id     = 0,
            .model         = m->world_transform,
            .view          = vp->view_matrix,
            .projection    = vp->projection_matrix,
            .mvp           = mvp,
            .base_color    = (MopColor){ 0.2f, 0.4f, 1.0f, 1.0f },
            .opacity       = 0.12f,
            .light_dir     = vp->light_dir,
            .ambient       = 1.0f,
            .shading_mode  = MOP_SHADING_FLAT,
            .wireframe     = false,
            .depth_test    = true,
            .backface_cull = false,
            .texture       = NULL,
            .blend_mode    = MOP_BLEND_ADDITIVE,
            .metallic      = 0.0f,
            .roughness     = 0.5f,
            .emissive      = (MopVec3){0,0,0},
            .lights        = NULL,
            .light_count   = 0,
            .vertex_format = NULL,
        };
        vp->rhi->draw(vp->device, vp->framebuffer, &call);
    }
}
