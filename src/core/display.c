/*
 * Master of Puppets — Display Settings
 * display.c — Default construction and viewport get/set
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"

MopDisplaySettings mop_display_settings_default(void) {
  MopDisplaySettings ds;
  ds.wireframe_overlay = false;
  ds.wireframe_color = (MopColor){1.0f, 0.6f, 0.2f, 1.0f};
  ds.wireframe_opacity = 0.15f;
  ds.show_normals = false;
  ds.normal_display_length = 0.1f;
  ds.show_bounds = false;
  ds.show_vertices = false;
  ds.vertex_display_size = 3.0f;
  ds.vertex_map_mode = MOP_VTXMAP_NONE;
  ds.vertex_map_channel = 0;
  return ds;
}

void mop_viewport_set_display(MopViewport *vp, const MopDisplaySettings *ds) {
  if (!vp || !ds)
    return;
  vp->display = *ds;
}

MopDisplaySettings mop_viewport_get_display(const MopViewport *vp) {
  if (!vp)
    return mop_display_settings_default();
  return vp->display;
}

/* -------------------------------------------------------------------------
 * Vertex map color computation
 *
 * Given a vertex map mode and a face index, compute a display color.
 * Returns true if a color was computed (mode != NONE), false otherwise.
 * ------------------------------------------------------------------------- */

bool mop_display_vertex_map_color(MopVertexMapDisplay mode, uint32_t face_index,
                                  const MopVertex *vert, float *out_r,
                                  float *out_g, float *out_b) {
  switch (mode) {
  case MOP_VTXMAP_NONE:
    return false;
  case MOP_VTXMAP_UV:
    if (vert) {
      *out_r = vert->u;
      *out_g = vert->v;
      *out_b = 0.0f;
    }
    return true;
  case MOP_VTXMAP_WEIGHTS:
    /* Placeholder — bone weights not yet stored in MopVertex */
    *out_r = 0.5f;
    *out_g = 0.5f;
    *out_b = 0.5f;
    return true;
  case MOP_VTXMAP_NORMALS:
    if (vert) {
      *out_r = vert->normal.x * 0.5f + 0.5f;
      *out_g = vert->normal.y * 0.5f + 0.5f;
      *out_b = vert->normal.z * 0.5f + 0.5f;
    }
    return true;
  case MOP_VTXMAP_CUSTOM:
    *out_r = 1.0f;
    *out_g = 0.0f;
    *out_b = 1.0f;
    return true;
  case MOP_VTXMAP_FACE_ID: {
    uint32_t hash = face_index * 2654435761u;
    *out_r = ((hash >> 0) & 0xFF) / 255.0f;
    *out_g = ((hash >> 8) & 0xFF) / 255.0f;
    *out_b = ((hash >> 16) & 0xFF) / 255.0f;
    return true;
  }
  default:
    return false;
  }
}
