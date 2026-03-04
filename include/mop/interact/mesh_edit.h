/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * mesh_edit.h — Mesh editing operations (vertex/edge/face)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_INTERACT_MESH_EDIT_H
#define MOP_INTERACT_MESH_EDIT_H

#include <mop/types.h>

typedef struct MopMesh MopMesh;
typedef struct MopViewport MopViewport;

/* Vertex operations */
void mop_mesh_move_vertices(MopMesh *mesh, MopViewport *vp,
                            const uint32_t *indices, uint32_t count,
                            MopVec3 delta);
void mop_mesh_delete_vertices(MopMesh *mesh, MopViewport *vp,
                              const uint32_t *indices, uint32_t count);
void mop_mesh_merge_vertices(MopMesh *mesh, MopViewport *vp, uint32_t v0,
                             uint32_t v1);

/* Edge operations */
void mop_mesh_split_edge(MopMesh *mesh, MopViewport *vp, uint32_t edge_v0,
                         uint32_t edge_v1);
void mop_mesh_dissolve_edge(MopMesh *mesh, MopViewport *vp, uint32_t edge_v0,
                            uint32_t edge_v1);

/* Face operations */
void mop_mesh_extrude_faces(MopMesh *mesh, MopViewport *vp,
                            const uint32_t *face_indices, uint32_t count,
                            float distance);
void mop_mesh_inset_faces(MopMesh *mesh, MopViewport *vp,
                          const uint32_t *face_indices, uint32_t count,
                          float inset);
void mop_mesh_delete_faces(MopMesh *mesh, MopViewport *vp,
                           const uint32_t *face_indices, uint32_t count);
void mop_mesh_flip_normals(MopMesh *mesh, MopViewport *vp,
                           const uint32_t *face_indices, uint32_t count);

#endif /* MOP_INTERACT_MESH_EDIT_H */
