/*
 * Master of Puppets — Sub-Element Selection
 * selection.c — Edit mode management and element selection
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include <mop/mop.h>

#include <math.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Edit mode
 * ------------------------------------------------------------------------- */

void mop_mesh_set_edit_mode(MopMesh *mesh, MopEditMode mode) {
  if (!mesh)
    return;
  MOP_VP_LOCK(mesh->viewport);
  mesh->edit_mode = mode;
  MOP_VP_UNLOCK(mesh->viewport);
}

MopEditMode mop_mesh_get_edit_mode(const MopMesh *mesh) {
  if (!mesh)
    return MOP_EDIT_NONE;
  return mesh->edit_mode;
}

/* -------------------------------------------------------------------------
 * Selection API
 * ------------------------------------------------------------------------- */

const MopSelection *mop_viewport_get_selection(const MopViewport *vp) {
  if (!vp)
    return NULL;
  return &vp->selection;
}

void mop_viewport_select_element(MopViewport *vp, uint32_t element_index) {
  if (!vp)
    return;
  MOP_VP_LOCK(vp);
  MopSelection *sel = &vp->selection;

  /* Check if already selected */
  for (uint32_t i = 0; i < sel->element_count; i++) {
    if (sel->elements[i] == element_index) {
      MOP_VP_UNLOCK(vp);
      return;
    }
  }

  /* Add if not at max */
  if (sel->element_count < sel->element_capacity) {
    sel->elements[sel->element_count] = element_index;
    sel->element_count++;
  }
  MOP_VP_UNLOCK(vp);
}

void mop_viewport_deselect_element(MopViewport *vp, uint32_t element_index) {
  if (!vp)
    return;
  MOP_VP_LOCK(vp);
  MopSelection *sel = &vp->selection;

  for (uint32_t i = 0; i < sel->element_count; i++) {
    if (sel->elements[i] == element_index) {
      /* Swap with last and shrink */
      sel->elements[i] = sel->elements[sel->element_count - 1];
      sel->element_count--;
      break;
    }
  }
  MOP_VP_UNLOCK(vp);
}

void mop_viewport_clear_selection(MopViewport *vp) {
  if (!vp)
    return;
  MOP_VP_LOCK(vp);
  vp->selection.element_count = 0;
  MOP_VP_UNLOCK(vp);
}

void mop_viewport_toggle_element(MopViewport *vp, uint32_t element_index) {
  if (!vp)
    return;
  MOP_VP_LOCK(vp);
  MopSelection *sel = &vp->selection;

  /* Check if present */
  for (uint32_t i = 0; i < sel->element_count; i++) {
    if (sel->elements[i] == element_index) {
      /* Remove: swap with last */
      sel->elements[i] = sel->elements[sel->element_count - 1];
      sel->element_count--;
      MOP_VP_UNLOCK(vp);
      return;
    }
  }

  /* Not present — add */
  if (sel->element_count < sel->element_capacity) {
    sel->elements[sel->element_count] = element_index;
    sel->element_count++;
  }
  MOP_VP_UNLOCK(vp);
}

/* -------------------------------------------------------------------------
 * Multi-object selection
 * ------------------------------------------------------------------------- */

void mop_viewport_select_object(MopViewport *vp, uint32_t id, bool additive) {
  if (!vp || id == 0)
    return;

  MOP_VP_LOCK(vp);
  if (!additive) {
    /* Clear existing selection, then add */
    vp->selected_count = 0;
  }

  /* Check if already selected */
  for (uint32_t i = 0; i < vp->selected_count; i++) {
    if (vp->selected_ids[i] == id) {
      if (additive) {
        /* Toggle: remove it */
        vp->selected_ids[i] = vp->selected_ids[vp->selected_count - 1];
        vp->selected_count--;
        vp->selected_id = vp->selected_count > 0 ? vp->selected_ids[0] : 0;
      }
      MOP_VP_UNLOCK(vp);
      return;
    }
  }

  /* Add to selection */
  if (vp->selected_count < vp->selected_capacity) {
    vp->selected_ids[vp->selected_count] = id;
    vp->selected_count++;
  }

  /* Update backward-compat single-select field */
  vp->selected_id = vp->selected_ids[0];
  MOP_VP_UNLOCK(vp);
}

void mop_viewport_deselect_object(MopViewport *vp, uint32_t id) {
  if (!vp)
    return;
  MOP_VP_LOCK(vp);
  for (uint32_t i = 0; i < vp->selected_count; i++) {
    if (vp->selected_ids[i] == id) {
      vp->selected_ids[i] = vp->selected_ids[vp->selected_count - 1];
      vp->selected_count--;
      vp->selected_id = vp->selected_count > 0 ? vp->selected_ids[0] : 0;
      break;
    }
  }
  MOP_VP_UNLOCK(vp);
}

bool mop_viewport_is_object_selected(const MopViewport *vp, uint32_t id) {
  if (!vp)
    return false;
  for (uint32_t i = 0; i < vp->selected_count; i++) {
    if (vp->selected_ids[i] == id)
      return true;
  }
  return false;
}

uint32_t mop_viewport_get_selected_count(const MopViewport *vp) {
  return vp ? vp->selected_count : 0;
}

/* -------------------------------------------------------------------------
 * Picking helpers
 *
 * These functions are used by the input handler to resolve screen-space
 * clicks into sub-element picks (vertex, edge, face).  They have external
 * linkage so they can be called from src/interact/input.c in future wiring.
 * ------------------------------------------------------------------------- */

/* Decode packed pick buffer value.
 * Upper 16 bits = mesh object_id, lower 16 bits = triangle index.
 * Returns the triangle index, writes mesh id to *out_mesh_id. */
uint32_t mop_selection_decode_face(uint32_t packed_id, uint32_t *out_mesh_id) {
  if (out_mesh_id)
    *out_mesh_id = (packed_id >> 16) & 0xFFFFu;
  return packed_id & 0xFFFFu;
}

/* Project a world-space point to screen coordinates.
 * Returns false if the point is behind the camera. */
bool mop_selection_project_to_screen(const MopMat4 *mvp, MopVec3 world_pos,
                                     int vp_width, int vp_height, float *out_sx,
                                     float *out_sy) {
  MopVec4 clip = mop_mat4_mul_vec4(
      *mvp, (MopVec4){world_pos.x, world_pos.y, world_pos.z, 1.0f});
  if (clip.w <= 0.0f)
    return false;

  float inv_w = 1.0f / clip.w;
  float ndc_x = clip.x * inv_w;
  float ndc_y = clip.y * inv_w;

  *out_sx = (ndc_x * 0.5f + 0.5f) * (float)vp_width;
  *out_sy = (1.0f - (ndc_y * 0.5f + 0.5f)) * (float)vp_height;
  return true;
}

/* Point-to-segment squared distance in 2D screen space. */
float mop_selection_point_seg_dist_sq(float px, float py, float ax, float ay,
                                      float bx, float by) {
  float abx = bx - ax;
  float aby = by - ay;
  float apx = px - ax;
  float apy = py - ay;

  float ab_sq = abx * abx + aby * aby;
  if (ab_sq < 1e-12f)
    return apx * apx + apy * apy;

  float t = (apx * abx + apy * aby) / ab_sq;
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;

  float cx = ax + t * abx - px;
  float cy = ay + t * aby - py;
  return cx * cx + cy * cy;
}

/* Find the nearest vertex to a screen point within a pixel threshold.
 * Returns the vertex index, or UINT32_MAX if none found. */
uint32_t mop_selection_pick_vertex(const MopViewport *vp, const MopMesh *mesh,
                                   float screen_x, float screen_y,
                                   float threshold_px) {
  if (!vp || !mesh || !mesh->vertex_buffer)
    return UINT32_MAX;

  const MopVertex *verts =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  if (!verts)
    return UINT32_MAX;

  MopMat4 mvp = mop_mat4_multiply(
      vp->projection_matrix,
      mop_mat4_multiply(vp->view_matrix, mesh->world_transform));

  float best_dist_sq = threshold_px * threshold_px;
  uint32_t best_idx = UINT32_MAX;

  for (uint32_t i = 0; i < mesh->vertex_count; i++) {
    float sx, sy;
    if (!mop_selection_project_to_screen(&mvp, verts[i].position, vp->width,
                                         vp->height, &sx, &sy))
      continue;

    float dx = sx - screen_x;
    float dy = sy - screen_y;
    float dist_sq = dx * dx + dy * dy;
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best_idx = i;
    }
  }

  return best_idx;
}

/* Find the nearest edge to a screen point within a pixel threshold.
 * Encodes the edge as (lo << 16 | hi) where lo < hi.
 * Returns UINT32_MAX if none found. */
uint32_t mop_selection_pick_edge(const MopViewport *vp, const MopMesh *mesh,
                                 float screen_x, float screen_y,
                                 float threshold_px) {
  if (!vp || !mesh || !mesh->vertex_buffer || !mesh->index_buffer)
    return UINT32_MAX;

  const MopVertex *verts =
      (const MopVertex *)vp->rhi->buffer_read(mesh->vertex_buffer);
  const uint32_t *indices =
      (const uint32_t *)vp->rhi->buffer_read(mesh->index_buffer);
  if (!verts || !indices)
    return UINT32_MAX;

  MopMat4 mvp = mop_mat4_multiply(
      vp->projection_matrix,
      mop_mat4_multiply(vp->view_matrix, mesh->world_transform));

  float best_dist_sq = threshold_px * threshold_px;
  uint32_t best_edge = UINT32_MAX;

  uint32_t tri_count = mesh->index_count / 3;
  for (uint32_t t = 0; t < tri_count; t++) {
    uint32_t i0 = indices[t * 3 + 0];
    uint32_t i1 = indices[t * 3 + 1];
    uint32_t i2 = indices[t * 3 + 2];

    /* 3 edges per triangle */
    uint32_t edge_pairs[6] = {i0, i1, i1, i2, i2, i0};
    for (int e = 0; e < 3; e++) {
      uint32_t ea = edge_pairs[e * 2 + 0];
      uint32_t eb = edge_pairs[e * 2 + 1];

      float sax, say, sbx, sby;
      if (!mop_selection_project_to_screen(&mvp, verts[ea].position, vp->width,
                                           vp->height, &sax, &say))
        continue;
      if (!mop_selection_project_to_screen(&mvp, verts[eb].position, vp->width,
                                           vp->height, &sbx, &sby))
        continue;

      float d = mop_selection_point_seg_dist_sq(screen_x, screen_y, sax, say,
                                                sbx, sby);
      if (d < best_dist_sq) {
        best_dist_sq = d;
        /* Encode edge: smaller index in upper bits for canonical ordering */
        uint32_t lo = ea < eb ? ea : eb;
        uint32_t hi = ea < eb ? eb : ea;
        best_edge = (lo << 16) | hi;
      }
    }
  }

  return best_edge;
}
