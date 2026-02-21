/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * snapshot.h — Scene snapshot for raytracers and external consumers
 *
 * Zero-copy scene iterator.  All pointers reference MOP-owned memory
 * and are valid until the next mop_viewport_render call.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_SNAPSHOT_H
#define MOP_SNAPSHOT_H

#include "camera_query.h"
#include "light.h"
#include "material.h"
#include "types.h"

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Mesh view — read-only view into one mesh's data
 *
 * vertices/indices point into RHI buffer memory (zero-copy).
 * world_transform is the fully-resolved hierarchical transform.
 * ------------------------------------------------------------------------- */

typedef struct MopMeshView {
  uint32_t object_id;
  uint32_t vertex_count;
  uint32_t index_count;
  const MopVertex *vertices;
  const uint32_t *indices;
  MopMat4 world_transform;
  MopMaterial material;
  float opacity;
  MopBlendMode blend_mode;
} MopMeshView;

/* -------------------------------------------------------------------------
 * Scene snapshot — complete read-only view of the scene
 * ------------------------------------------------------------------------- */

typedef struct MopSceneSnapshot {
  /* Camera */
  MopCameraState camera;

  /* Framebuffer dimensions */
  int width;
  int height;

  /* Lights */
  const MopLight *lights;
  uint32_t light_count;

  /* Iteration state (opaque — do not access directly) */
  const MopViewport *_vp;
  uint32_t _mesh_idx;
} MopSceneSnapshot;

/* Take a snapshot of the current scene state.  The snapshot is valid
 * until the next mop_viewport_render, resize, or destroy.
 * Call this AFTER mop_viewport_render to get consistent transforms. */
MopSceneSnapshot mop_viewport_snapshot(const MopViewport *vp);

/* -------------------------------------------------------------------------
 * Mesh iterator
 *
 * Walk all scene meshes without allocation:
 *
 *   MopSceneSnapshot snap = mop_viewport_snapshot(vp);
 *   MopMeshView mesh;
 *   while (mop_snapshot_next_mesh(&snap, &mesh)) {
 *       // mesh.vertices, mesh.indices, mesh.world_transform ...
 *   }
 *
 * To restart iteration, call mop_snapshot_reset.
 * ------------------------------------------------------------------------- */

bool mop_snapshot_next_mesh(MopSceneSnapshot *snap, MopMeshView *out);
void mop_snapshot_reset(MopSceneSnapshot *snap);

/* Total number of scene meshes (same as mop_viewport_mesh_count).
 * Useful for pre-allocating raytracer acceleration structures. */
uint32_t mop_snapshot_mesh_count(const MopSceneSnapshot *snap);

/* -------------------------------------------------------------------------
 * Triangle iterator
 *
 * For raytracers that want world-space triangles directly.
 * Transforms positions and normals into world space on the fly.
 * ------------------------------------------------------------------------- */

typedef struct MopTriangle {
  MopVec3 p[3];   /* world-space positions */
  MopVec3 n[3];   /* world-space normals */
  MopColor c[3];  /* vertex colors */
  float uv[3][2]; /* texture coordinates */
  MopMaterial material;
  uint32_t object_id;
} MopTriangle;

typedef struct MopTriangleIter {
  MopSceneSnapshot _snap;
  MopMeshView _current_mesh;
  uint32_t _tri_idx;
  bool _has_mesh;
  MopMat4 _normal_matrix;
} MopTriangleIter;

MopTriangleIter mop_triangle_iter_begin(const MopViewport *vp);
bool mop_triangle_iter_next(MopTriangleIter *iter, MopTriangle *out);

/* Total triangle count across all scene meshes (for BVH pre-allocation). */
uint32_t mop_snapshot_triangle_count(const MopSceneSnapshot *snap);

#endif /* MOP_SNAPSHOT_H */
