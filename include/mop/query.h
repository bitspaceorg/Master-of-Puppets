/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * query.h — Scene query API for external consumers
 *
 * Read-only access to mesh data, materials, lights, and transforms.
 * All pointers returned by query functions point into MOP-owned memory
 * and are valid until the next mop_viewport_render, mop_viewport_resize,
 * or mop_viewport_destroy call.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_QUERY_H
#define MOP_QUERY_H

#include "light.h"
#include "material.h"
#include "types.h"
#include "vertex_format.h"

/* Forward declarations */
typedef struct MopViewport MopViewport;
typedef struct MopMesh MopMesh;

/* -------------------------------------------------------------------------
 * Mesh enumeration
 * ------------------------------------------------------------------------- */

/* Return the number of active scene meshes in the viewport.
 * Excludes internal meshes (grid, gizmo handles, background). */
uint32_t mop_viewport_mesh_count(const MopViewport *vp);

/* Return the mesh at the given active-mesh index [0, mesh_count).
 * Returns NULL if index is out of range. */
MopMesh *mop_viewport_mesh_at(const MopViewport *vp, uint32_t index);

/* Find a mesh by its object_id.  Returns NULL if not found.
 * O(n) scan — for hot paths, cache the result. */
MopMesh *mop_viewport_mesh_by_id(const MopViewport *vp, uint32_t object_id);

/* -------------------------------------------------------------------------
 * Mesh introspection — read-only access to per-mesh data
 * ------------------------------------------------------------------------- */

/* Identity */
uint32_t mop_mesh_get_object_id(const MopMesh *mesh);
bool mop_mesh_is_active(const MopMesh *mesh);

/* Geometry counts */
uint32_t mop_mesh_get_vertex_count(const MopMesh *mesh);
uint32_t mop_mesh_get_index_count(const MopMesh *mesh);
uint32_t mop_mesh_get_triangle_count(const MopMesh *mesh);

/* Vertex data — zero-copy pointer into the RHI buffer.
 * Returns NULL if the mesh uses a flexible vertex format (use
 * mop_mesh_get_vertex_data_raw + mop_mesh_get_vertex_format instead).
 * Pointer valid until next geometry update or viewport destroy. */
const MopVertex *mop_mesh_get_vertices(const MopMesh *mesh,
                                       const MopViewport *vp);

/* Index data — zero-copy pointer into the RHI buffer. */
const uint32_t *mop_mesh_get_indices(const MopMesh *mesh,
                                     const MopViewport *vp);

/* Raw vertex data for flexible-format meshes.
 * Returns the raw byte pointer and stride.  Returns NULL for
 * standard-format meshes (use mop_mesh_get_vertices instead). */
const void *mop_mesh_get_vertex_data_raw(const MopMesh *mesh,
                                         const MopViewport *vp,
                                         uint32_t *out_stride);

/* Vertex format — NULL means standard MopVertex layout. */
const MopVertexFormat *mop_mesh_get_vertex_format(const MopMesh *mesh);

/* Transforms */
MopMat4 mop_mesh_get_local_transform(const MopMesh *mesh);
MopMat4 mop_mesh_get_world_transform(const MopMesh *mesh);

/* Material — returns the material if set, otherwise the default. */
MopMaterial mop_mesh_get_material(const MopMesh *mesh);
bool mop_mesh_has_material(const MopMesh *mesh);

/* Blend and opacity */
MopBlendMode mop_mesh_get_blend_mode(const MopMesh *mesh);
float mop_mesh_get_opacity(const MopMesh *mesh);

/* -------------------------------------------------------------------------
 * Light enumeration
 * ------------------------------------------------------------------------- */

/* Return the light at the given index [0, mop_viewport_light_count).
 * Returns NULL if index is out of range or the light is inactive. */
const MopLight *mop_viewport_light_at(const MopViewport *vp, uint32_t index);

#endif /* MOP_QUERY_H */
