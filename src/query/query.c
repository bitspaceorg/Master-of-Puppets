/*
 * Master of Puppets — Scene Query API
 * query.c — Mesh enumeration, introspection, and light access
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "viewport/viewport_internal.h"

/* -------------------------------------------------------------------------
 * Internal filter: a mesh is a "scene mesh" if it's active, has a non-zero
 * object_id, and is not a gizmo handle (object_id < 0xFFFF0000).
 * This matches the filter used by pass_scene_opaque/pass_scene_transparent.
 * ------------------------------------------------------------------------- */

static bool is_scene_mesh(const struct MopMesh *m) {
    return m->active && m->object_id != 0 && m->object_id < 0xFFFF0000u;
}

/* -------------------------------------------------------------------------
 * Mesh enumeration
 * ------------------------------------------------------------------------- */

uint32_t mop_viewport_mesh_count(const MopViewport *vp) {
    if (!vp) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < vp->mesh_count; i++) {
        if (is_scene_mesh(&vp->meshes[i])) count++;
    }
    return count;
}

MopMesh *mop_viewport_mesh_at(const MopViewport *vp, uint32_t index) {
    if (!vp) return NULL;
    uint32_t active_idx = 0;
    for (uint32_t i = 0; i < vp->mesh_count; i++) {
        if (is_scene_mesh(&vp->meshes[i])) {
            if (active_idx == index) return &vp->meshes[i];
            active_idx++;
        }
    }
    return NULL;
}

MopMesh *mop_viewport_mesh_by_id(const MopViewport *vp, uint32_t object_id) {
    if (!vp || object_id == 0) return NULL;
    for (uint32_t i = 0; i < vp->mesh_count; i++) {
        if (vp->meshes[i].active && vp->meshes[i].object_id == object_id) {
            return &vp->meshes[i];
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Mesh introspection
 * ------------------------------------------------------------------------- */

uint32_t mop_mesh_get_object_id(const MopMesh *mesh) {
    return mesh ? mesh->object_id : 0;
}

bool mop_mesh_is_active(const MopMesh *mesh) {
    return mesh ? mesh->active : false;
}

uint32_t mop_mesh_get_vertex_count(const MopMesh *mesh) {
    return mesh ? mesh->vertex_count : 0;
}

uint32_t mop_mesh_get_index_count(const MopMesh *mesh) {
    return mesh ? mesh->index_count : 0;
}

uint32_t mop_mesh_get_triangle_count(const MopMesh *mesh) {
    return mesh ? mesh->index_count / 3 : 0;
}

const MopVertex *mop_mesh_get_vertices(const MopMesh *mesh,
                                        const MopViewport *vp) {
    if (!mesh || !vp || !mesh->vertex_buffer) return NULL;
    if (mesh->vertex_format) return NULL;  /* flex format — use _raw */
    return (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
}

const uint32_t *mop_mesh_get_indices(const MopMesh *mesh,
                                      const MopViewport *vp) {
    if (!mesh || !vp || !mesh->index_buffer) return NULL;
    return (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
}

const void *mop_mesh_get_vertex_data_raw(const MopMesh *mesh,
                                          const MopViewport *vp,
                                          uint32_t *out_stride) {
    if (!mesh || !vp || !mesh->vertex_buffer || !mesh->vertex_format)
        return NULL;
    if (out_stride) *out_stride = mesh->vertex_format->stride;
    return vp->rhi->buffer_read(mesh->vertex_buffer);
}

const MopVertexFormat *mop_mesh_get_vertex_format(const MopMesh *mesh) {
    return mesh ? mesh->vertex_format : NULL;
}

MopMat4 mop_mesh_get_local_transform(const MopMesh *mesh) {
    return mesh ? mesh->transform : mop_mat4_identity();
}

MopMat4 mop_mesh_get_world_transform(const MopMesh *mesh) {
    return mesh ? mesh->world_transform : mop_mat4_identity();
}

MopMaterial mop_mesh_get_material(const MopMesh *mesh) {
    if (!mesh) return mop_material_default();
    return mesh->has_material ? mesh->material : mop_material_default();
}

bool mop_mesh_has_material(const MopMesh *mesh) {
    return mesh ? mesh->has_material : false;
}

MopBlendMode mop_mesh_get_blend_mode(const MopMesh *mesh) {
    return mesh ? mesh->blend_mode : MOP_BLEND_OPAQUE;
}

float mop_mesh_get_opacity(const MopMesh *mesh) {
    return mesh ? mesh->opacity : 1.0f;
}

/* -------------------------------------------------------------------------
 * Light enumeration
 * ------------------------------------------------------------------------- */

const MopLight *mop_viewport_light_at(const MopViewport *vp, uint32_t index) {
    if (!vp || index >= vp->light_count) return NULL;
    if (!vp->lights[index].active) return NULL;
    return &vp->lights[index];
}
