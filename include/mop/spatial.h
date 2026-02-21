/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * spatial.h — Axis-aligned bounding boxes and spatial queries
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_SPATIAL_H
#define MOP_SPATIAL_H

#include "camera_query.h"
#include "types.h"

/* Forward declarations */
typedef struct MopViewport MopViewport;
typedef struct MopMesh MopMesh;

/* -------------------------------------------------------------------------
 * AABB — axis-aligned bounding box
 * ------------------------------------------------------------------------- */

typedef struct MopAABB {
  MopVec3 min;
  MopVec3 max;
} MopAABB;

/* -------------------------------------------------------------------------
 * Per-mesh AABB
 * ------------------------------------------------------------------------- */

/* Compute the local-space AABB from vertex data. */
MopAABB mop_mesh_get_aabb_local(const MopMesh *mesh, const MopViewport *vp);

/* Compute the world-space AABB by transforming the 8 corners of the
 * local AABB by the mesh's world_transform and re-fitting. */
MopAABB mop_mesh_get_aabb_world(const MopMesh *mesh, const MopViewport *vp);

/* -------------------------------------------------------------------------
 * AABB utilities
 * ------------------------------------------------------------------------- */

MopAABB mop_aabb_union(MopAABB a, MopAABB b);
bool mop_aabb_overlaps(MopAABB a, MopAABB b);
MopVec3 mop_aabb_center(MopAABB box);
MopVec3 mop_aabb_extents(MopAABB box);
float mop_aabb_surface_area(MopAABB box);

/* -------------------------------------------------------------------------
 * Frustum — 6 planes extracted from the view-projection matrix
 *
 * Each plane is (a, b, c, d) where ax + by + cz + d >= 0 is inside.
 * Planes order: left, right, bottom, top, near, far.
 * ------------------------------------------------------------------------- */

typedef struct MopFrustum {
  MopVec4 planes[6];
} MopFrustum;

/* Extract frustum planes from the viewport's current VP matrix.
 * Uses the Gribb-Hartmann method. */
MopFrustum mop_viewport_get_frustum(const MopViewport *vp);

/* Test an AABB against the frustum.
 * Returns:  1 = fully inside, 0 = intersects, -1 = fully outside */
int mop_frustum_test_aabb(const MopFrustum *frustum, MopAABB box);

/* -------------------------------------------------------------------------
 * Ray intersection
 * ------------------------------------------------------------------------- */

/* Ray-AABB intersection (slab method).
 * Returns true if the ray hits the box.  t_near/t_far are entry/exit. */
bool mop_ray_intersect_aabb(MopRay ray, MopAABB box, float *t_near,
                            float *t_far);

/* Ray-triangle intersection (Moller-Trumbore).
 * Returns true if hit.  t = distance, u/v = barycentric coords. */
bool mop_ray_intersect_triangle(MopRay ray, MopVec3 v0, MopVec3 v1, MopVec3 v2,
                                float *t, float *u, float *v);

/* -------------------------------------------------------------------------
 * Scene-level spatial queries
 * ------------------------------------------------------------------------- */

/* World-space AABB of the entire scene (union of all mesh AABBs). */
MopAABB mop_viewport_get_scene_aabb(const MopViewport *vp);

/* Count how many scene meshes are visible in the current frustum. */
uint32_t mop_viewport_visible_mesh_count(const MopViewport *vp);

/* -------------------------------------------------------------------------
 * CPU raycast — AABB broadphase + triangle narrowphase
 * ------------------------------------------------------------------------- */

typedef struct MopRayHit {
  bool hit;
  uint32_t object_id;
  float distance;
  MopVec3 position;
  MopVec3 normal;
  float u, v;
  uint32_t triangle_index;
} MopRayHit;

/* Cast from a pixel position (top-left origin). */
MopRayHit mop_viewport_raycast(const MopViewport *vp, float pixel_x,
                               float pixel_y);

/* Cast an arbitrary world-space ray. */
MopRayHit mop_viewport_raycast_ray(const MopViewport *vp, MopRay ray);

#endif /* MOP_SPATIAL_H */
