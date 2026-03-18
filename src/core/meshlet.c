/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * meshlet.c — Meshlet generation (Phase 10)
 *
 * Greedy meshlet builder: iterates triangles, packing into clusters
 * that respect the vertex/triangle limits.  Computes per-meshlet
 * bounding spheres and normal cones for GPU culling.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <mop/core/meshlet.h>
#include <mop/util/log.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Bounding sphere computation (Ritter's algorithm)
 * ------------------------------------------------------------------------- */

static void compute_bounding_sphere(const MopVertex *vertices,
                                    const uint32_t *vert_indices,
                                    uint32_t vert_count, float out_center[3],
                                    float *out_radius) {
  if (vert_count == 0) {
    out_center[0] = out_center[1] = out_center[2] = 0.0f;
    *out_radius = 0.0f;
    return;
  }

  /* Initial center = centroid */
  float cx = 0, cy = 0, cz = 0;
  for (uint32_t i = 0; i < vert_count; i++) {
    const MopVec3 *p = &vertices[vert_indices[i]].position;
    cx += p->x;
    cy += p->y;
    cz += p->z;
  }
  float inv_n = 1.0f / (float)vert_count;
  cx *= inv_n;
  cy *= inv_n;
  cz *= inv_n;

  /* Radius = max distance from center */
  float r2 = 0.0f;
  for (uint32_t i = 0; i < vert_count; i++) {
    const MopVec3 *p = &vertices[vert_indices[i]].position;
    float dx = p->x - cx, dy = p->y - cy, dz = p->z - cz;
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > r2)
      r2 = d2;
  }

  out_center[0] = cx;
  out_center[1] = cy;
  out_center[2] = cz;
  *out_radius = sqrtf(r2);
}

/* -------------------------------------------------------------------------
 * Normal cone computation
 * ------------------------------------------------------------------------- */

static void compute_normal_cone(const MopVertex *vertices,
                                const uint32_t *indices, uint32_t tri_count,
                                const float center[3], MopMeshletCone *cone) {
  if (tri_count == 0) {
    memset(cone, 0, sizeof(*cone));
    cone->cutoff = -1.0f; /* never cull */
    return;
  }

  /* Compute average normal */
  float avg_nx = 0, avg_ny = 0, avg_nz = 0;
  for (uint32_t t = 0; t < tri_count; t++) {
    uint32_t i0 = indices[t * 3 + 0];
    uint32_t i1 = indices[t * 3 + 1];
    uint32_t i2 = indices[t * 3 + 2];

    /* Face normal via cross product */
    const MopVec3 *p0 = &vertices[i0].position;
    const MopVec3 *p1 = &vertices[i1].position;
    const MopVec3 *p2 = &vertices[i2].position;

    float e1x = p1->x - p0->x, e1y = p1->y - p0->y, e1z = p1->z - p0->z;
    float e2x = p2->x - p0->x, e2y = p2->y - p0->y, e2z = p2->z - p0->z;

    float nx = e1y * e2z - e1z * e2y;
    float ny = e1z * e2x - e1x * e2z;
    float nz = e1x * e2y - e1y * e2x;

    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) {
      float inv = 1.0f / len;
      avg_nx += nx * inv;
      avg_ny += ny * inv;
      avg_nz += nz * inv;
    }
  }

  float alen = sqrtf(avg_nx * avg_nx + avg_ny * avg_ny + avg_nz * avg_nz);
  if (alen > 1e-8f) {
    float inv = 1.0f / alen;
    avg_nx *= inv;
    avg_ny *= inv;
    avg_nz *= inv;
  } else {
    avg_nx = 0;
    avg_ny = 1;
    avg_nz = 0;
  }

  cone->axis[0] = avg_nx;
  cone->axis[1] = avg_ny;
  cone->axis[2] = avg_nz;

  /* Cutoff = minimum dot product of any face normal with the average */
  float min_dot = 1.0f;
  for (uint32_t t = 0; t < tri_count; t++) {
    uint32_t i0 = indices[t * 3 + 0];
    uint32_t i1 = indices[t * 3 + 1];
    uint32_t i2 = indices[t * 3 + 2];

    const MopVec3 *p0 = &vertices[i0].position;
    const MopVec3 *p1 = &vertices[i1].position;
    const MopVec3 *p2 = &vertices[i2].position;

    float e1x = p1->x - p0->x, e1y = p1->y - p0->y, e1z = p1->z - p0->z;
    float e2x = p2->x - p0->x, e2y = p2->y - p0->y, e2z = p2->z - p0->z;

    float nx = e1y * e2z - e1z * e2y;
    float ny = e1z * e2x - e1x * e2z;
    float nz = e1x * e2y - e1y * e2x;

    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) {
      float inv = 1.0f / len;
      float dot =
          (nx * inv) * avg_nx + (ny * inv) * avg_ny + (nz * inv) * avg_nz;
      if (dot < min_dot)
        min_dot = dot;
    }
  }

  cone->cutoff = min_dot;

  /* Apex = center offset along -axis by radius/sin(angle) */
  cone->apex[0] = center[0];
  cone->apex[1] = center[1];
  cone->apex[2] = center[2];
}

/* -------------------------------------------------------------------------
 * Meshlet builder
 * ------------------------------------------------------------------------- */

uint32_t mop_meshlet_count_estimate(uint32_t triangle_count) {
  if (triangle_count == 0)
    return 0;
  return (triangle_count + MOP_MESHLET_MAX_TRIANGLES - 1) /
         MOP_MESHLET_MAX_TRIANGLES;
}

bool mop_meshlet_build(const MopVertex *vertices, uint32_t vertex_count,
                       const uint32_t *indices, uint32_t index_count,
                       MopMeshletData *out) {
  if (!out)
    return false;
  memset(out, 0, sizeof(*out));
  if (!vertices || !indices || vertex_count == 0 || index_count == 0) {
    MOP_WARN("mop_meshlet_build: invalid input");
    return false;
  }
  if (index_count % 3 != 0) {
    MOP_WARN("mop_meshlet_build: index_count must be multiple of 3");
    return false;
  }

  uint32_t tri_count = index_count / 3;

  /* Estimate max meshlets — must account for both the triangle limit
   * (124 per meshlet) and the vertex limit (64 per meshlet).  Worst case
   * for vertex limit: every triangle uses 3 unique vertices →
   * floor(64/3) = 21 triangles per meshlet. */
  uint32_t by_tris =
      (tri_count + MOP_MESHLET_MAX_TRIANGLES - 1) / MOP_MESHLET_MAX_TRIANGLES;
  uint32_t min_tris_per_vert_limit = MOP_MESHLET_MAX_VERTICES / 3;
  uint32_t by_verts =
      (tri_count + min_tris_per_vert_limit - 1) / min_tris_per_vert_limit;
  uint32_t max_meshlets = (by_verts > by_tris ? by_verts : by_tris) + 1;

  out->meshlets = (MopMeshlet *)calloc(max_meshlets, sizeof(MopMeshlet));
  out->cones = (MopMeshletCone *)calloc(max_meshlets, sizeof(MopMeshletCone));
  /* Worst case: every vertex unique per meshlet */
  uint32_t max_vert_indices = max_meshlets * MOP_MESHLET_MAX_VERTICES;
  out->vertex_indices = (uint32_t *)malloc(max_vert_indices * sizeof(uint32_t));
  /* Worst case: max triangles per meshlet */
  uint32_t max_prim = max_meshlets * MOP_MESHLET_MAX_INDICES;
  out->prim_indices = (uint8_t *)malloc(max_prim);

  if (!out->meshlets || !out->cones || !out->vertex_indices ||
      !out->prim_indices) {
    mop_meshlet_free(out);
    return false;
  }

  /* Per-meshlet working state */
  uint32_t local_verts[MOP_MESHLET_MAX_VERTICES];
  uint32_t local_vert_count = 0;
  uint8_t local_prims[MOP_MESHLET_MAX_INDICES];
  uint32_t local_prim_count = 0;

  /* Map from global vertex index to local meshlet index (-1 = not added) */
  int32_t *vert_map = (int32_t *)malloc(vertex_count * sizeof(int32_t));
  if (!vert_map) {
    mop_meshlet_free(out);
    return false;
  }
  memset(vert_map, -1, vertex_count * sizeof(int32_t));

  /* Greedy pass: pack triangles into meshlets */
  uint32_t meshlet_idx = 0;
  uint32_t total_vert_indices = 0;
  uint32_t total_prim_indices = 0;

  /* Original triangle indices for normal cone computation */
  uint32_t meshlet_tri_start = 0;

  for (uint32_t t = 0; t < tri_count; t++) {
    uint32_t i0 = indices[t * 3 + 0];
    uint32_t i1 = indices[t * 3 + 1];
    uint32_t i2 = indices[t * 3 + 2];

    /* Count how many new vertices this triangle needs */
    uint32_t new_verts = 0;
    if (vert_map[i0] < 0)
      new_verts++;
    if (vert_map[i1] < 0)
      new_verts++;
    if (vert_map[i2] < 0)
      new_verts++;

    /* Check if triangle fits in current meshlet */
    bool fits = (local_vert_count + new_verts <= MOP_MESHLET_MAX_VERTICES) &&
                (local_prim_count / 3 + 1 <= MOP_MESHLET_MAX_TRIANGLES);

    if (!fits && local_prim_count > 0) {
      /* Emit current meshlet */
      MopMeshlet *ml = &out->meshlets[meshlet_idx];
      ml->vertex_offset = total_vert_indices;
      ml->vertex_count = local_vert_count;
      ml->triangle_offset = total_prim_indices;
      ml->triangle_count = local_prim_count / 3;

      /* Bounding sphere */
      compute_bounding_sphere(vertices, local_verts, local_vert_count,
                              ml->center, &ml->radius);

      /* Normal cone */
      compute_normal_cone(vertices, indices + meshlet_tri_start * 3,
                          local_prim_count / 3, ml->center,
                          &out->cones[meshlet_idx]);

      /* Copy to output buffers */
      memcpy(out->vertex_indices + total_vert_indices, local_verts,
             local_vert_count * sizeof(uint32_t));
      total_vert_indices += local_vert_count;

      memcpy(out->prim_indices + total_prim_indices, local_prims,
             local_prim_count);
      total_prim_indices += local_prim_count;

      meshlet_idx++;

      /* Reset for next meshlet */
      for (uint32_t v = 0; v < local_vert_count; v++)
        vert_map[local_verts[v]] = -1;
      local_vert_count = 0;
      local_prim_count = 0;
      meshlet_tri_start = t;
    }

    /* Add vertices */
    uint32_t li0, li1, li2;
    if (vert_map[i0] < 0) {
      vert_map[i0] = (int32_t)local_vert_count;
      local_verts[local_vert_count++] = i0;
    }
    li0 = (uint32_t)vert_map[i0];

    if (vert_map[i1] < 0) {
      vert_map[i1] = (int32_t)local_vert_count;
      local_verts[local_vert_count++] = i1;
    }
    li1 = (uint32_t)vert_map[i1];

    if (vert_map[i2] < 0) {
      vert_map[i2] = (int32_t)local_vert_count;
      local_verts[local_vert_count++] = i2;
    }
    li2 = (uint32_t)vert_map[i2];

    /* Add triangle with local indices */
    local_prims[local_prim_count++] = (uint8_t)li0;
    local_prims[local_prim_count++] = (uint8_t)li1;
    local_prims[local_prim_count++] = (uint8_t)li2;
  }

  /* Emit final meshlet */
  if (local_prim_count > 0) {
    MopMeshlet *ml = &out->meshlets[meshlet_idx];
    ml->vertex_offset = total_vert_indices;
    ml->vertex_count = local_vert_count;
    ml->triangle_offset = total_prim_indices;
    ml->triangle_count = local_prim_count / 3;

    compute_bounding_sphere(vertices, local_verts, local_vert_count, ml->center,
                            &ml->radius);
    compute_normal_cone(vertices, indices + meshlet_tri_start * 3,
                        local_prim_count / 3, ml->center,
                        &out->cones[meshlet_idx]);

    memcpy(out->vertex_indices + total_vert_indices, local_verts,
           local_vert_count * sizeof(uint32_t));
    total_vert_indices += local_vert_count;

    memcpy(out->prim_indices + total_prim_indices, local_prims,
           local_prim_count);
    total_prim_indices += local_prim_count;

    meshlet_idx++;
  }

  free(vert_map);

  out->meshlet_count = meshlet_idx;
  out->vertex_index_count = total_vert_indices;
  out->prim_index_count = total_prim_indices;

  return true;
}

/* -------------------------------------------------------------------------
 * Free
 * ------------------------------------------------------------------------- */

void mop_meshlet_free(MopMeshletData *data) {
  if (!data)
    return;
  free(data->meshlets);
  free(data->cones);
  free(data->vertex_indices);
  free(data->prim_indices);
  memset(data, 0, sizeof(*data));
}
