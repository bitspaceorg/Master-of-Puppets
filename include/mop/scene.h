/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * scene.h — Mesh management within a viewport
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_SCENE_H
#define MOP_SCENE_H

#include "types.h"

/* Forward declaration — defined in viewport.h */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Opaque mesh handle
 * ------------------------------------------------------------------------- */

typedef struct MopMesh MopMesh;

/* -------------------------------------------------------------------------
 * Mesh descriptor
 *
 * vertices     : array of MopVertex, owned by application
 * vertex_count : number of vertices
 * indices      : array of uint32_t triangle indices, owned by application
 * index_count  : number of indices (must be a multiple of 3)
 * object_id    : unique identifier for picking (0 = no object)
 *
 * The engine copies vertex and index data during mop_viewport_add_mesh.
 * The application may free its arrays after the call returns.
 * ------------------------------------------------------------------------- */

typedef struct MopMeshDesc {
    const MopVertex *vertices;
    uint32_t         vertex_count;
    const uint32_t  *indices;
    uint32_t         index_count;
    uint32_t         object_id;
} MopMeshDesc;

/* -------------------------------------------------------------------------
 * Mesh management
 *
 * All mesh functions require a valid, non-NULL viewport.
 * Meshes are owned by their viewport and destroyed when the viewport is
 * destroyed.  Explicit removal is optional.
 * ------------------------------------------------------------------------- */

/* Add a mesh to the viewport.  Returns NULL on failure. */
MopMesh *mop_viewport_add_mesh(MopViewport *viewport,
                               const MopMeshDesc *desc);

/* Remove a mesh from the viewport and free its resources. */
void mop_viewport_remove_mesh(MopViewport *viewport, MopMesh *mesh);

/* Set the model transform for a mesh.  Identity by default. */
void mop_mesh_set_transform(MopMesh *mesh, const MopMat4 *transform);

#endif /* MOP_SCENE_H */
