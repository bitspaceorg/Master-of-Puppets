/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * scene.h — Mesh management within a viewport
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_SCENE_H
#define MOP_SCENE_H

#include "types.h"
#include "vertex_format.h"

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

/* Add a mesh to the viewport.  Returns NULL on failure.
 *
 * NOTE: The returned MopMesh pointer may be invalidated if the internal
 * mesh array is reallocated (when adding more meshes beyond the current
 * capacity).  Do not cache this pointer across calls to mop_viewport_add_mesh.
 * Instead, re-query using mop_viewport_mesh_at or mop_viewport_mesh_by_id. */
MopMesh *mop_viewport_add_mesh(MopViewport *viewport,
                               const MopMeshDesc *desc);

/* -------------------------------------------------------------------------
 * Extended mesh descriptor — flexible vertex format
 *
 * Like MopMeshDesc but takes raw vertex bytes + a vertex format descriptor
 * instead of a MopVertex array.  The engine copies vertex and index data
 * during the call; the application may free its arrays afterward.
 * ------------------------------------------------------------------------- */

typedef struct MopMeshDescEx {
    const void            *vertex_data;     /* raw interleaved bytes */
    uint32_t               vertex_count;
    const uint32_t        *indices;
    uint32_t               index_count;
    uint32_t               object_id;
    const MopVertexFormat *vertex_format;   /* required */
} MopMeshDescEx;

/* Add a mesh with a flexible vertex format.  Returns NULL on failure. */
MopMesh *mop_viewport_add_mesh_ex(MopViewport *viewport,
                                   const MopMeshDescEx *desc);

/* Remove a mesh from the viewport and free its resources. */
void mop_viewport_remove_mesh(MopViewport *viewport, MopMesh *mesh);

/* Update a mesh's vertex and index data in-place.
 * If the new data fits within the existing buffers, this is a fast memcpy
 * with no allocation.  If the buffers need to grow, they are reallocated
 * (2x growth strategy).  Vertex and index counts may differ from the
 * original.  The application may free its arrays after the call returns. */
void mop_mesh_update_geometry(MopMesh *mesh, MopViewport *viewport,
                              const MopVertex *vertices, uint32_t vertex_count,
                              const uint32_t *indices, uint32_t index_count);

/* Set the model transform for a mesh.  Identity by default. */
void mop_mesh_set_transform(MopMesh *mesh, const MopMat4 *transform);

/* Set the opacity for a mesh.  1.0 = fully opaque (default), 0.0 = invisible. */
void mop_mesh_set_opacity(MopMesh *mesh, float opacity);

/* Set the blend mode for a mesh.  MOP_BLEND_OPAQUE by default. */
void mop_mesh_set_blend_mode(MopMesh *mesh, MopBlendMode mode);

/* -------------------------------------------------------------------------
 * Texture management
 *
 * Textures are owned by the viewport's RHI device.  Destroy them before
 * destroying the viewport.
 * ------------------------------------------------------------------------- */

typedef struct MopTexture MopTexture;

MopTexture *mop_viewport_create_texture(MopViewport *viewport, int width,
                                        int height, const uint8_t *rgba_data);
void mop_viewport_destroy_texture(MopViewport *viewport, MopTexture *texture);
void mop_mesh_set_texture(MopMesh *mesh, MopTexture *texture);

/* -------------------------------------------------------------------------
 * Per-mesh TRS (position / rotation / scale)
 *
 * MOP auto-computes the model matrix from TRS each frame.  Using these
 * functions is the recommended path; mop_mesh_set_transform is available
 * as an override for callers who compute their own matrix.
 *
 * Rotation components are euler angles in radians (Rz * Ry * Rx order).
 * Default: position = (0,0,0), rotation = (0,0,0), scale = (1,1,1).
 * ------------------------------------------------------------------------- */

void    mop_mesh_set_position(MopMesh *mesh, MopVec3 position);
void    mop_mesh_set_rotation(MopMesh *mesh, MopVec3 rotation);
void    mop_mesh_set_scale(MopMesh *mesh, MopVec3 scale);
MopVec3 mop_mesh_get_position(const MopMesh *mesh);
MopVec3 mop_mesh_get_rotation(const MopMesh *mesh);
MopVec3 mop_mesh_get_scale(const MopMesh *mesh);

/* -------------------------------------------------------------------------
 * Hierarchical transforms (Scene Graph)
 *
 * A mesh may have a single parent.  When rendering, MOP computes the
 * world_transform as the product of all ancestor transforms down to the
 * mesh's own local transform.  This is done in two passes: roots first,
 * then children.
 *
 * parent_index = -1 means no parent (root).
 * ------------------------------------------------------------------------- */

/* Set a mesh's parent.  The parent must belong to the same viewport. */
void mop_mesh_set_parent(MopMesh *mesh, MopMesh *parent, MopViewport *viewport);

/* Remove a mesh's parent (make it a root node). */
void mop_mesh_clear_parent(MopMesh *mesh);

/* -------------------------------------------------------------------------
 * Instanced mesh API (Phase 6B)
 *
 * An instanced mesh stores a single piece of geometry with multiple
 * per-instance transforms.  The engine draws the mesh once per instance
 * using the instanced draw path.
 * ------------------------------------------------------------------------- */

typedef struct MopInstancedMesh MopInstancedMesh;

/* Add an instanced mesh to the viewport.  Returns NULL on failure.
 * The engine copies transforms; the application may free its array. */
MopInstancedMesh *mop_viewport_add_instanced_mesh(MopViewport *viewport,
                                                    const MopMeshDesc *desc,
                                                    const MopMat4 *transforms,
                                                    uint32_t instance_count);

/* Update the per-instance transforms of an instanced mesh. */
void mop_instanced_mesh_update_transforms(MopInstancedMesh *mesh,
                                           const MopMat4 *transforms,
                                           uint32_t count);

/* Remove an instanced mesh from the viewport and free its resources. */
void mop_viewport_remove_instanced_mesh(MopViewport *viewport,
                                         MopInstancedMesh *mesh);

#endif /* MOP_SCENE_H */
