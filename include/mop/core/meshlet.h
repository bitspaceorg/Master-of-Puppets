/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * meshlet.h — Meshlet generation and mesh shading (Phase 10)
 *
 * Meshlets are small fixed-size clusters of triangles (up to 64 vertices
 * and 124 triangles per meshlet).  They are the fundamental unit of work
 * for task/mesh shader pipelines (VK_EXT_mesh_shader).
 *
 * Each meshlet contains:
 *   - A compact list of unique vertex indices (into the original mesh)
 *   - A compact list of local triangle indices (3 uint8 per triangle)
 *   - A bounding sphere and normal cone for GPU culling
 *
 * The meshlet generation algorithm greedily builds clusters that maximize
 * spatial locality, then computes per-meshlet culling bounds.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_MESHLET_H
#define MOP_CORE_MESHLET_H

#include <mop/types.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Meshlet limits (matching VK_EXT_mesh_shader recommendations)
 * ------------------------------------------------------------------------- */

#define MOP_MESHLET_MAX_VERTICES 64
#define MOP_MESHLET_MAX_TRIANGLES 124
#define MOP_MESHLET_MAX_INDICES (MOP_MESHLET_MAX_TRIANGLES * 3)

/* -------------------------------------------------------------------------
 * Meshlet descriptor — one per cluster
 *
 * GPU layout (std430): suitable for direct upload to SSBO.
 * Total: 32 bytes per meshlet.
 * ------------------------------------------------------------------------- */

typedef struct MopMeshlet {
  uint32_t vertex_offset;   /* offset into meshlet vertex index buffer */
  uint32_t vertex_count;    /* number of unique vertices (<=64) */
  uint32_t triangle_offset; /* offset into meshlet primitive index buffer */
  uint32_t triangle_count;  /* number of triangles (<=124) */

  /* Bounding sphere (object space) for frustum/occlusion culling */
  float center[3]; /* sphere center */
  float radius;    /* sphere radius */
} MopMeshlet;

/* -------------------------------------------------------------------------
 * Meshlet normal cone — for backface culling in task shader
 *
 * If dot(normalize(view_dir), cone_axis) < cone_cutoff, the entire
 * meshlet is backface-facing and can be culled.
 * ------------------------------------------------------------------------- */

typedef struct MopMeshletCone {
  float axis[3]; /* average normal direction */
  float cutoff;  /* cosine of half-angle (negative = wide) */
  float apex[3]; /* cone apex (object space) */
  float _pad;
} MopMeshletCone;

/* -------------------------------------------------------------------------
 * Meshlet build result — output of mop_meshlet_build
 * ------------------------------------------------------------------------- */

typedef struct MopMeshletData {
  MopMeshlet *meshlets; /* array of meshlet descriptors */
  uint32_t meshlet_count;

  MopMeshletCone *cones; /* parallel array: normal cone per meshlet */

  uint32_t *vertex_indices; /* meshlet-local → global vertex index map */
  uint32_t vertex_index_count;

  uint8_t *prim_indices;     /* packed triangle indices (3 × uint8 per tri) */
  uint32_t prim_index_count; /* total uint8 count (= total_triangles * 3) */
} MopMeshletData;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Build meshlets from a triangle mesh.
 *
 * vertices:      array of MopVertex (positions used for spatial clustering)
 * vertex_count:  number of vertices
 * indices:       triangle index array (3 indices per triangle)
 * index_count:   total index count (must be multiple of 3)
 *
 * Returns true on success.  out must be freed with mop_meshlet_free. */
bool mop_meshlet_build(const MopVertex *vertices, uint32_t vertex_count,
                       const uint32_t *indices, uint32_t index_count,
                       MopMeshletData *out);

/* Free all memory allocated by mop_meshlet_build. */
void mop_meshlet_free(MopMeshletData *data);

/* Compute the number of meshlets needed for a mesh. */
uint32_t mop_meshlet_count_estimate(uint32_t triangle_count);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_MESHLET_H */
