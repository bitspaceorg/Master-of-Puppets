/*
 * Master of Puppets — Mesh Editing Operations
 * mesh_edit.c — Vertex, edge, and face editing via half-edge auxiliary
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include <mop/mop.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Half-edge auxiliary structure
 *
 * Built on demand from the mesh's index buffer.  Used internally by
 * editing operations that need adjacency information.
 * ------------------------------------------------------------------------- */

typedef struct MopHalfEdge {
  uint32_t vertex; /* destination vertex */
  uint32_t face;   /* adjacent face */
  uint32_t next;   /* next half-edge in face */
  uint32_t twin;   /* opposite half-edge (UINT32_MAX if boundary) */
} MopHalfEdge;

#define MOP_INVALID_HE UINT32_MAX

/* Build half-edge structure from triangle index buffer.
 * Returns allocated array of half-edges (3 per face).
 * Caller must free().  Returns NULL on failure. */
MopHalfEdge *mop_mesh_build_half_edges(const uint32_t *indices,
                                       uint32_t index_count,
                                       uint32_t vertex_count) {
  uint32_t face_count = index_count / 3;
  uint32_t he_count = face_count * 3;
  if (he_count == 0)
    return NULL;

  MopHalfEdge *edges = (MopHalfEdge *)calloc(he_count, sizeof(MopHalfEdge));
  if (!edges)
    return NULL;

  /* Initialize half-edges from triangles */
  for (uint32_t f = 0; f < face_count; f++) {
    uint32_t base = f * 3;
    for (uint32_t e = 0; e < 3; e++) {
      uint32_t he_idx = base + e;
      uint32_t next_e = (e + 1) % 3;
      edges[he_idx].vertex = indices[base + next_e];
      edges[he_idx].face = f;
      edges[he_idx].next = base + next_e;
      edges[he_idx].twin = MOP_INVALID_HE;
    }
  }

  /* Find twins: brute force O(n^2) for simplicity — acceptable for edit ops */
  for (uint32_t i = 0; i < he_count; i++) {
    if (edges[i].twin != MOP_INVALID_HE)
      continue;
    /* This half-edge goes from src -> dst */
    uint32_t dst_i = edges[i].vertex;
    uint32_t base_i = (i / 3) * 3;
    uint32_t local_i = i - base_i;
    /* Source vertex is the previous edge's destination, or the triangle's
     * vertex at the current local index */
    uint32_t src_i = indices[base_i + local_i];

    for (uint32_t j = i + 1; j < he_count; j++) {
      if (edges[j].twin != MOP_INVALID_HE)
        continue;
      uint32_t dst_j = edges[j].vertex;
      uint32_t base_j = (j / 3) * 3;
      uint32_t local_j = j - base_j;
      uint32_t src_j = indices[base_j + local_j];

      /* Twin: opposite direction */
      if (src_i == dst_j && dst_i == src_j) {
        edges[i].twin = j;
        edges[j].twin = i;
        break;
      }
    }
  }

  (void)vertex_count;
  return edges;
}

/* -------------------------------------------------------------------------
 * Vertex operations
 * ------------------------------------------------------------------------- */

void mop_mesh_move_vertices(MopMesh *mesh, MopViewport *vp,
                            const uint32_t *indices, uint32_t count,
                            MopVec3 delta) {
  if (!mesh || !vp || !indices || count == 0)
    return;
  if (!mesh->vertex_buffer)
    return;

  const MopVertex *src =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  if (!src)
    return;

  uint32_t vc = mesh->vertex_count;
  MopVertex *verts = (MopVertex *)malloc(vc * sizeof(MopVertex));
  if (!verts)
    return;
  memcpy(verts, src, vc * sizeof(MopVertex));

  for (uint32_t i = 0; i < count; i++) {
    uint32_t idx = indices[i];
    if (idx < vc) {
      verts[idx].position.x += delta.x;
      verts[idx].position.y += delta.y;
      verts[idx].position.z += delta.z;
    }
  }

  /* Read index data to pass through unchanged */
  const uint32_t *idx_src = NULL;
  if (mesh->index_buffer)
    idx_src = (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);

  if (idx_src) {
    mop_mesh_update_geometry(mesh, vp, verts, vc, idx_src, mesh->index_count);
  }
  free(verts);
}

void mop_mesh_delete_vertices(MopMesh *mesh, MopViewport *vp,
                              const uint32_t *indices, uint32_t count) {
  if (!mesh || !vp || !indices || count == 0)
    return;
  if (!mesh->vertex_buffer || !mesh->index_buffer)
    return;

  const MopVertex *src =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  const uint32_t *idx_src =
      (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
  if (!src || !idx_src)
    return;

  uint32_t vc = mesh->vertex_count;
  uint32_t ic = mesh->index_count;

  /* Mark vertices for deletion */
  bool *deleted = (bool *)calloc(vc, sizeof(bool));
  if (!deleted)
    return;

  for (uint32_t i = 0; i < count; i++) {
    if (indices[i] < vc)
      deleted[indices[i]] = true;
  }

  /* Build remapping table */
  uint32_t *remap = (uint32_t *)malloc(vc * sizeof(uint32_t));
  if (!remap) {
    free(deleted);
    return;
  }

  uint32_t new_vc = 0;
  for (uint32_t i = 0; i < vc; i++) {
    if (deleted[i]) {
      remap[i] = UINT32_MAX;
    } else {
      remap[i] = new_vc;
      new_vc++;
    }
  }

  /* Build new vertex array */
  MopVertex *new_verts = (MopVertex *)malloc(new_vc * sizeof(MopVertex));
  if (!new_verts) {
    free(deleted);
    free(remap);
    return;
  }

  for (uint32_t i = 0; i < vc; i++) {
    if (!deleted[i])
      new_verts[remap[i]] = src[i];
  }

  /* Build new index array, removing faces that reference deleted vertices */
  uint32_t *new_indices = (uint32_t *)malloc(ic * sizeof(uint32_t));
  if (!new_indices) {
    free(deleted);
    free(remap);
    free(new_verts);
    return;
  }

  uint32_t new_ic = 0;
  uint32_t face_count = ic / 3;
  for (uint32_t f = 0; f < face_count; f++) {
    uint32_t i0 = idx_src[f * 3 + 0];
    uint32_t i1 = idx_src[f * 3 + 1];
    uint32_t i2 = idx_src[f * 3 + 2];

    /* Skip face if any vertex was deleted */
    if (i0 >= vc || i1 >= vc || i2 >= vc)
      continue;
    if (deleted[i0] || deleted[i1] || deleted[i2])
      continue;

    new_indices[new_ic + 0] = remap[i0];
    new_indices[new_ic + 1] = remap[i1];
    new_indices[new_ic + 2] = remap[i2];
    new_ic += 3;
  }

  if (new_vc > 0 && new_ic > 0) {
    mop_mesh_update_geometry(mesh, vp, new_verts, new_vc, new_indices, new_ic);
  }

  free(deleted);
  free(remap);
  free(new_verts);
  free(new_indices);
}

void mop_mesh_merge_vertices(MopMesh *mesh, MopViewport *vp, uint32_t v0,
                             uint32_t v1) {
  if (!mesh || !vp)
    return;
  if (!mesh->vertex_buffer || !mesh->index_buffer)
    return;
  if (v0 == v1)
    return;

  const MopVertex *src =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  const uint32_t *idx_src =
      (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
  if (!src || !idx_src)
    return;

  uint32_t vc = mesh->vertex_count;
  uint32_t ic = mesh->index_count;

  if (v0 >= vc || v1 >= vc)
    return;

  /* Copy vertices — v0 gets the average position of v0 and v1 */
  MopVertex *verts = (MopVertex *)malloc(vc * sizeof(MopVertex));
  uint32_t *indices_buf = (uint32_t *)malloc(ic * sizeof(uint32_t));
  if (!verts || !indices_buf) {
    free(verts);
    free(indices_buf);
    return;
  }

  memcpy(verts, src, vc * sizeof(MopVertex));
  memcpy(indices_buf, idx_src, ic * sizeof(uint32_t));

  /* Average the positions */
  verts[v0].position.x = (src[v0].position.x + src[v1].position.x) * 0.5f;
  verts[v0].position.y = (src[v0].position.y + src[v1].position.y) * 0.5f;
  verts[v0].position.z = (src[v0].position.z + src[v1].position.z) * 0.5f;

  /* Average the normals and renormalize */
  MopVec3 avg_n = {
      (src[v0].normal.x + src[v1].normal.x) * 0.5f,
      (src[v0].normal.y + src[v1].normal.y) * 0.5f,
      (src[v0].normal.z + src[v1].normal.z) * 0.5f,
  };
  verts[v0].normal = mop_vec3_normalize(avg_n);

  /* Replace all references to v1 with v0 */
  for (uint32_t i = 0; i < ic; i++) {
    if (indices_buf[i] == v1)
      indices_buf[i] = v0;
  }

  /* Remove degenerate faces (where v0 appears more than once) */
  uint32_t *clean_idx = (uint32_t *)malloc(ic * sizeof(uint32_t));
  if (!clean_idx) {
    free(verts);
    free(indices_buf);
    return;
  }

  uint32_t new_ic = 0;
  uint32_t face_count = ic / 3;
  for (uint32_t f = 0; f < face_count; f++) {
    uint32_t i0 = indices_buf[f * 3 + 0];
    uint32_t i1 = indices_buf[f * 3 + 1];
    uint32_t i2 = indices_buf[f * 3 + 2];

    /* Skip degenerate faces */
    if (i0 == i1 || i1 == i2 || i2 == i0)
      continue;

    clean_idx[new_ic + 0] = i0;
    clean_idx[new_ic + 1] = i1;
    clean_idx[new_ic + 2] = i2;
    new_ic += 3;
  }

  /* Now remove v1 from the vertex array and reindex */
  uint32_t *remap = (uint32_t *)malloc(vc * sizeof(uint32_t));
  if (!remap) {
    free(verts);
    free(indices_buf);
    free(clean_idx);
    return;
  }

  uint32_t new_vc = 0;
  for (uint32_t i = 0; i < vc; i++) {
    if (i == v1) {
      remap[i] = remap[v0]; /* map to v0's new position */
    } else {
      remap[i] = new_vc;
      new_vc++;
    }
  }
  /* Fix: v1's remap must point to v0's new index */
  remap[v1] = remap[v0];

  MopVertex *final_verts = (MopVertex *)malloc(new_vc * sizeof(MopVertex));
  if (!final_verts) {
    free(verts);
    free(indices_buf);
    free(clean_idx);
    free(remap);
    return;
  }

  for (uint32_t i = 0; i < vc; i++) {
    if (i != v1)
      final_verts[remap[i]] = verts[i];
  }

  /* Remap indices */
  for (uint32_t i = 0; i < new_ic; i++) {
    clean_idx[i] = remap[clean_idx[i]];
  }

  if (new_vc > 0 && new_ic > 0) {
    mop_mesh_update_geometry(mesh, vp, final_verts, new_vc, clean_idx, new_ic);
  }

  free(verts);
  free(indices_buf);
  free(clean_idx);
  free(remap);
  free(final_verts);
}

/* -------------------------------------------------------------------------
 * Edge operations
 * ------------------------------------------------------------------------- */

void mop_mesh_split_edge(MopMesh *mesh, MopViewport *vp, uint32_t edge_v0,
                         uint32_t edge_v1) {
  if (!mesh || !vp)
    return;
  if (!mesh->vertex_buffer || !mesh->index_buffer)
    return;

  const MopVertex *src =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  const uint32_t *idx_src =
      (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
  if (!src || !idx_src)
    return;

  uint32_t vc = mesh->vertex_count;
  uint32_t ic = mesh->index_count;

  if (edge_v0 >= vc || edge_v1 >= vc)
    return;

  /* New vertex at midpoint */
  MopVertex mid_vert;
  mid_vert.position.x =
      (src[edge_v0].position.x + src[edge_v1].position.x) * 0.5f;
  mid_vert.position.y =
      (src[edge_v0].position.y + src[edge_v1].position.y) * 0.5f;
  mid_vert.position.z =
      (src[edge_v0].position.z + src[edge_v1].position.z) * 0.5f;

  MopVec3 avg_n = {
      (src[edge_v0].normal.x + src[edge_v1].normal.x) * 0.5f,
      (src[edge_v0].normal.y + src[edge_v1].normal.y) * 0.5f,
      (src[edge_v0].normal.z + src[edge_v1].normal.z) * 0.5f,
  };
  mid_vert.normal = mop_vec3_normalize(avg_n);

  mid_vert.color.r = (src[edge_v0].color.r + src[edge_v1].color.r) * 0.5f;
  mid_vert.color.g = (src[edge_v0].color.g + src[edge_v1].color.g) * 0.5f;
  mid_vert.color.b = (src[edge_v0].color.b + src[edge_v1].color.b) * 0.5f;
  mid_vert.color.a = (src[edge_v0].color.a + src[edge_v1].color.a) * 0.5f;
  mid_vert.u = (src[edge_v0].u + src[edge_v1].u) * 0.5f;
  mid_vert.v = (src[edge_v0].v + src[edge_v1].v) * 0.5f;

  uint32_t mid_idx = vc; /* index of the new vertex */

  /* New vertex array: original + 1 new */
  uint32_t new_vc = vc + 1;
  MopVertex *new_verts = (MopVertex *)malloc(new_vc * sizeof(MopVertex));
  if (!new_verts)
    return;
  memcpy(new_verts, src, vc * sizeof(MopVertex));
  new_verts[mid_idx] = mid_vert;

  /* For each triangle containing the edge (v0,v1), split into 2 triangles.
   * Max new indices: original + 3 per split face (each split adds 1 face = 3
   * more indices) */
  uint32_t face_count = ic / 3;
  uint32_t max_new_ic = ic + face_count * 3; /* upper bound */
  uint32_t *new_indices = (uint32_t *)malloc(max_new_ic * sizeof(uint32_t));
  if (!new_indices) {
    free(new_verts);
    return;
  }

  uint32_t new_ic = 0;
  for (uint32_t f = 0; f < face_count; f++) {
    uint32_t i0 = idx_src[f * 3 + 0];
    uint32_t i1 = idx_src[f * 3 + 1];
    uint32_t i2 = idx_src[f * 3 + 2];

    /* Check if this face contains edge (edge_v0, edge_v1) */
    int edge_local = -1; /* which edge of the triangle: 0=(i0,i1), 1=(i1,i2),
                            2=(i2,i0) */
    uint32_t tri[3] = {i0, i1, i2};
    for (int e = 0; e < 3; e++) {
      uint32_t ea = tri[e];
      uint32_t eb = tri[(e + 1) % 3];
      if ((ea == edge_v0 && eb == edge_v1) ||
          (ea == edge_v1 && eb == edge_v0)) {
        edge_local = e;
        break;
      }
    }

    if (edge_local < 0) {
      /* Face doesn't contain the edge — keep as-is */
      new_indices[new_ic + 0] = i0;
      new_indices[new_ic + 1] = i1;
      new_indices[new_ic + 2] = i2;
      new_ic += 3;
    } else {
      /* Split the face into two triangles.
       * Edge is tri[edge_local] -> tri[(edge_local+1)%3].
       * Opposite vertex is tri[(edge_local+2)%3]. */
      uint32_t va = tri[edge_local];
      uint32_t vb = tri[(edge_local + 1) % 3];
      uint32_t vc_opp = tri[(edge_local + 2) % 3];

      /* Triangle 1: va, mid, vc_opp */
      new_indices[new_ic + 0] = va;
      new_indices[new_ic + 1] = mid_idx;
      new_indices[new_ic + 2] = vc_opp;
      new_ic += 3;

      /* Triangle 2: mid, vb, vc_opp */
      new_indices[new_ic + 0] = mid_idx;
      new_indices[new_ic + 1] = vb;
      new_indices[new_ic + 2] = vc_opp;
      new_ic += 3;
    }
  }

  mop_mesh_update_geometry(mesh, vp, new_verts, new_vc, new_indices, new_ic);
  free(new_verts);
  free(new_indices);
}

void mop_mesh_dissolve_edge(MopMesh *mesh, MopViewport *vp, uint32_t edge_v0,
                            uint32_t edge_v1) {
  if (!mesh || !vp)
    return;
  if (!mesh->vertex_buffer || !mesh->index_buffer)
    return;

  const MopVertex *src =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  const uint32_t *idx_src =
      (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
  if (!src || !idx_src)
    return;

  uint32_t vc = mesh->vertex_count;
  uint32_t ic = mesh->index_count;

  if (edge_v0 >= vc || edge_v1 >= vc)
    return;

  /* Remove all faces that contain the edge (edge_v0, edge_v1) */
  uint32_t face_count = ic / 3;
  uint32_t *new_indices = (uint32_t *)malloc(ic * sizeof(uint32_t));
  if (!new_indices)
    return;

  uint32_t new_ic = 0;
  for (uint32_t f = 0; f < face_count; f++) {
    uint32_t i0 = idx_src[f * 3 + 0];
    uint32_t i1 = idx_src[f * 3 + 1];
    uint32_t i2 = idx_src[f * 3 + 2];

    /* Check if face contains the edge */
    bool has_edge = false;
    uint32_t tri[3] = {i0, i1, i2};
    for (int e = 0; e < 3; e++) {
      uint32_t ea = tri[e];
      uint32_t eb = tri[(e + 1) % 3];
      if ((ea == edge_v0 && eb == edge_v1) ||
          (ea == edge_v1 && eb == edge_v0)) {
        has_edge = true;
        break;
      }
    }

    if (!has_edge) {
      new_indices[new_ic + 0] = i0;
      new_indices[new_ic + 1] = i1;
      new_indices[new_ic + 2] = i2;
      new_ic += 3;
    }
  }

  /* Copy vertices unchanged */
  MopVertex *new_verts = (MopVertex *)malloc(vc * sizeof(MopVertex));
  if (!new_verts) {
    free(new_indices);
    return;
  }
  memcpy(new_verts, src, vc * sizeof(MopVertex));

  if (new_ic > 0) {
    mop_mesh_update_geometry(mesh, vp, new_verts, vc, new_indices, new_ic);
  }

  free(new_verts);
  free(new_indices);
}

/* -------------------------------------------------------------------------
 * Face operations
 * ------------------------------------------------------------------------- */

void mop_mesh_extrude_faces(MopMesh *mesh, MopViewport *vp,
                            const uint32_t *face_indices, uint32_t count,
                            float distance) {
  if (!mesh || !vp || !face_indices || count == 0)
    return;
  if (!mesh->vertex_buffer || !mesh->index_buffer)
    return;

  const MopVertex *src =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  const uint32_t *idx_src =
      (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
  if (!src || !idx_src)
    return;

  uint32_t vc = mesh->vertex_count;
  uint32_t ic = mesh->index_count;
  uint32_t face_count = ic / 3;

  /* Mark which faces are to be extruded */
  bool *extrude_flag = (bool *)calloc(face_count, sizeof(bool));
  if (!extrude_flag)
    return;

  for (uint32_t i = 0; i < count; i++) {
    if (face_indices[i] < face_count)
      extrude_flag[face_indices[i]] = true;
  }

  /* For each extruded face, we duplicate its 3 vertices (offset by normal *
   * distance), replace the original face indices to use the new vertices, and
   * create 3 side-wall quads (6 triangles). */

  /* Upper bound: original verts + 3 per extruded face */
  uint32_t max_new_vc = vc + count * 3;
  /* Upper bound: original indices + 18 per extruded face (3 quads * 2 tris * 3
   * indices) */
  uint32_t max_new_ic = ic + count * 18;

  MopVertex *new_verts = (MopVertex *)malloc(max_new_vc * sizeof(MopVertex));
  uint32_t *new_indices = (uint32_t *)malloc(max_new_ic * sizeof(uint32_t));
  if (!new_verts || !new_indices) {
    free(extrude_flag);
    free(new_verts);
    free(new_indices);
    return;
  }

  /* Copy original vertices */
  memcpy(new_verts, src, vc * sizeof(MopVertex));
  uint32_t cur_vc = vc;

  /* Copy original indices */
  memcpy(new_indices, idx_src, ic * sizeof(uint32_t));
  uint32_t cur_ic = ic;

  for (uint32_t f = 0; f < face_count; f++) {
    if (!extrude_flag[f])
      continue;

    uint32_t i0 = idx_src[f * 3 + 0];
    uint32_t i1 = idx_src[f * 3 + 1];
    uint32_t i2 = idx_src[f * 3 + 2];

    /* Compute face normal */
    MopVec3 p0 = src[i0].position;
    MopVec3 p1 = src[i1].position;
    MopVec3 p2 = src[i2].position;

    MopVec3 e1 = mop_vec3_sub(p1, p0);
    MopVec3 e2 = mop_vec3_sub(p2, p0);
    MopVec3 fn = mop_vec3_normalize(mop_vec3_cross(e1, e2));
    MopVec3 offset = mop_vec3_scale(fn, distance);

    /* Duplicate the 3 vertices, offset by normal * distance */
    uint32_t ni0 = cur_vc;
    uint32_t ni1 = cur_vc + 1;
    uint32_t ni2 = cur_vc + 2;

    new_verts[ni0] = src[i0];
    new_verts[ni0].position = mop_vec3_add(src[i0].position, offset);
    new_verts[ni1] = src[i1];
    new_verts[ni1].position = mop_vec3_add(src[i1].position, offset);
    new_verts[ni2] = src[i2];
    new_verts[ni2].position = mop_vec3_add(src[i2].position, offset);
    cur_vc += 3;

    /* Replace the original face to use new (extruded) vertices */
    new_indices[f * 3 + 0] = ni0;
    new_indices[f * 3 + 1] = ni1;
    new_indices[f * 3 + 2] = ni2;

    /* Create 3 side-wall quads (each as 2 triangles).
     * Edge 0: i0-i1 to ni0-ni1
     * Edge 1: i1-i2 to ni1-ni2
     * Edge 2: i2-i0 to ni2-ni0 */
    uint32_t orig[3] = {i0, i1, i2};
    uint32_t extruded[3] = {ni0, ni1, ni2};

    for (int e = 0; e < 3; e++) {
      uint32_t a = orig[e];
      uint32_t b = orig[(e + 1) % 3];
      uint32_t na = extruded[e];
      uint32_t nb = extruded[(e + 1) % 3];

      /* Quad: a, b, nb, na — as 2 triangles */
      new_indices[cur_ic + 0] = a;
      new_indices[cur_ic + 1] = b;
      new_indices[cur_ic + 2] = nb;
      cur_ic += 3;

      new_indices[cur_ic + 0] = a;
      new_indices[cur_ic + 1] = nb;
      new_indices[cur_ic + 2] = na;
      cur_ic += 3;
    }
  }

  mop_mesh_update_geometry(mesh, vp, new_verts, cur_vc, new_indices, cur_ic);

  free(extrude_flag);
  free(new_verts);
  free(new_indices);
}

void mop_mesh_inset_faces(MopMesh *mesh, MopViewport *vp,
                          const uint32_t *face_indices, uint32_t count,
                          float inset) {
  if (!mesh || !vp || !face_indices || count == 0)
    return;
  if (!mesh->vertex_buffer || !mesh->index_buffer)
    return;

  const MopVertex *src =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  const uint32_t *idx_src =
      (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
  if (!src || !idx_src)
    return;

  uint32_t vc = mesh->vertex_count;
  uint32_t ic = mesh->index_count;
  uint32_t face_count = ic / 3;

  /* For each inset face: create 3 new vertices (inset toward centroid),
   * replace original face with inset face, add 3 connecting quads */

  /* Upper bound allocations */
  uint32_t max_new_vc = vc + count * 3;
  uint32_t max_new_ic = ic + count * 18;

  MopVertex *new_verts = (MopVertex *)malloc(max_new_vc * sizeof(MopVertex));
  uint32_t *new_indices = (uint32_t *)malloc(max_new_ic * sizeof(uint32_t));
  if (!new_verts || !new_indices) {
    free(new_verts);
    free(new_indices);
    return;
  }

  memcpy(new_verts, src, vc * sizeof(MopVertex));
  memcpy(new_indices, idx_src, ic * sizeof(uint32_t));

  uint32_t cur_vc = vc;
  uint32_t cur_ic = ic;

  for (uint32_t fi = 0; fi < count; fi++) {
    uint32_t f = face_indices[fi];
    if (f >= face_count)
      continue;

    uint32_t i0 = idx_src[f * 3 + 0];
    uint32_t i1 = idx_src[f * 3 + 1];
    uint32_t i2 = idx_src[f * 3 + 2];

    /* Compute centroid */
    MopVec3 centroid = {
        (src[i0].position.x + src[i1].position.x + src[i2].position.x) / 3.0f,
        (src[i0].position.y + src[i1].position.y + src[i2].position.y) / 3.0f,
        (src[i0].position.z + src[i1].position.z + src[i2].position.z) / 3.0f,
    };

    /* Create inset vertices: lerp from original toward centroid by inset
     * factor */
    float t = inset;
    if (t < 0.0f)
      t = 0.0f;
    if (t > 1.0f)
      t = 1.0f;

    uint32_t ni0 = cur_vc;
    uint32_t ni1 = cur_vc + 1;
    uint32_t ni2 = cur_vc + 2;

    uint32_t orig_verts[3] = {i0, i1, i2};
    for (int k = 0; k < 3; k++) {
      uint32_t oi = orig_verts[k];
      new_verts[cur_vc + (uint32_t)k] = src[oi];
      new_verts[cur_vc + (uint32_t)k].position.x =
          src[oi].position.x + (centroid.x - src[oi].position.x) * t;
      new_verts[cur_vc + (uint32_t)k].position.y =
          src[oi].position.y + (centroid.y - src[oi].position.y) * t;
      new_verts[cur_vc + (uint32_t)k].position.z =
          src[oi].position.z + (centroid.z - src[oi].position.z) * t;
    }
    cur_vc += 3;

    /* Replace the original face with the inset face */
    new_indices[f * 3 + 0] = ni0;
    new_indices[f * 3 + 1] = ni1;
    new_indices[f * 3 + 2] = ni2;

    /* Create 3 connecting quads */
    uint32_t orig[3] = {i0, i1, i2};
    uint32_t inset_v[3] = {ni0, ni1, ni2};

    for (int e = 0; e < 3; e++) {
      uint32_t a = orig[e];
      uint32_t b = orig[(e + 1) % 3];
      uint32_t na = inset_v[e];
      uint32_t nb = inset_v[(e + 1) % 3];

      new_indices[cur_ic + 0] = a;
      new_indices[cur_ic + 1] = b;
      new_indices[cur_ic + 2] = nb;
      cur_ic += 3;

      new_indices[cur_ic + 0] = a;
      new_indices[cur_ic + 1] = nb;
      new_indices[cur_ic + 2] = na;
      cur_ic += 3;
    }
  }

  mop_mesh_update_geometry(mesh, vp, new_verts, cur_vc, new_indices, cur_ic);

  free(new_verts);
  free(new_indices);
}

void mop_mesh_delete_faces(MopMesh *mesh, MopViewport *vp,
                           const uint32_t *face_indices, uint32_t count) {
  if (!mesh || !vp || !face_indices || count == 0)
    return;
  if (!mesh->vertex_buffer || !mesh->index_buffer)
    return;

  const MopVertex *src =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  const uint32_t *idx_src =
      (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
  if (!src || !idx_src)
    return;

  uint32_t vc = mesh->vertex_count;
  uint32_t ic = mesh->index_count;
  uint32_t face_count = ic / 3;

  /* Mark faces for deletion */
  bool *del = (bool *)calloc(face_count, sizeof(bool));
  if (!del)
    return;

  for (uint32_t i = 0; i < count; i++) {
    if (face_indices[i] < face_count)
      del[face_indices[i]] = true;
  }

  /* Build new index array without deleted faces */
  uint32_t *new_indices = (uint32_t *)malloc(ic * sizeof(uint32_t));
  if (!new_indices) {
    free(del);
    return;
  }

  uint32_t new_ic = 0;
  for (uint32_t f = 0; f < face_count; f++) {
    if (del[f])
      continue;
    new_indices[new_ic + 0] = idx_src[f * 3 + 0];
    new_indices[new_ic + 1] = idx_src[f * 3 + 1];
    new_indices[new_ic + 2] = idx_src[f * 3 + 2];
    new_ic += 3;
  }

  /* Find referenced vertices */
  bool *referenced = (bool *)calloc(vc, sizeof(bool));
  if (!referenced) {
    free(del);
    free(new_indices);
    return;
  }

  for (uint32_t i = 0; i < new_ic; i++) {
    if (new_indices[i] < vc)
      referenced[new_indices[i]] = true;
  }

  /* Build vertex remap */
  uint32_t *remap = (uint32_t *)malloc(vc * sizeof(uint32_t));
  if (!remap) {
    free(del);
    free(new_indices);
    free(referenced);
    return;
  }

  uint32_t new_vc = 0;
  for (uint32_t i = 0; i < vc; i++) {
    if (referenced[i]) {
      remap[i] = new_vc;
      new_vc++;
    } else {
      remap[i] = UINT32_MAX;
    }
  }

  /* Build compacted vertex array */
  MopVertex *new_verts = (MopVertex *)malloc(new_vc * sizeof(MopVertex));
  if (!new_verts) {
    free(del);
    free(new_indices);
    free(referenced);
    free(remap);
    return;
  }

  for (uint32_t i = 0; i < vc; i++) {
    if (referenced[i])
      new_verts[remap[i]] = src[i];
  }

  /* Remap indices */
  for (uint32_t i = 0; i < new_ic; i++) {
    new_indices[i] = remap[new_indices[i]];
  }

  if (new_vc > 0 && new_ic > 0) {
    mop_mesh_update_geometry(mesh, vp, new_verts, new_vc, new_indices, new_ic);
  }

  free(del);
  free(new_indices);
  free(referenced);
  free(remap);
  free(new_verts);
}

void mop_mesh_flip_normals(MopMesh *mesh, MopViewport *vp,
                           const uint32_t *face_indices, uint32_t count) {
  if (!mesh || !vp || !face_indices || count == 0)
    return;
  if (!mesh->vertex_buffer || !mesh->index_buffer)
    return;

  const MopVertex *src =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  const uint32_t *idx_src =
      (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
  if (!src || !idx_src)
    return;

  uint32_t vc = mesh->vertex_count;
  uint32_t ic = mesh->index_count;
  uint32_t face_count = ic / 3;

  MopVertex *new_verts = (MopVertex *)malloc(vc * sizeof(MopVertex));
  uint32_t *new_indices = (uint32_t *)malloc(ic * sizeof(uint32_t));
  if (!new_verts || !new_indices) {
    free(new_verts);
    free(new_indices);
    return;
  }

  memcpy(new_verts, src, vc * sizeof(MopVertex));
  memcpy(new_indices, idx_src, ic * sizeof(uint32_t));

  for (uint32_t fi = 0; fi < count; fi++) {
    uint32_t f = face_indices[fi];
    if (f >= face_count)
      continue;

    /* Reverse winding order: swap indices 1 and 2 */
    uint32_t tmp = new_indices[f * 3 + 1];
    new_indices[f * 3 + 1] = new_indices[f * 3 + 2];
    new_indices[f * 3 + 2] = tmp;

    /* Negate normals of the face's vertices */
    uint32_t i0 = new_indices[f * 3 + 0];
    uint32_t i1 = new_indices[f * 3 + 1];
    uint32_t i2 = new_indices[f * 3 + 2];

    if (i0 < vc) {
      new_verts[i0].normal.x = -new_verts[i0].normal.x;
      new_verts[i0].normal.y = -new_verts[i0].normal.y;
      new_verts[i0].normal.z = -new_verts[i0].normal.z;
    }
    if (i1 < vc) {
      new_verts[i1].normal.x = -new_verts[i1].normal.x;
      new_verts[i1].normal.y = -new_verts[i1].normal.y;
      new_verts[i1].normal.z = -new_verts[i1].normal.z;
    }
    if (i2 < vc) {
      new_verts[i2].normal.x = -new_verts[i2].normal.x;
      new_verts[i2].normal.y = -new_verts[i2].normal.y;
      new_verts[i2].normal.z = -new_verts[i2].normal.z;
    }
  }

  mop_mesh_update_geometry(mesh, vp, new_verts, vc, new_indices, ic);

  free(new_verts);
  free(new_indices);
}
