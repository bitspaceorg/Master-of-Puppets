/*
 * Master of Puppets — Scene Snapshot
 * snapshot.c — Zero-copy scene iteration for raytracers and exporters
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"

#include <math.h>

/* -------------------------------------------------------------------------
 * Filter: same as query.c — active, non-zero object_id, not gizmo
 * ------------------------------------------------------------------------- */

static bool is_scene_mesh(const struct MopMesh *m) {
  return m->active && m->object_id != 0 && m->object_id < 0xFFFF0000u;
}

/* -------------------------------------------------------------------------
 * Snapshot creation
 * ------------------------------------------------------------------------- */

MopSceneSnapshot mop_viewport_snapshot(const MopViewport *vp) {
  MopSceneSnapshot snap = {0};
  if (!vp)
    return snap;

  snap.camera = mop_viewport_get_camera_state(vp);
  snap.width = vp->width;
  snap.height = vp->height;
  snap.lights = vp->lights;
  snap.light_count = vp->light_count;
  snap._vp = vp;
  snap._mesh_idx = 0;
  return snap;
}

/* -------------------------------------------------------------------------
 * Mesh iterator
 * ------------------------------------------------------------------------- */

bool mop_snapshot_next_mesh(MopSceneSnapshot *snap, MopMeshView *out) {
  if (!snap || !out || !snap->_vp)
    return false;

  const MopViewport *vp = snap->_vp;

  while (snap->_mesh_idx < vp->mesh_count) {
    const struct MopMesh *m = &vp->meshes[snap->_mesh_idx];
    snap->_mesh_idx++;

    if (!is_scene_mesh(m))
      continue;

    out->object_id = m->object_id;
    out->vertex_count = m->vertex_count;
    out->index_count = m->index_count;
    out->world_transform = m->world_transform;
    out->opacity = m->opacity;
    out->blend_mode = m->blend_mode;
    out->material = m->has_material ? m->material : mop_material_default();

    /* Zero-copy vertex/index access */
    if (m->vertex_buffer && !m->vertex_format) {
      out->vertices = (const MopVertex *)vp->rhi->buffer_read(m->vertex_buffer);
    } else {
      out->vertices = NULL;
    }
    if (m->index_buffer) {
      out->indices = (const uint32_t *)vp->rhi->buffer_read(m->index_buffer);
    } else {
      out->indices = NULL;
    }

    return true;
  }

  return false;
}

void mop_snapshot_reset(MopSceneSnapshot *snap) {
  if (snap)
    snap->_mesh_idx = 0;
}

uint32_t mop_snapshot_mesh_count(const MopSceneSnapshot *snap) {
  if (!snap || !snap->_vp)
    return 0;
  return mop_viewport_mesh_count(snap->_vp);
}

/* -------------------------------------------------------------------------
 * Triangle count
 * ------------------------------------------------------------------------- */

uint32_t mop_snapshot_triangle_count(const MopSceneSnapshot *snap) {
  if (!snap || !snap->_vp)
    return 0;

  const MopViewport *vp = snap->_vp;
  uint32_t total = 0;
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    const struct MopMesh *m = &vp->meshes[i];
    if (is_scene_mesh(m))
      total += m->index_count / 3;
  }
  return total;
}

/* -------------------------------------------------------------------------
 * Compute the normal matrix: transpose of inverse of upper-left 3x3.
 * For uniform scaling, this is just the upper-left 3x3 itself.
 * We compute the full inverse for correctness with non-uniform scale.
 * ------------------------------------------------------------------------- */

static MopMat4 compute_normal_matrix(MopMat4 world) {
  MopMat4 inv = mop_mat4_inverse(world);
  /* Transpose the inverse — for normals we only need the upper-left 3x3,
   * but we store it in a MopMat4 for convenience with mop_mat4_mul_vec4. */
  MopMat4 r = {0};
#define I(row, col) ((col) * 4 + (row))
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      r.d[I(row, col)] = inv.d[I(col, row)];
    }
  }
  r.d[I(3, 3)] = 1.0f;
#undef I
  return r;
}

/* -------------------------------------------------------------------------
 * Triangle iterator
 * ------------------------------------------------------------------------- */

MopTriangleIter mop_triangle_iter_begin(const MopViewport *vp) {
  MopTriangleIter iter;
  iter._snap = mop_viewport_snapshot(vp);
  iter._tri_idx = 0;
  iter._has_mesh = mop_snapshot_next_mesh(&iter._snap, &iter._current_mesh);
  if (iter._has_mesh) {
    iter._normal_matrix =
        compute_normal_matrix(iter._current_mesh.world_transform);
  }
  return iter;
}

bool mop_triangle_iter_next(MopTriangleIter *iter, MopTriangle *out) {
  if (!iter || !out)
    return false;

  while (iter->_has_mesh) {
    MopMeshView *mv = &iter->_current_mesh;
    uint32_t tri_count = mv->index_count / 3;

    /* If we have triangles left in the current mesh */
    if (iter->_tri_idx < tri_count && mv->vertices && mv->indices) {
      uint32_t base = iter->_tri_idx * 3;
      uint32_t i0 = mv->indices[base + 0];
      uint32_t i1 = mv->indices[base + 1];
      uint32_t i2 = mv->indices[base + 2];

      /* Bounds check */
      if (i0 >= mv->vertex_count || i1 >= mv->vertex_count ||
          i2 >= mv->vertex_count) {
        iter->_tri_idx++;
        continue;
      }

      const MopVertex *v0 = &mv->vertices[i0];
      const MopVertex *v1 = &mv->vertices[i1];
      const MopVertex *v2 = &mv->vertices[i2];

      MopMat4 w = mv->world_transform;
      MopMat4 nm = iter->_normal_matrix;

      /* Transform positions to world space */
      for (int k = 0; k < 3; k++) {
        const MopVertex *v = (k == 0) ? v0 : (k == 1) ? v1 : v2;
        MopVec4 wp = mop_mat4_mul_vec4(
            w, (MopVec4){v->position.x, v->position.y, v->position.z, 1.0f});
        out->p[k] = (MopVec3){wp.x, wp.y, wp.z};

        /* Transform normal (no translation, normalize) */
        MopVec4 wn = mop_mat4_mul_vec4(
            nm, (MopVec4){v->normal.x, v->normal.y, v->normal.z, 0.0f});
        out->n[k] = mop_vec3_normalize((MopVec3){wn.x, wn.y, wn.z});

        out->c[k] = v->color;
        out->uv[k][0] = v->u;
        out->uv[k][1] = v->v;
      }

      out->material = mv->material;
      out->object_id = mv->object_id;

      iter->_tri_idx++;
      return true;
    }

    /* Advance to next mesh */
    iter->_tri_idx = 0;
    iter->_has_mesh =
        mop_snapshot_next_mesh(&iter->_snap, &iter->_current_mesh);
    if (iter->_has_mesh) {
      iter->_normal_matrix =
          compute_normal_matrix(iter->_current_mesh.world_transform);
    }
  }

  return false;
}
