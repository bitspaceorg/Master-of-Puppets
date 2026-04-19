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

/* -------------------------------------------------------------------------
 * Vertex operations
 * ------------------------------------------------------------------------- */

/* Translate selected vertices by `delta` in world space.
 * Recomputes normals and uploads updated geometry. */
void mop_mesh_move_vertices(MopMesh *mesh, MopViewport *vp,
                            const uint32_t *indices, uint32_t count,
                            MopVec3 delta);

/* Remove vertices and all faces referencing them. */
void mop_mesh_delete_vertices(MopMesh *mesh, MopViewport *vp,
                              const uint32_t *indices, uint32_t count);

/* Weld v1 into v0 — faces referencing v1 are remapped to v0. */
void mop_mesh_merge_vertices(MopMesh *mesh, MopViewport *vp, uint32_t v0,
                             uint32_t v1);

/* -------------------------------------------------------------------------
 * Edge operations
 * ------------------------------------------------------------------------- */

/* Insert a midpoint vertex on the edge (v0,v1), splitting adjacent faces. */
void mop_mesh_split_edge(MopMesh *mesh, MopViewport *vp, uint32_t edge_v0,
                         uint32_t edge_v1);

/* Remove the edge (v0,v1) and merge adjacent faces. */
void mop_mesh_dissolve_edge(MopMesh *mesh, MopViewport *vp, uint32_t edge_v0,
                            uint32_t edge_v1);

/* -------------------------------------------------------------------------
 * Face operations
 * ------------------------------------------------------------------------- */

/* Extrude selected faces outward by `distance` along face normals. */
void mop_mesh_extrude_faces(MopMesh *mesh, MopViewport *vp,
                            const uint32_t *face_indices, uint32_t count,
                            float distance);

/* Inset faces — shrink each face by `inset` and create surrounding quads. */
void mop_mesh_inset_faces(MopMesh *mesh, MopViewport *vp,
                          const uint32_t *face_indices, uint32_t count,
                          float inset);

/* Delete faces (leaves vertices intact). */
void mop_mesh_delete_faces(MopMesh *mesh, MopViewport *vp,
                           const uint32_t *face_indices, uint32_t count);

/* Reverse winding order of selected faces, flipping their normals. */
void mop_mesh_flip_normals(MopMesh *mesh, MopViewport *vp,
                           const uint32_t *face_indices, uint32_t count);

#endif /* MOP_INTERACT_MESH_EDIT_H */
