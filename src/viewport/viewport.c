/*
 * Master of Puppets — Viewport Core
 * viewport.c — Viewport lifecycle, scene management, rendering orchestration
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "viewport/viewport_internal.h"
#include "rhi/rhi.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Viewport lifecycle
 * ------------------------------------------------------------------------- */

MopViewport *mop_viewport_create(const MopViewportDesc *desc) {
    if (!desc || desc->width <= 0 || desc->height <= 0) {
        return NULL;
    }

    const MopRhiBackend *rhi = mop_rhi_get_backend(desc->backend);
    if (!rhi) {
        return NULL;
    }

    MopRhiDevice *device = rhi->device_create();
    if (!device) {
        return NULL;
    }

    MopRhiFramebufferDesc fb_desc = {
        .width  = desc->width,
        .height = desc->height
    };
    MopRhiFramebuffer *fb = rhi->framebuffer_create(device, &fb_desc);
    if (!fb) {
        rhi->device_destroy(device);
        return NULL;
    }

    MopViewport *vp = calloc(1, sizeof(MopViewport));
    if (!vp) {
        rhi->framebuffer_destroy(device, fb);
        rhi->device_destroy(device);
        return NULL;
    }

    vp->rhi          = rhi;
    vp->device       = device;
    vp->framebuffer  = fb;
    vp->backend_type = (desc->backend == MOP_BACKEND_AUTO)
                        ? mop_backend_default()
                        : desc->backend;
    vp->width        = desc->width;
    vp->height       = desc->height;
    vp->clear_color  = (MopColor){ 0.1f, 0.1f, 0.1f, 1.0f };
    vp->render_mode  = MOP_RENDER_SOLID;
    vp->mesh_count   = 0;

    /* Default camera: looking at origin from Z+ */
    vp->cam_eye         = (MopVec3){ 0.0f, 0.0f, 5.0f };
    vp->cam_target      = (MopVec3){ 0.0f, 0.0f, 0.0f };
    vp->cam_up          = (MopVec3){ 0.0f, 1.0f, 0.0f };
    vp->cam_fov_radians = 45.0f * (3.14159265358979323846f / 180.0f);
    vp->cam_near        = 0.1f;
    vp->cam_far         = 100.0f;

    /* Compute initial matrices */
    float aspect = (float)vp->width / (float)vp->height;
    vp->view_matrix       = mop_mat4_look_at(vp->cam_eye, vp->cam_target,
                                              vp->cam_up);
    vp->projection_matrix = mop_mat4_perspective(vp->cam_fov_radians, aspect,
                                                  vp->cam_near, vp->cam_far);

    return vp;
}

void mop_viewport_destroy(MopViewport *viewport) {
    if (!viewport) {
        return;
    }

    /* Destroy all active mesh buffers */
    for (uint32_t i = 0; i < viewport->mesh_count; i++) {
        struct MopMesh *mesh = &viewport->meshes[i];
        if (mesh->active) {
            if (mesh->vertex_buffer) {
                viewport->rhi->buffer_destroy(viewport->device,
                                              mesh->vertex_buffer);
            }
            if (mesh->index_buffer) {
                viewport->rhi->buffer_destroy(viewport->device,
                                              mesh->index_buffer);
            }
            mesh->active = false;
        }
    }

    if (viewport->framebuffer) {
        viewport->rhi->framebuffer_destroy(viewport->device,
                                           viewport->framebuffer);
    }
    if (viewport->device) {
        viewport->rhi->device_destroy(viewport->device);
    }

    free(viewport);
}

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

void mop_viewport_resize(MopViewport *viewport, int width, int height) {
    if (!viewport || width <= 0 || height <= 0) {
        return;
    }
    viewport->width  = width;
    viewport->height = height;

    viewport->rhi->framebuffer_resize(viewport->device, viewport->framebuffer,
                                      width, height);

    /* Recompute projection matrix */
    float aspect = (float)width / (float)height;
    viewport->projection_matrix = mop_mat4_perspective(
        viewport->cam_fov_radians, aspect, viewport->cam_near, viewport->cam_far
    );
}

void mop_viewport_set_clear_color(MopViewport *viewport, MopColor color) {
    if (!viewport) return;
    viewport->clear_color = color;
}

void mop_viewport_set_render_mode(MopViewport *viewport, MopRenderMode mode) {
    if (!viewport) return;
    viewport->render_mode = mode;
}

void mop_viewport_set_camera(MopViewport *viewport,
                             MopVec3 eye, MopVec3 target, MopVec3 up,
                             float fov_degrees,
                             float near_plane, float far_plane) {
    if (!viewport) return;

    viewport->cam_eye         = eye;
    viewport->cam_target      = target;
    viewport->cam_up          = up;
    viewport->cam_fov_radians = fov_degrees * (3.14159265358979323846f / 180.0f);
    viewport->cam_near        = near_plane;
    viewport->cam_far         = far_plane;

    viewport->view_matrix = mop_mat4_look_at(eye, target, up);

    float aspect = (float)viewport->width / (float)viewport->height;
    viewport->projection_matrix = mop_mat4_perspective(
        viewport->cam_fov_radians, aspect, near_plane, far_plane
    );
}

MopBackendType mop_viewport_get_backend(MopViewport *viewport) {
    if (!viewport) return MOP_BACKEND_CPU;
    return viewport->backend_type;
}

/* -------------------------------------------------------------------------
 * Scene management
 * ------------------------------------------------------------------------- */

MopMesh *mop_viewport_add_mesh(MopViewport *viewport,
                               const MopMeshDesc *desc) {
    if (!viewport || !desc || !desc->vertices || !desc->indices) {
        return NULL;
    }
    if (desc->vertex_count == 0 || desc->index_count == 0) {
        return NULL;
    }
    if (desc->index_count % 3 != 0) {
        return NULL;
    }
    if (viewport->mesh_count >= MOP_MAX_MESHES) {
        return NULL;
    }

    /* Find a free slot (reuse inactive entries) */
    uint32_t slot = viewport->mesh_count;
    for (uint32_t i = 0; i < viewport->mesh_count; i++) {
        if (!viewport->meshes[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == viewport->mesh_count) {
        viewport->mesh_count++;
    }

    /* Create RHI buffers */
    MopRhiBufferDesc vb_desc = {
        .data = desc->vertices,
        .size = desc->vertex_count * sizeof(MopVertex)
    };
    MopRhiBuffer *vb = viewport->rhi->buffer_create(viewport->device, &vb_desc);
    if (!vb) {
        return NULL;
    }

    MopRhiBufferDesc ib_desc = {
        .data = desc->indices,
        .size = desc->index_count * sizeof(uint32_t)
    };
    MopRhiBuffer *ib = viewport->rhi->buffer_create(viewport->device, &ib_desc);
    if (!ib) {
        viewport->rhi->buffer_destroy(viewport->device, vb);
        return NULL;
    }

    /* Compute an average base color from vertex colors */
    MopColor avg = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (uint32_t i = 0; i < desc->vertex_count; i++) {
        avg.r += desc->vertices[i].color.r;
        avg.g += desc->vertices[i].color.g;
        avg.b += desc->vertices[i].color.b;
    }
    float inv = 1.0f / (float)desc->vertex_count;
    avg.r *= inv;
    avg.g *= inv;
    avg.b *= inv;

    struct MopMesh *mesh = &viewport->meshes[slot];
    mesh->vertex_buffer = vb;
    mesh->index_buffer  = ib;
    mesh->vertex_count  = desc->vertex_count;
    mesh->index_count   = desc->index_count;
    mesh->object_id     = desc->object_id;
    mesh->transform     = mop_mat4_identity();
    mesh->base_color    = avg;
    mesh->active        = true;

    return mesh;
}

void mop_viewport_remove_mesh(MopViewport *viewport, MopMesh *mesh) {
    if (!viewport || !mesh) return;

    if (mesh->vertex_buffer) {
        viewport->rhi->buffer_destroy(viewport->device, mesh->vertex_buffer);
        mesh->vertex_buffer = NULL;
    }
    if (mesh->index_buffer) {
        viewport->rhi->buffer_destroy(viewport->device, mesh->index_buffer);
        mesh->index_buffer = NULL;
    }
    mesh->active = false;
}

void mop_mesh_set_transform(MopMesh *mesh, const MopMat4 *transform) {
    if (!mesh || !transform) return;
    mesh->transform = *transform;
}

/* -------------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------------- */

void mop_viewport_render(MopViewport *viewport) {
    if (!viewport) return;

    viewport->rhi->frame_begin(viewport->device, viewport->framebuffer,
                               viewport->clear_color);

    for (uint32_t i = 0; i < viewport->mesh_count; i++) {
        struct MopMesh *mesh = &viewport->meshes[i];
        if (!mesh->active) continue;

        MopMat4 mvp = mop_mat4_multiply(
            viewport->projection_matrix,
            mop_mat4_multiply(viewport->view_matrix, mesh->transform)
        );

        MopRhiDrawCall call = {
            .vertex_buffer = mesh->vertex_buffer,
            .index_buffer  = mesh->index_buffer,
            .vertex_count  = mesh->vertex_count,
            .index_count   = mesh->index_count,
            .object_id     = mesh->object_id,
            .model         = mesh->transform,
            .view          = viewport->view_matrix,
            .projection    = viewport->projection_matrix,
            .mvp           = mvp,
            .base_color    = mesh->base_color,
            .wireframe     = (viewport->render_mode == MOP_RENDER_WIREFRAME),
            .depth_test    = true,
            .backface_cull = true
        };

        viewport->rhi->draw(viewport->device, viewport->framebuffer, &call);
    }

    viewport->rhi->frame_end(viewport->device, viewport->framebuffer);
}

/* -------------------------------------------------------------------------
 * Framebuffer readback
 * ------------------------------------------------------------------------- */

const uint8_t *mop_viewport_read_color(MopViewport *viewport,
                                       int *out_width, int *out_height) {
    if (!viewport) return NULL;
    return viewport->rhi->framebuffer_read_color(
        viewport->device, viewport->framebuffer, out_width, out_height
    );
}

/* -------------------------------------------------------------------------
 * Picking
 * ------------------------------------------------------------------------- */

MopPickResult mop_viewport_pick(MopViewport *viewport, int x, int y) {
    MopPickResult result = { .hit = false, .object_id = 0, .depth = 1.0f };

    if (!viewport) return result;
    if (x < 0 || x >= viewport->width || y < 0 || y >= viewport->height) {
        return result;
    }

    uint32_t id = viewport->rhi->pick_read_id(
        viewport->device, viewport->framebuffer, x, y
    );

    if (id != 0) {
        result.hit       = true;
        result.object_id = id;
        result.depth     = viewport->rhi->pick_read_depth(
            viewport->device, viewport->framebuffer, x, y
        );
    }

    return result;
}
