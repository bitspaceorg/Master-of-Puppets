/*
 * Master of Puppets — Built-in Overlay Implementations
 * overlay_builtin.c — Wireframe-on-shaded, normals, bounds, selection,
 *                      analytical grid, 2D light indicators
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include "rhi/rhi.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * RHI buffer data accessor
 *
 * The overlay code needs to read vertex data from RHI buffers.  Since
 * MopRhiBuffer is opaque per-backend, we call through the backend's
 * buffer_read function pointer.  CPU returns buf->data, Vulkan returns
 * buf->shadow.
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Wireframe-on-shaded overlay
 *
 * For each active scene mesh, re-issue the draw call with wireframe=true,
 * using the overlay wireframe color and reduced opacity (alpha blend).
 * The wireframe is drawn on top of the shaded surface.
 * ------------------------------------------------------------------------- */

void mop_overlay_builtin_wireframe(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp)
    return;

  MopColor wf_color = vp->display.wireframe_color;
  float wf_opacity = vp->display.wireframe_opacity;

  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *m = &vp->meshes[i];
    if (!m->active)
      continue;
    if (m->object_id == 0)
      continue; /* skip grid/bg */
    if (m->object_id >= 0xFFFE0000u)
      continue; /* skip gizmo */

    MopMat4 mvp = mop_mat4_multiply(
        vp->projection_matrix,
        mop_mat4_multiply(vp->view_matrix, m->world_transform));

    MopRhiDrawCall call = {
        .vertex_buffer = m->vertex_buffer,
        .index_buffer = m->index_buffer,
        .vertex_count = m->vertex_count,
        .index_count = m->index_count,
        .object_id = 0, /* don't write to pick buffer */
        .model = m->world_transform,
        .view = vp->view_matrix,
        .projection = vp->projection_matrix,
        .mvp = mvp,
        .base_color = wf_color,
        .opacity = wf_opacity,
        .light_dir = vp->light_dir,
        .ambient = 1.0f, /* unlit wireframe */
        .shading_mode = MOP_SHADING_FLAT,
        .wireframe = true,
        .depth_test = true,
        .backface_cull = false,
        .texture = NULL,
        .blend_mode = MOP_BLEND_ALPHA,
        .metallic = 0.0f,
        .roughness = 0.5f,
        .emissive = (MopVec3){0, 0, 0},
        .lights = NULL,
        .light_count = 0,
        .vertex_format = NULL,
    };
    vp->rhi->draw(vp->device, vp->framebuffer, &call);
  }
}

/* -------------------------------------------------------------------------
 * Vertex normals overlay
 *
 * For each active scene mesh, read vertex positions and normals from the
 * buffer, generate line geometry (position -> position + normal * length),
 * and draw as wireframe lines colored by normal direction (RGB = XYZ).
 * ------------------------------------------------------------------------- */

void mop_overlay_builtin_normals(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp)
    return;

  float length = vp->display.normal_display_length;

  for (uint32_t mi = 0; mi < vp->mesh_count; mi++) {
    struct MopMesh *m = &vp->meshes[mi];
    if (!m->active)
      continue;
    if (m->object_id == 0)
      continue;
    if (m->object_id >= 0xFFFE0000u)
      continue;

    const MopVertex *verts =
        (const MopVertex *)vp->rhi->buffer_read(m->vertex_buffer);
    uint32_t vc = m->vertex_count;
    if (vc == 0)
      continue;

    /* Generate line geometry: 2 vertices per normal line, 2 indices */
    uint32_t line_vc = vc * 2;
    uint32_t line_ic = vc * 2;
    MopVertex *line_v = malloc(line_vc * sizeof(MopVertex));
    uint32_t *line_i = malloc(line_ic * sizeof(uint32_t));
    if (!line_v || !line_i) {
      free(line_v);
      free(line_i);
      continue;
    }

    for (uint32_t j = 0; j < vc; j++) {
      MopVec3 p = verts[j].position;
      MopVec3 n = verts[j].normal;
      /* Color = normal direction mapped to [0,1] */
      MopColor nc = {fabsf(n.x), fabsf(n.y), fabsf(n.z), 1.0f};
      MopVec3 tip = {p.x + n.x * length, p.y + n.y * length,
                     p.z + n.z * length};
      line_v[j * 2 + 0] = (MopVertex){p, n, nc, 0, 0};
      line_v[j * 2 + 1] = (MopVertex){tip, n, nc, 0, 0};
      line_i[j * 2 + 0] = j * 2;
      line_i[j * 2 + 1] = j * 2 + 1;
    }

    /* Create temp buffers */
    MopRhiBufferDesc vb_desc = {.data = line_v,
                                .size = line_vc * sizeof(MopVertex)};
    MopRhiBufferDesc ib_desc = {.data = line_i,
                                .size = line_ic * sizeof(uint32_t)};
    MopRhiBuffer *vb = vp->rhi->buffer_create(vp->device, &vb_desc);
    MopRhiBuffer *ib = vp->rhi->buffer_create(vp->device, &ib_desc);
    free(line_v);
    free(line_i);

    if (!vb || !ib) {
      if (vb)
        vp->rhi->buffer_destroy(vp->device, vb);
      if (ib)
        vp->rhi->buffer_destroy(vp->device, ib);
      continue;
    }

    MopMat4 mvp = mop_mat4_multiply(
        vp->projection_matrix,
        mop_mat4_multiply(vp->view_matrix, m->world_transform));

    MopRhiDrawCall call = {
        .vertex_buffer = vb,
        .index_buffer = ib,
        .vertex_count = line_vc,
        .index_count = line_ic,
        .object_id = 0,
        .model = m->world_transform,
        .view = vp->view_matrix,
        .projection = vp->projection_matrix,
        .mvp = mvp,
        .base_color = (MopColor){1, 1, 1, 1},
        .opacity = 1.0f,
        .light_dir = vp->light_dir,
        .ambient = 1.0f,
        .shading_mode = MOP_SHADING_FLAT,
        .wireframe = true,
        .depth_test = true,
        .backface_cull = false,
        .texture = NULL,
        .blend_mode = MOP_BLEND_OPAQUE,
        .metallic = 0.0f,
        .roughness = 0.5f,
        .emissive = (MopVec3){0, 0, 0},
        .lights = NULL,
        .light_count = 0,
        .vertex_format = NULL,
    };
    vp->rhi->draw(vp->device, vp->framebuffer, &call);

    vp->rhi->buffer_destroy(vp->device, vb);
    vp->rhi->buffer_destroy(vp->device, ib);
  }
}

/* -------------------------------------------------------------------------
 * Bounding box overlay
 *
 * For each active scene mesh, compute the AABB from the vertex buffer
 * (in local space), transform to world, and draw a 12-line wireframe box.
 * ------------------------------------------------------------------------- */

void mop_overlay_builtin_bounds(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp)
    return;

  for (uint32_t mi = 0; mi < vp->mesh_count; mi++) {
    struct MopMesh *m = &vp->meshes[mi];
    if (!m->active)
      continue;
    if (m->object_id == 0)
      continue;
    if (m->object_id >= 0xFFFE0000u)
      continue;

    const MopVertex *verts =
        (const MopVertex *)vp->rhi->buffer_read(m->vertex_buffer);
    uint32_t vc = m->vertex_count;
    if (vc == 0)
      continue;

    /* Compute local-space AABB */
    MopVec3 bmin = verts[0].position;
    MopVec3 bmax = verts[0].position;
    for (uint32_t j = 1; j < vc; j++) {
      MopVec3 p = verts[j].position;
      if (p.x < bmin.x)
        bmin.x = p.x;
      if (p.y < bmin.y)
        bmin.y = p.y;
      if (p.z < bmin.z)
        bmin.z = p.z;
      if (p.x > bmax.x)
        bmax.x = p.x;
      if (p.y > bmax.y)
        bmax.y = p.y;
      if (p.z > bmax.z)
        bmax.z = p.z;
    }

    /* 8 corners of AABB */
    MopVec3 corners[8] = {
        {bmin.x, bmin.y, bmin.z}, {bmax.x, bmin.y, bmin.z},
        {bmax.x, bmax.y, bmin.z}, {bmin.x, bmax.y, bmin.z},
        {bmin.x, bmin.y, bmax.z}, {bmax.x, bmin.y, bmax.z},
        {bmax.x, bmax.y, bmax.z}, {bmin.x, bmax.y, bmax.z},
    };

    /* 12 edges as index pairs */
    static const uint32_t edges[24] = {
        0, 1, 1, 2, 2, 3, 3, 0, /* bottom face */
        4, 5, 5, 6, 6, 7, 7, 4, /* top face */
        0, 4, 1, 5, 2, 6, 3, 7, /* verticals */
    };

    MopColor box_color = vp->theme.bounds_color;
    MopVec3 n_up = {0, 1, 0};

    MopVertex box_v[8];
    for (int j = 0; j < 8; j++) {
      box_v[j] = (MopVertex){corners[j], n_up, box_color, 0, 0};
    }

    MopRhiBufferDesc vb_desc = {.data = box_v, .size = sizeof(box_v)};
    MopRhiBufferDesc ib_desc = {.data = edges, .size = sizeof(edges)};
    MopRhiBuffer *vb = vp->rhi->buffer_create(vp->device, &vb_desc);
    MopRhiBuffer *ib = vp->rhi->buffer_create(vp->device, &ib_desc);
    if (!vb || !ib) {
      if (vb)
        vp->rhi->buffer_destroy(vp->device, vb);
      if (ib)
        vp->rhi->buffer_destroy(vp->device, ib);
      continue;
    }

    MopMat4 mvp = mop_mat4_multiply(
        vp->projection_matrix,
        mop_mat4_multiply(vp->view_matrix, m->world_transform));

    MopRhiDrawCall call = {
        .vertex_buffer = vb,
        .index_buffer = ib,
        .vertex_count = 8,
        .index_count = 24,
        .object_id = 0,
        .model = m->world_transform,
        .view = vp->view_matrix,
        .projection = vp->projection_matrix,
        .mvp = mvp,
        .base_color = box_color,
        .opacity = 1.0f,
        .light_dir = vp->light_dir,
        .ambient = 1.0f,
        .shading_mode = MOP_SHADING_FLAT,
        .wireframe = true,
        .depth_test = true,
        .backface_cull = false,
        .texture = NULL,
        .blend_mode = MOP_BLEND_OPAQUE,
        .metallic = 0.0f,
        .roughness = 0.5f,
        .emissive = (MopVec3){0, 0, 0},
        .lights = NULL,
        .light_count = 0,
        .vertex_format = NULL,
    };
    vp->rhi->draw(vp->device, vp->framebuffer, &call);

    vp->rhi->buffer_destroy(vp->device, vb);
    vp->rhi->buffer_destroy(vp->device, ib);
  }
}

/* -------------------------------------------------------------------------
 * Selection highlight overlay
 *
 * If a mesh is selected (viewport->selected_id matches mesh->object_id),
 * redraw it with additive blend at a highlight color.
 * ------------------------------------------------------------------------- */

void mop_overlay_builtin_selection(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp || vp->selected_id == 0)
    return;

  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *m = &vp->meshes[i];
    if (!m->active)
      continue;
    if (m->object_id != vp->selected_id)
      continue;

    MopMat4 mvp = mop_mat4_multiply(
        vp->projection_matrix,
        mop_mat4_multiply(vp->view_matrix, m->world_transform));

    MopRhiDrawCall call = {
        .vertex_buffer = m->vertex_buffer,
        .index_buffer = m->index_buffer,
        .vertex_count = m->vertex_count,
        .index_count = m->index_count,
        .object_id = 0,
        .model = m->world_transform,
        .view = vp->view_matrix,
        .projection = vp->projection_matrix,
        .mvp = mvp,
        .base_color = vp->theme.selection_outline,
        .opacity = vp->theme.face_select_opacity,
        .light_dir = vp->light_dir,
        .ambient = 1.0f,
        .shading_mode = MOP_SHADING_FLAT,
        .wireframe = false,
        .depth_test = true,
        .backface_cull = false,
        .texture = NULL,
        .blend_mode = MOP_BLEND_ALPHA,
        .metallic = 0.0f,
        .roughness = 0.5f,
        .emissive = (MopVec3){0, 0, 0},
        .lights = NULL,
        .light_count = 0,
        .vertex_format = NULL,
    };
    vp->rhi->draw(vp->device, vp->framebuffer, &call);
  }
}

/* -------------------------------------------------------------------------
 * Helper: check if an element index is in the current selection
 * ------------------------------------------------------------------------- */

static bool is_element_selected(const MopSelection *sel, uint32_t index) {
  for (uint32_t i = 0; i < sel->element_count; i++) {
    if (sel->elements[i] == index)
      return true;
  }
  return false;
}

/* -------------------------------------------------------------------------
 * Helper: find the mesh being edited (matching selection.mesh_object_id)
 * ------------------------------------------------------------------------- */

static struct MopMesh *find_edit_mesh(MopViewport *vp) {
  uint32_t oid = vp->selection.mesh_object_id;
  if (oid == 0)
    return NULL;
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    struct MopMesh *m = &vp->meshes[i];
    if (m->active && m->object_id == oid)
      return m;
  }
  return NULL;
}

/* -------------------------------------------------------------------------
 * Vertex select overlay (Phase 3K)
 *
 * In vertex edit mode, draw small filled dots at all mesh vertices.
 * Selected vertices are highlighted with the theme's vertex_select_color.
 * Unselected vertices are drawn in a neutral grey.
 * ------------------------------------------------------------------------- */

void mop_overlay_edit_vertices(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp)
    return;
  if (vp->selection.mode != MOP_EDIT_VERTEX)
    return;

  struct MopMesh *m = find_edit_mesh(vp);
  if (!m || !m->vertex_buffer)
    return;

  const MopVertex *verts =
      (const MopVertex *)vp->rhi->buffer_read(m->vertex_buffer);
  uint32_t vc = m->vertex_count;
  if (!verts || vc == 0)
    return;

  MopColor sel_color = vp->theme.vertex_select_color;
  MopColor unsorted_color = {0.6f, 0.6f, 0.6f, 1.0f};
  float dot_size = vp->theme.vertex_select_size;
  if (dot_size < 1.0f)
    dot_size = 3.0f;

  /* For each vertex, generate a small screen-space cross (4 line segments)
   * to approximate a filled dot.  We use 2 perpendicular lines per vertex. */
  uint32_t line_vc = vc * 2; /* 2 verts per point (degenerate line) */
  uint32_t line_ic = vc * 2;
  MopVertex *line_v = (MopVertex *)malloc(line_vc * sizeof(MopVertex));
  uint32_t *line_i = (uint32_t *)malloc(line_ic * sizeof(uint32_t));
  if (!line_v || !line_i) {
    free(line_v);
    free(line_i);
    return;
  }

  MopVec3 n_up = {0, 1, 0};
  (void)dot_size; /* dot size is a visual hint; wireframe lines approximate */

  for (uint32_t j = 0; j < vc; j++) {
    MopColor c =
        is_element_selected(&vp->selection, j) ? sel_color : unsorted_color;
    MopVec3 p = verts[j].position;
    /* Degenerate line (point) at the vertex position */
    line_v[j * 2 + 0] = (MopVertex){p, n_up, c, 0, 0};
    line_v[j * 2 + 1] = (MopVertex){p, n_up, c, 0, 0};
    line_i[j * 2 + 0] = j * 2;
    line_i[j * 2 + 1] = j * 2 + 1;
  }

  MopRhiBufferDesc vb_desc = {.data = line_v,
                              .size = line_vc * sizeof(MopVertex)};
  MopRhiBufferDesc ib_desc = {.data = line_i,
                              .size = line_ic * sizeof(uint32_t)};
  MopRhiBuffer *vb = vp->rhi->buffer_create(vp->device, &vb_desc);
  MopRhiBuffer *ib = vp->rhi->buffer_create(vp->device, &ib_desc);
  free(line_v);
  free(line_i);

  if (!vb || !ib) {
    if (vb)
      vp->rhi->buffer_destroy(vp->device, vb);
    if (ib)
      vp->rhi->buffer_destroy(vp->device, ib);
    return;
  }

  MopMat4 mvp =
      mop_mat4_multiply(vp->projection_matrix,
                        mop_mat4_multiply(vp->view_matrix, m->world_transform));

  MopRhiDrawCall call = {
      .vertex_buffer = vb,
      .index_buffer = ib,
      .vertex_count = line_vc,
      .index_count = line_ic,
      .object_id = 0,
      .model = m->world_transform,
      .view = vp->view_matrix,
      .projection = vp->projection_matrix,
      .mvp = mvp,
      .base_color = (MopColor){1, 1, 1, 1},
      .opacity = 1.0f,
      .light_dir = vp->light_dir,
      .ambient = 1.0f,
      .shading_mode = MOP_SHADING_FLAT,
      .wireframe = true,
      .depth_test = false, /* draw on top */
      .backface_cull = false,
      .texture = NULL,
      .blend_mode = MOP_BLEND_OPAQUE,
      .metallic = 0.0f,
      .roughness = 0.5f,
      .emissive = (MopVec3){0, 0, 0},
      .lights = NULL,
      .light_count = 0,
      .vertex_format = NULL,
  };
  vp->rhi->draw(vp->device, vp->framebuffer, &call);

  vp->rhi->buffer_destroy(vp->device, vb);
  vp->rhi->buffer_destroy(vp->device, ib);
}

/* -------------------------------------------------------------------------
 * Edge select overlay (Phase 3K)
 *
 * In edge edit mode, draw all edges of the mesh as wireframe lines.
 * Selected edges are highlighted with the theme's edge_select_color.
 * ------------------------------------------------------------------------- */

void mop_overlay_edit_edges(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp)
    return;
  if (vp->selection.mode != MOP_EDIT_EDGE)
    return;

  struct MopMesh *m = find_edit_mesh(vp);
  if (!m || !m->vertex_buffer || !m->index_buffer)
    return;

  const MopVertex *verts =
      (const MopVertex *)vp->rhi->buffer_read(m->vertex_buffer);
  const uint32_t *indices =
      (const uint32_t *)vp->rhi->buffer_read(m->index_buffer);
  if (!verts || !indices)
    return;

  uint32_t ic = m->index_count;
  uint32_t face_count = ic / 3;
  if (face_count == 0)
    return;

  MopColor sel_color = vp->theme.edge_select_color;
  MopColor default_color = {0.4f, 0.4f, 0.4f, 1.0f};
  MopVec3 n_up = {0, 1, 0};

  /* Each face has 3 edges, but edges shared between faces are duplicated.
   * For overlay drawing, some duplication is acceptable. */
  uint32_t max_edge_lines = face_count * 3;
  uint32_t line_vc = max_edge_lines * 2;
  uint32_t line_ic = max_edge_lines * 2;

  MopVertex *line_v = (MopVertex *)malloc(line_vc * sizeof(MopVertex));
  uint32_t *line_i = (uint32_t *)malloc(line_ic * sizeof(uint32_t));
  if (!line_v || !line_i) {
    free(line_v);
    free(line_i);
    return;
  }

  uint32_t out_idx = 0;
  for (uint32_t f = 0; f < face_count; f++) {
    uint32_t tri[3] = {indices[f * 3 + 0], indices[f * 3 + 1],
                       indices[f * 3 + 2]};

    for (int e = 0; e < 3; e++) {
      uint32_t ea = tri[e];
      uint32_t eb = tri[(e + 1) % 3];

      /* Canonical edge encoding: smaller index in upper 16 bits */
      uint32_t lo = ea < eb ? ea : eb;
      uint32_t hi = ea < eb ? eb : ea;
      uint32_t edge_id = (lo << 16) | hi;

      MopColor c = is_element_selected(&vp->selection, edge_id) ? sel_color
                                                                : default_color;

      line_v[out_idx * 2 + 0] = (MopVertex){verts[ea].position, n_up, c, 0, 0};
      line_v[out_idx * 2 + 1] = (MopVertex){verts[eb].position, n_up, c, 0, 0};
      line_i[out_idx * 2 + 0] = out_idx * 2;
      line_i[out_idx * 2 + 1] = out_idx * 2 + 1;
      out_idx++;
    }
  }

  uint32_t actual_vc = out_idx * 2;
  uint32_t actual_ic = out_idx * 2;

  MopRhiBufferDesc vb_desc = {.data = line_v,
                              .size = actual_vc * sizeof(MopVertex)};
  MopRhiBufferDesc ib_desc = {.data = line_i,
                              .size = actual_ic * sizeof(uint32_t)};
  MopRhiBuffer *vb = vp->rhi->buffer_create(vp->device, &vb_desc);
  MopRhiBuffer *ib = vp->rhi->buffer_create(vp->device, &ib_desc);
  free(line_v);
  free(line_i);

  if (!vb || !ib) {
    if (vb)
      vp->rhi->buffer_destroy(vp->device, vb);
    if (ib)
      vp->rhi->buffer_destroy(vp->device, ib);
    return;
  }

  MopMat4 mvp =
      mop_mat4_multiply(vp->projection_matrix,
                        mop_mat4_multiply(vp->view_matrix, m->world_transform));

  MopRhiDrawCall call = {
      .vertex_buffer = vb,
      .index_buffer = ib,
      .vertex_count = actual_vc,
      .index_count = actual_ic,
      .object_id = 0,
      .model = m->world_transform,
      .view = vp->view_matrix,
      .projection = vp->projection_matrix,
      .mvp = mvp,
      .base_color = (MopColor){1, 1, 1, 1},
      .opacity = 1.0f,
      .light_dir = vp->light_dir,
      .ambient = 1.0f,
      .shading_mode = MOP_SHADING_FLAT,
      .wireframe = true,
      .depth_test = true,
      .backface_cull = false,
      .texture = NULL,
      .blend_mode = MOP_BLEND_OPAQUE,
      .metallic = 0.0f,
      .roughness = 0.5f,
      .emissive = (MopVec3){0, 0, 0},
      .lights = NULL,
      .light_count = 0,
      .vertex_format = NULL,
  };
  vp->rhi->draw(vp->device, vp->framebuffer, &call);

  vp->rhi->buffer_destroy(vp->device, vb);
  vp->rhi->buffer_destroy(vp->device, ib);
}

/* -------------------------------------------------------------------------
 * Face select overlay (Phase 3K)
 *
 * In face edit mode, alpha-blend selected faces with the theme's
 * face_select_color at face_select_opacity.
 * ------------------------------------------------------------------------- */

void mop_overlay_edit_faces(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp)
    return;
  if (vp->selection.mode != MOP_EDIT_FACE)
    return;
  if (vp->selection.element_count == 0)
    return;

  struct MopMesh *m = find_edit_mesh(vp);
  if (!m || !m->vertex_buffer || !m->index_buffer)
    return;

  const MopVertex *verts =
      (const MopVertex *)vp->rhi->buffer_read(m->vertex_buffer);
  const uint32_t *indices =
      (const uint32_t *)vp->rhi->buffer_read(m->index_buffer);
  if (!verts || !indices)
    return;

  uint32_t ic = m->index_count;
  uint32_t face_count = ic / 3;
  if (face_count == 0)
    return;

  MopColor sel_color = vp->theme.face_select_color;
  float sel_opacity = vp->theme.face_select_opacity;
  if (sel_opacity <= 0.0f)
    sel_opacity = 0.2f;

  /* Count selected faces that are valid */
  uint32_t sel_face_count = 0;
  for (uint32_t i = 0; i < vp->selection.element_count; i++) {
    if (vp->selection.elements[i] < face_count)
      sel_face_count++;
  }
  if (sel_face_count == 0)
    return;

  /* Build geometry for selected faces only */
  uint32_t sel_ic = sel_face_count * 3;
  MopVertex *sel_v = (MopVertex *)malloc(sel_ic * sizeof(MopVertex));
  uint32_t *sel_i = (uint32_t *)malloc(sel_ic * sizeof(uint32_t));
  if (!sel_v || !sel_i) {
    free(sel_v);
    free(sel_i);
    return;
  }

  MopVec3 n_up = {0, 1, 0};
  uint32_t vi = 0;
  for (uint32_t si = 0; si < vp->selection.element_count; si++) {
    uint32_t f = vp->selection.elements[si];
    if (f >= face_count)
      continue;

    for (int k = 0; k < 3; k++) {
      uint32_t orig_idx = indices[f * 3 + (uint32_t)k];
      if (orig_idx < m->vertex_count) {
        sel_v[vi] = verts[orig_idx];
      } else {
        sel_v[vi] = (MopVertex){{0, 0, 0}, n_up, sel_color, 0, 0};
      }
      sel_v[vi].color = sel_color;
      sel_i[vi] = vi;
      vi++;
    }
  }

  MopRhiBufferDesc vb_desc = {.data = sel_v, .size = vi * sizeof(MopVertex)};
  MopRhiBufferDesc ib_desc = {.data = sel_i, .size = vi * sizeof(uint32_t)};
  MopRhiBuffer *vb = vp->rhi->buffer_create(vp->device, &vb_desc);
  MopRhiBuffer *ib = vp->rhi->buffer_create(vp->device, &ib_desc);
  free(sel_v);
  free(sel_i);

  if (!vb || !ib) {
    if (vb)
      vp->rhi->buffer_destroy(vp->device, vb);
    if (ib)
      vp->rhi->buffer_destroy(vp->device, ib);
    return;
  }

  MopMat4 mvp =
      mop_mat4_multiply(vp->projection_matrix,
                        mop_mat4_multiply(vp->view_matrix, m->world_transform));

  MopRhiDrawCall call = {
      .vertex_buffer = vb,
      .index_buffer = ib,
      .vertex_count = vi,
      .index_count = vi,
      .object_id = 0,
      .model = m->world_transform,
      .view = vp->view_matrix,
      .projection = vp->projection_matrix,
      .mvp = mvp,
      .base_color = sel_color,
      .opacity = sel_opacity,
      .light_dir = vp->light_dir,
      .ambient = 1.0f,
      .shading_mode = MOP_SHADING_FLAT,
      .wireframe = false,
      .depth_test = true,
      .backface_cull = false,
      .texture = NULL,
      .blend_mode = MOP_BLEND_ALPHA,
      .metallic = 0.0f,
      .roughness = 0.5f,
      .emissive = (MopVec3){0, 0, 0},
      .lights = NULL,
      .light_count = 0,
      .vertex_format = NULL,
  };
  vp->rhi->draw(vp->device, vp->framebuffer, &call);

  vp->rhi->buffer_destroy(vp->device, vb);
  vp->rhi->buffer_destroy(vp->device, ib);
}

/* -------------------------------------------------------------------------
 * Object outline overlay (silhouette post-process)
 *
 * Scans the object_id buffer for silhouette edges — pixels where the
 * object_id differs from a neighbor.  Paints the accent color at those
 * edges.  Selected objects get a thicker outline, unselected get 1px.
 *
 * Works on all backends via the RHI framebuffer_read_object_id function.
 * The color buffer is written in-place (CPU) or via readback (GPU).
 * ------------------------------------------------------------------------- */

void mop_overlay_builtin_outline(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp || !vp->rhi->framebuffer_read_object_id)
    return;

  int w, h;
  const uint32_t *id_buf =
      vp->rhi->framebuffer_read_object_id(vp->device, vp->framebuffer, &w, &h);
  if (!id_buf || w <= 0 || h <= 0)
    return;

  int cw, ch;
  const uint8_t *color_ro =
      vp->rhi->framebuffer_read_color(vp->device, vp->framebuffer, &cw, &ch);
  if (!color_ro || cw != w || ch != h)
    return;

  /* We need a mutable pointer to the color buffer.  Both CPU and Vulkan
   * backends return their internal readback buffer which we can write. */
  uint8_t *rgba = (uint8_t *)(uintptr_t)color_ro;

  MopColor accent = vp->theme.accent;
  uint8_t ar = (uint8_t)(accent.r * 255.0f);
  uint8_t ag = (uint8_t)(accent.g * 255.0f);
  uint8_t ab = (uint8_t)(accent.b * 255.0f);

  uint32_t sel_id = vp->selected_id;
  float alpha_sel = vp->theme.outline_opacity_selected;
  float alpha_unsel = vp->theme.outline_opacity_unselected;
  int radius_sel = 2; /* thick outline for selected objects */

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint32_t id = id_buf[y * w + x];
      if (id == 0 || id >= 0xFFFD0000u)
        continue; /* skip background/chrome (grid, lights, gizmo) */

      /* 4-connected neighbor edge test.
       * Ignore chrome neighbors (>=0xFFFD0000) so grid/UI geometry
       * doesn't create false outline edges. Background (id==0) neighbors
       * ARE counted — they form the object silhouette. */
      bool is_edge = false;
      uint32_t nid;
      if (x > 0) {
        nid = id_buf[y * w + (x - 1)];
        if (nid != id && nid < 0xFFFD0000u)
          is_edge = true;
      }
      if (x < w - 1) {
        nid = id_buf[y * w + (x + 1)];
        if (nid != id && nid < 0xFFFD0000u)
          is_edge = true;
      }
      if (y > 0) {
        nid = id_buf[(y - 1) * w + x];
        if (nid != id && nid < 0xFFFD0000u)
          is_edge = true;
      }
      if (y < h - 1) {
        nid = id_buf[(y + 1) * w + x];
        if (nid != id && nid < 0xFFFD0000u)
          is_edge = true;
      }
      if (!is_edge)
        continue;

      bool selected = (id == sel_id && sel_id != 0);
      float alpha = selected ? alpha_sel : alpha_unsel;

      if (selected) {
        /* Thick outline: paint within radius around edge pixel */
        for (int dy = -radius_sel; dy <= radius_sel; dy++) {
          for (int dx = -radius_sel; dx <= radius_sel; dx++) {
            int px = x + dx, py = y + dy;
            if (px < 0 || px >= w || py < 0 || py >= h)
              continue;
            int idx = (py * w + px) * 4;
            rgba[idx + 0] =
                (uint8_t)(rgba[idx + 0] * (1.0f - alpha) + ar * alpha);
            rgba[idx + 1] =
                (uint8_t)(rgba[idx + 1] * (1.0f - alpha) + ag * alpha);
            rgba[idx + 2] =
                (uint8_t)(rgba[idx + 2] * (1.0f - alpha) + ab * alpha);
          }
        }
      } else {
        /* Thin 1px outline */
        int idx = (y * w + x) * 4;
        rgba[idx + 0] = (uint8_t)(rgba[idx + 0] * (1.0f - alpha) + ar * alpha);
        rgba[idx + 1] = (uint8_t)(rgba[idx + 1] * (1.0f - alpha) + ag * alpha);
        rgba[idx + 2] = (uint8_t)(rgba[idx + 2] * (1.0f - alpha) + ab * alpha);
      }
    }
  }
}

/* -------------------------------------------------------------------------
 * Selection outline post-process (Phase 6)
 *
 * Draws a silhouette outline around the selected object by scanning the
 * object_id buffer.  Pixels adjacent to the selected object (but not part
 * of it) are blended with the theme's selection_outline color.
 *
 * NOTE: This directly accesses the CPU framebuffer (MopSwFramebuffer).
 * For GPU backends a shader-based approach would be needed.
 * ------------------------------------------------------------------------- */

void mop_overlay_builtin_selection_outline(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp || vp->selected_id == 0)
    return;

  int w, h;
  const uint8_t *color_buf =
      vp->rhi->framebuffer_read_color(vp->device, vp->framebuffer, &w, &h);
  if (!color_buf || w <= 0 || h <= 0)
    return;

  /* Read the object_id buffer from the CPU framebuffer.
   * For the CPU backend, we can access it via the MopSwFramebuffer. */
  MopSwFramebuffer *sw_fb = (MopSwFramebuffer *)vp->framebuffer;
  const uint32_t *id_buf = sw_fb->object_id;
  if (!id_buf)
    return;

  uint32_t sel_id = vp->selected_id;
  MopColor outline_color = vp->theme.selection_outline;
  float outline_width = vp->theme.selection_outline_width;
  int radius = (int)(outline_width + 0.5f);
  if (radius < 1)
    radius = 1;

  /* Scan for edge pixels: non-selected pixels adjacent to selected pixels */
  uint8_t *rgba = sw_fb->color;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint32_t id = id_buf[y * w + x];
      if (id == sel_id)
        continue; /* skip interior pixels */

      /* Check if any neighbor within radius is the selected object */
      bool adjacent = false;
      for (int dy = -radius; dy <= radius && !adjacent; dy++) {
        for (int dx = -radius; dx <= radius && !adjacent; dx++) {
          int nx = x + dx, ny = y + dy;
          if (nx < 0 || nx >= w || ny < 0 || ny >= h)
            continue;
          if (id_buf[ny * w + nx] == sel_id)
            adjacent = true;
        }
      }

      if (adjacent) {
        int idx = (y * w + x) * 4;
        uint8_t or_ = (uint8_t)(outline_color.r * 255.0f);
        uint8_t og = (uint8_t)(outline_color.g * 255.0f);
        uint8_t ob = (uint8_t)(outline_color.b * 255.0f);
        /* Alpha blend outline over existing pixel */
        float alpha = outline_color.a;
        rgba[idx + 0] = (uint8_t)(rgba[idx + 0] * (1.0f - alpha) + or_ * alpha);
        rgba[idx + 1] = (uint8_t)(rgba[idx + 1] * (1.0f - alpha) + og * alpha);
        rgba[idx + 2] = (uint8_t)(rgba[idx + 2] * (1.0f - alpha) + ob * alpha);
      }
    }
  }
}

/* =========================================================================
 * Analytical ground-plane grid (screen-space, anti-aliased)
 *
 * Replaces the old geometry-based grid.  For each background pixel, cast a
 * ray from the camera through the pixel, intersect with Y=0, and compute
 * distance to the nearest grid line.  Uses smoothstep for anti-aliasing
 * and distance-based fade to avoid moiré.
 * ========================================================================= */

static inline float ov_smoothstep(float lo, float hi, float x) {
  if (x <= lo)
    return 1.0f;
  if (x >= hi)
    return 0.0f;
  float t = (x - lo) / (hi - lo);
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

/* Blender's grid line AA from GPU Gems 2, Ch.22 "Fast Pre-filtered Lines".
 * Approximates pixel-circle vs. line-segment intersection area.
 * DISC_RADIUS = 1/sqrt(pi) * 1.05 treats the square pixel as a circle
 * with equivalent area and adds 5% for better coverage. */
#define MOP_DISC_RADIUS 0.5923f /* M_1_SQRTPI * 1.05 */
#define MOP_GRID_LINE_SMOOTH_START (0.5f + MOP_DISC_RADIUS)
#define MOP_GRID_LINE_SMOOTH_END (0.5f - MOP_DISC_RADIUS)

static inline float grid_line_step(float dist) {
  /* smoothstep(start, end, dist) where start > end → inverted */
  if (dist <= MOP_GRID_LINE_SMOOTH_END)
    return 1.0f;
  if (dist >= MOP_GRID_LINE_SMOOTH_START)
    return 0.0f;
  float t = (dist - MOP_GRID_LINE_SMOOTH_END) /
            (MOP_GRID_LINE_SMOOTH_START - MOP_GRID_LINE_SMOOTH_END);
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

void mop_overlay_builtin_grid(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp)
    return;

  int w, h;
  const uint8_t *color_ro =
      vp->rhi->framebuffer_read_color(vp->device, vp->framebuffer, &w, &h);
  if (!color_ro || w <= 0 || h <= 0)
    return;

  const float *depth_buf =
      vp->rhi->framebuffer_read_depth(vp->device, vp->framebuffer, &w, &h);
  if (!depth_buf)
    return;

  uint8_t *rgba = (uint8_t *)(uintptr_t)color_ro;

  MopMat4 VPm = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);

  /* VP matrix rows for depth at (wx, 0, wz) */
  float vp_z0 = VPm.d[2], vp_z2 = VPm.d[10], vp_z3 = VPm.d[14];
  float vp_w0 = VPm.d[3], vp_w2 = VPm.d[11], vp_w3 = VPm.d[15];

  /* Homography: screen NDC → Y=0 world XZ (skip Y column of VP) */
  float H[9];
  H[0] = VPm.d[0];
  H[1] = VPm.d[8];
  H[2] = VPm.d[12];
  H[3] = VPm.d[1];
  H[4] = VPm.d[9];
  H[5] = VPm.d[13];
  H[6] = VPm.d[3];
  H[7] = VPm.d[11];
  H[8] = VPm.d[15];

  float det = H[0] * (H[4] * H[8] - H[5] * H[7]) -
              H[1] * (H[3] * H[8] - H[5] * H[6]) +
              H[2] * (H[3] * H[7] - H[4] * H[6]);
  if (fabsf(det) < 1e-12f)
    return;
  float idet = 1.0f / det;
  float Hi[9];
  Hi[0] = (H[4] * H[8] - H[5] * H[7]) * idet;
  Hi[1] = (H[2] * H[7] - H[1] * H[8]) * idet;
  Hi[2] = (H[1] * H[5] - H[2] * H[4]) * idet;
  Hi[3] = (H[5] * H[6] - H[3] * H[8]) * idet;
  Hi[4] = (H[0] * H[8] - H[2] * H[6]) * idet;
  Hi[5] = (H[2] * H[3] - H[0] * H[5]) * idet;
  Hi[6] = (H[3] * H[7] - H[4] * H[6]) * idet;
  Hi[7] = (H[1] * H[6] - H[0] * H[7]) * idet;
  Hi[8] = (H[0] * H[4] - H[1] * H[3]) * idet;

  bool is_cpu = (vp->backend_type == MOP_BACKEND_CPU);
  float grid_half = 25.0f; /* 50x50 */

  /* Grid line colors from theme */
  MopColor minor_c = vp->theme.grid_minor;
  MopColor major_c = vp->theme.grid_major;
  MopColor ax_x_c = vp->theme.grid_axis_x;
  MopColor ax_z_c = vp->theme.grid_axis_z;

  /* Minor grid → sRGB */
  uint8_t dr = (uint8_t)(sqrtf(minor_c.r) * 255.0f);
  uint8_t dg = (uint8_t)(sqrtf(minor_c.g) * 255.0f);
  uint8_t db = (uint8_t)(sqrtf(minor_c.b) * 255.0f);

  /* Major grid → sRGB */
  uint8_t cr = (uint8_t)(sqrtf(major_c.r) * 255.0f);
  uint8_t cg_c = (uint8_t)(sqrtf(major_c.g) * 255.0f);
  uint8_t cb = (uint8_t)(sqrtf(major_c.b) * 255.0f);

  /* X-axis (red) → sRGB */
  uint8_t ax_xr = (uint8_t)(sqrtf(ax_x_c.r) * 255.0f);
  uint8_t ax_xg = (uint8_t)(sqrtf(ax_x_c.g) * 255.0f);
  uint8_t ax_xb = (uint8_t)(sqrtf(ax_x_c.b) * 255.0f);

  /* Z-axis (green) → sRGB */
  uint8_t ax_zr = (uint8_t)(sqrtf(ax_z_c.r) * 255.0f);
  uint8_t ax_zg = (uint8_t)(sqrtf(ax_z_c.g) * 255.0f);
  uint8_t ax_zb = (uint8_t)(sqrtf(ax_z_c.b) * 255.0f);

  float inv_w2 = 2.0f / (float)w;
  float inv_h2 = 2.0f / (float)h;

  /* Bounding box optimization: project grid corners to screen */
  int px0 = 0, px1 = w, py0 = 0, py1 = h;
  {
    float smin_x = (float)w, smax_x = 0, smin_y = (float)h, smax_y = 0;
    float gc[4][2] = {{-grid_half, -grid_half},
                      {grid_half, -grid_half},
                      {grid_half, grid_half},
                      {-grid_half, grid_half}};
    bool full = false;
    for (int c = 0; c < 4; c++) {
      float cw = VPm.d[3] * gc[c][0] + VPm.d[11] * gc[c][1] + VPm.d[15];
      if (cw < 0.001f) {
        full = true;
        break;
      }
      float ndx = (VPm.d[0] * gc[c][0] + VPm.d[8] * gc[c][1] + VPm.d[12]) / cw;
      float ndy = (VPm.d[1] * gc[c][0] + VPm.d[9] * gc[c][1] + VPm.d[13]) / cw;
      float sx = (ndx + 1.0f) * 0.5f * (float)w;
      float sy = (1.0f - ndy) * 0.5f * (float)h;
      if (sx < smin_x)
        smin_x = sx;
      if (sx > smax_x)
        smax_x = sx;
      if (sy < smin_y)
        smin_y = sy;
      if (sy > smax_y)
        smax_y = sy;
    }
    if (!full) {
      px0 = (int)smin_x - 2;
      if (px0 < 0)
        px0 = 0;
      px1 = (int)smax_x + 2;
      if (px1 > w)
        px1 = w;
      py0 = (int)smin_y - 2;
      if (py0 < 0)
        py0 = 0;
      py1 = (int)smax_y + 2;
      if (py1 > h)
        py1 = h;
    }
  }

  for (int py = py0; py < py1; py++) {
    float ny = 1.0f - ((float)py + 0.5f) * inv_h2;
    float ry0 = Hi[1] * ny + Hi[2];
    float ry1 = Hi[4] * ny + Hi[5];
    float ry2 = Hi[7] * ny + Hi[8];
    int row_off = py * w;

    for (int px = px0; px < px1; px++) {
      float nx = ((float)px + 0.5f) * inv_w2 - 1.0f;
      float s = Hi[6] * nx + ry2;
      if (s < 1e-8f)
        continue;

      float inv_s = 1.0f / s;
      float wx = (Hi[0] * nx + ry0) * inv_s;
      float wz = (Hi[3] * nx + ry1) * inv_s;

      if (wx < -grid_half || wx > grid_half || wz < -grid_half ||
          wz > grid_half)
        continue;

      /* Quick skip: if not near any grid line, skip before derivatives.
       * Subgrid spacing = 1/3, so max proximity is 1/6. If we're far
       * from all lines at all levels, skip immediately. */
      {
        float qx = fabsf(wx - roundf(wx));
        float qz = fabsf(wz - roundf(wz));
        float q3x = fabsf(wx * 3.0f - roundf(wx * 3.0f)) / 3.0f;
        float q3z = fabsf(wz * 3.0f - roundf(wz * 3.0f)) / 3.0f;
        float qax = fabsf(wz), qaz = fabsf(wx);
        /* Conservative threshold: skip if far from all lines.
         * inv_s tells us the scale — larger inv_s means closer view. */
        float thresh = 0.4f * inv_s;
        if (thresh > 0.15f)
          thresh = 0.15f;
        if (fminf(q3x, q3z) > thresh && fminf(qx, qz) > thresh &&
            fminf(qax, qaz) > thresh)
          continue;
      }

      /* Screen-space derivatives (CPU fwidth): use Manhattan norm
       * (abs(dFdx)+abs(dFdy)) to avoid sqrt — matches GLSL fwidth(). */
      float dwx_dpx = (Hi[0] - wx * Hi[6]) * inv_s * inv_w2;
      float dwx_dpy = (Hi[1] - wx * Hi[7]) * inv_s * inv_h2;
      float dwz_dpx = (Hi[3] - wz * Hi[6]) * inv_s * inv_w2;
      float dwz_dpy = (Hi[4] - wz * Hi[7]) * inv_s * inv_h2;
      float fw_x = fabsf(dwx_dpx) + fabsf(dwx_dpy);
      float fw_z = fabsf(dwz_dpx) + fabsf(dwz_dpy);
      if (fw_x < 1e-7f)
        fw_x = 1e-7f;
      if (fw_z < 1e-7f)
        fw_z = 1e-7f;

      /* --- Three grid levels using Blender's GPU Gems 2 disc-radius AA ---
       * For each level: distance to nearest line / fwidth → pixel dist,
       * then grid_line_step() for smooth pixel-circle coverage. */

      /* Subgrid: every 1/3 unit */
      float sub3x = wx * 3.0f, sub3z = wz * 3.0f;
      float sub_px_x = fabsf(sub3x - roundf(sub3x)) / (3.0f * fw_x);
      float sub_px_z = fabsf(sub3z - roundf(sub3z)) / (3.0f * fw_z);
      float a_sub = grid_line_step(fminf(sub_px_x, sub_px_z));

      /* Major grid: every 1 unit */
      float maj_px_x = fabsf(wx - roundf(wx)) / fw_x;
      float maj_px_z = fabsf(wz - roundf(wz)) / fw_z;
      float a_maj = grid_line_step(fminf(maj_px_x, maj_px_z));

      /* Axis: X-axis (wz=0), Z-axis (wx=0) — use line_size=0.1 like Blender */
      float ax_px_x = fabsf(wz) / fw_z;
      float ax_px_z = fabsf(wx) / fw_x;
      float a_ax_x = grid_line_step(ax_px_x - 0.1f);
      float a_ax_z = grid_line_step(ax_px_z - 0.1f);
      float a_ax = fmaxf(a_ax_x, a_ax_z);

      /* Nothing visible? */
      if (a_sub < 0.01f && a_maj < 0.01f && a_ax < 0.01f)
        continue;

      /* Blender-style multi-iteration depth handling */
      float depth_mult = 1.0f;
      {
        float clip_z = vp_z0 * wx + vp_z2 * wz + vp_z3;
        float clip_w = vp_w0 * wx + vp_w2 * wz + vp_w3;
        if (clip_w > 0.001f) {
          float ndc_z = clip_z / clip_w;
          float gd = is_cpu ? ndc_z * 0.5f + 0.5f : ndc_z;
          float scene_d = depth_buf[row_off + px];
          /* Reverse-Z: clear=0, closer=larger; Standard: clear=1,
           * closer=smaller */
          bool is_reverse_z = vp->reverse_z && !is_cpu;
          bool has_geometry =
              is_reverse_z ? (scene_d > 0.0001f) : (scene_d < 0.9999f);
          if (has_geometry) {
            static const float biases[4] = {0.00025f, 0.00012f, 0.00004f,
                                            -0.00015f};
            static const float weights[4] = {0.4f, 0.3f, 0.2f, 0.1f};
            depth_mult = 0.0f;
            for (int i = 0; i < 4; i++) {
              if (is_reverse_z ? (gd - biases[i] >= scene_d)
                               : (gd + biases[i] <= scene_d))
                depth_mult += weights[i];
            }
            if (depth_mult < 0.01f)
              continue;
          }
        }
      }

      /* Edge fade — smooth fade over last 20% of grid extent (Blender-style) */
      float edge = fmaxf(fabsf(wx), fabsf(wz));
      float fade_start = grid_half * 0.8f;
      float edge_fade = ov_smoothstep(fade_start, grid_half, edge);

      /* Pick dominant level: axis > major > subgrid */
      float alpha;
      uint8_t fr, fg, fb;
      if (a_ax > 0.01f) {
        alpha = a_ax * 0.9f;
        if (a_ax_x >= a_ax_z) {
          fr = ax_xr;
          fg = ax_xg;
          fb = ax_xb; /* X-axis → red */
        } else {
          fr = ax_zr;
          fg = ax_zg;
          fb = ax_zb; /* Z-axis → green */
        }
      } else if (a_maj > 0.01f) {
        alpha = a_maj * 0.45f;
        fr = cr;
        fg = cg_c;
        fb = cb;
      } else {
        alpha = a_sub * 0.2f;
        fr = dr;
        fg = dg;
        fb = db;
      }

      alpha *= depth_mult * edge_fade;
      if (alpha < 0.005f)
        continue;

      int idx = (row_off + px) * 4;
      float ia = 1.0f - alpha;
      rgba[idx + 0] = (uint8_t)(rgba[idx + 0] * ia + fr * alpha);
      rgba[idx + 1] = (uint8_t)(rgba[idx + 1] * ia + fg * alpha);
      rgba[idx + 2] = (uint8_t)(rgba[idx + 2] * ia + fb * alpha);
    }
  }
}

/* =========================================================================
 * 2D screen-space light indicator overlay (anti-aliased lines)
 *
 * Projects 3D light indicator vertices to screen space and draws clean
 * anti-aliased 2D lines directly on the framebuffer.  Replaces the old
 * geometry-based indicators that caused pixelation/aliasing.
 * ========================================================================= */

/* Draw a single anti-aliased line segment on the framebuffer */
static void draw_aa_line(uint8_t *rgba, int w, int h, float x0, float y0,
                         float x1, float y1, uint8_t cr, uint8_t cg, uint8_t cb,
                         float line_width, float opacity) {
  /* Bounding box with margin */
  float margin = line_width + 1.5f;
  int bx0 = (int)fmaxf(0, fminf(x0, x1) - margin);
  int by0 = (int)fmaxf(0, fminf(y0, y1) - margin);
  int bx1 = (int)fminf((float)(w - 1), fmaxf(x0, x1) + margin);
  int by1 = (int)fminf((float)(h - 1), fmaxf(y0, y1) + margin);

  float dx = x1 - x0, dy = y1 - y0;
  float seg_len = sqrtf(dx * dx + dy * dy);
  if (seg_len < 0.5f)
    return;
  float inv_len = 1.0f / seg_len;
  /* Unit direction and perpendicular */
  float ux = dx * inv_len, uy = dy * inv_len;
  float hw = line_width * 0.5f;

  for (int py = by0; py <= by1; py++) {
    for (int px = bx0; px <= bx1; px++) {
      float fx = (float)px + 0.5f - x0;
      float fy = (float)py + 0.5f - y0;

      /* Project onto line segment */
      float along = fx * ux + fy * uy;
      if (along < -1.0f || along > seg_len + 1.0f)
        continue;

      /* Perpendicular distance */
      float perp = fabsf(fx * (-uy) + fy * ux);

      /* Anti-aliased alpha based on distance from line center */
      float alpha;
      if (perp <= hw - 0.5f)
        alpha = 1.0f;
      else if (perp >= hw + 0.5f)
        continue;
      else
        alpha = 1.0f - (perp - (hw - 0.5f));

      /* Smooth caps at endpoints */
      if (along < 0.0f)
        alpha *= fmaxf(0.0f, 1.0f + along);
      else if (along > seg_len)
        alpha *= fmaxf(0.0f, 1.0f - (along - seg_len));

      alpha *= opacity;
      if (alpha < 0.004f)
        continue;

      int idx = (py * w + px) * 4;
      rgba[idx + 0] = (uint8_t)(rgba[idx + 0] * (1.0f - alpha) + cr * alpha);
      rgba[idx + 1] = (uint8_t)(rgba[idx + 1] * (1.0f - alpha) + cg * alpha);
      rgba[idx + 2] = (uint8_t)(rgba[idx + 2] * (1.0f - alpha) + cb * alpha);
    }
  }
}

/* Depth-aware AA line: same as draw_aa_line but interpolates depth along the
 * segment and skips pixels that are behind scene geometry in the depth buffer.
 * d0/d1 are NDC depth at (x0,y0) and (x1,y1). */
static void draw_aa_line_depth(uint8_t *rgba, const float *depth_buf, int w,
                               int h, float x0, float y0, float d0, float x1,
                               float y1, float d1, uint8_t cr, uint8_t cg,
                               uint8_t cb, float line_width, float opacity,
                               bool reverse_z, bool is_cpu) {
  float margin = line_width + 1.5f;
  int bx0 = (int)fmaxf(0, fminf(x0, x1) - margin);
  int by0 = (int)fmaxf(0, fminf(y0, y1) - margin);
  int bx1 = (int)fminf((float)(w - 1), fmaxf(x0, x1) + margin);
  int by1 = (int)fminf((float)(h - 1), fmaxf(y0, y1) + margin);

  float dx = x1 - x0, dy = y1 - y0;
  float seg_len = sqrtf(dx * dx + dy * dy);
  if (seg_len < 0.5f)
    return;
  float inv_len = 1.0f / seg_len;
  float ux = dx * inv_len, uy = dy * inv_len;
  float hw = line_width * 0.5f;

  for (int py = by0; py <= by1; py++) {
    for (int px = bx0; px <= bx1; px++) {
      float fx = (float)px + 0.5f - x0;
      float fy = (float)py + 0.5f - y0;

      float along = fx * ux + fy * uy;
      if (along < -1.0f || along > seg_len + 1.0f)
        continue;

      float perp = fabsf(fx * (-uy) + fy * ux);

      float alpha;
      if (perp <= hw - 0.5f)
        alpha = 1.0f;
      else if (perp >= hw + 0.5f)
        continue;
      else
        alpha = 1.0f - (perp - (hw - 0.5f));

      if (along < 0.0f)
        alpha *= fmaxf(0.0f, 1.0f + along);
      else if (along > seg_len)
        alpha *= fmaxf(0.0f, 1.0f - (along - seg_len));

      alpha *= opacity;
      if (alpha < 0.004f)
        continue;

      /* Per-pixel depth test: interpolate depth along segment */
      if (depth_buf) {
        float t = along / seg_len;
        if (t < 0.0f)
          t = 0.0f;
        if (t > 1.0f)
          t = 1.0f;
        float ndc_z = d0 + (d1 - d0) * t;
        float gd = is_cpu ? ndc_z * 0.5f + 0.5f : ndc_z;
        float scene_d = depth_buf[py * w + px];
        float bias = 0.002f;
        bool occluded;
        if (reverse_z) {
          occluded = (scene_d > 0.0001f) && (gd + bias < scene_d);
        } else {
          occluded = (scene_d < 0.9999f) && (gd - bias > scene_d);
        }
        if (occluded)
          continue;
      }

      int idx = (py * w + px) * 4;
      rgba[idx + 0] = (uint8_t)(rgba[idx + 0] * (1.0f - alpha) + cr * alpha);
      rgba[idx + 1] = (uint8_t)(rgba[idx + 1] * (1.0f - alpha) + cg * alpha);
      rgba[idx + 2] = (uint8_t)(rgba[idx + 2] * (1.0f - alpha) + cb * alpha);
    }
  }
}

/* Project world point to screen pixel coordinates. Returns false if behind
 * camera. Optionally returns NDC depth in *out_depth (0..1). */
static bool project_to_screen_depth(MopVec3 p, const MopMat4 *vp_mat, int w,
                                    int h, float *sx, float *sy,
                                    float *out_depth) {
  MopVec4 clip = mop_mat4_mul_vec4(*vp_mat, (MopVec4){p.x, p.y, p.z, 1.0f});
  if (clip.w <= 0.001f)
    return false;
  float inv_w = 1.0f / clip.w;
  float nx = clip.x * inv_w;
  float ny = clip.y * inv_w;
  *sx = (nx * 0.5f + 0.5f) * (float)w;
  *sy = (1.0f - (ny * 0.5f + 0.5f)) * (float)h;
  if (out_depth)
    *out_depth = clip.z * inv_w; /* raw NDC z (Vulkan: [0,1], GL: [-1,1]) */
  return true;
}

static bool project_to_screen(MopVec3 p, const MopMat4 *vp_mat, int w, int h,
                              float *sx, float *sy) {
  return project_to_screen_depth(p, vp_mat, w, h, sx, sy, NULL);
}

/* Draw a circle as line segments between projected points.
 * If depth_buf is non-NULL, does per-pixel depth testing. */
static void draw_circle_2d_ex(uint8_t *rgba, const float *depth_buf, int w,
                              int h, MopVec3 center, float radius,
                              MopVec3 axis_u, MopVec3 axis_v,
                              const MopMat4 *vp_mat, uint8_t cr, uint8_t cg,
                              uint8_t cb, float line_w, float opacity,
                              bool reverse_z, bool is_cpu) {
  int segs = 32;
  float prev_sx, prev_sy, prev_d;
  bool prev_ok = false;
  for (int i = 0; i <= segs; i++) {
    float a = (float)i * 2.0f * 3.14159265f / (float)segs;
    float ca = cosf(a), sa = sinf(a);
    MopVec3 pt = {center.x + (axis_u.x * ca + axis_v.x * sa) * radius,
                  center.y + (axis_u.y * ca + axis_v.y * sa) * radius,
                  center.z + (axis_u.z * ca + axis_v.z * sa) * radius};
    float cur_sx, cur_sy, cur_d = 0.0f;
    bool cur_ok =
        project_to_screen_depth(pt, vp_mat, w, h, &cur_sx, &cur_sy, &cur_d);
    if (prev_ok && cur_ok) {
      if (depth_buf)
        draw_aa_line_depth(rgba, depth_buf, w, h, prev_sx, prev_sy, prev_d,
                           cur_sx, cur_sy, cur_d, cr, cg, cb, line_w, opacity,
                           reverse_z, is_cpu);
      else
        draw_aa_line(rgba, w, h, prev_sx, prev_sy, cur_sx, cur_sy, cr, cg, cb,
                     line_w, opacity);
    }
    prev_sx = cur_sx;
    prev_sy = cur_sy;
    prev_d = cur_d;
    prev_ok = cur_ok;
  }
}

static void draw_circle_2d(uint8_t *rgba, int w, int h, MopVec3 center,
                           float radius, MopVec3 axis_u, MopVec3 axis_v,
                           const MopMat4 *vp_mat, uint8_t cr, uint8_t cg,
                           uint8_t cb, float line_w, float opacity) {
  draw_circle_2d_ex(rgba, NULL, w, h, center, radius, axis_u, axis_v, vp_mat,
                    cr, cg, cb, line_w, opacity, false, false);
}

/* Forward declarations for helpers defined later (used by axis navigator too)
 */
static void draw_filled_circle_depth(uint8_t *rgba, const float *depth_buf,
                                     int w, int h, float cx, float cy,
                                     float radius, uint8_t cr, uint8_t cg,
                                     uint8_t cb, float opacity, float ndc_depth,
                                     bool reverse_z, bool is_cpu);
static void draw_filled_circle(uint8_t *rgba, int w, int h, float cx, float cy,
                               float radius, uint8_t cr, uint8_t cg, uint8_t cb,
                               float opacity);

void mop_overlay_builtin_light_indicators(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp)
    return;

  int w, h;
  const uint8_t *color_ro =
      vp->rhi->framebuffer_read_color(vp->device, vp->framebuffer, &w, &h);
  if (!color_ro || w <= 0 || h <= 0)
    return;
  uint8_t *rgba = (uint8_t *)(uintptr_t)color_ro;

  const float *li_depth_buf =
      vp->rhi->framebuffer_read_depth(vp->device, vp->framebuffer, &w, &h);
  bool li_is_cpu = (vp->backend_type == MOP_BACKEND_CPU);
  bool li_rev_z = vp->reverse_z && !li_is_cpu;

  MopMat4 vp_mat = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);

  /* Blender-style: thin clean lines, 1px linear ramp AA */
  float line_w = 1.0f;
  float opacity = 0.85f;

  for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
    if (!vp->lights[i].active)
      continue;
    const MopLight *light = &vp->lights[i];

    /* Use light's own color for indicator (clamped to visible brightness).
     * Blender tints light icons with the light color. */
    MopColor lc = light->color;
    float max_c = fmaxf(lc.r, fmaxf(lc.g, lc.b));
    if (max_c < 0.01f) {
      lc = (MopColor){0.85f, 0.85f, 0.85f, 1.0f};
    } else if (max_c > 1.0f) {
      lc.r /= max_c;
      lc.g /= max_c;
      lc.b /= max_c;
    }
    /* Ensure minimum brightness for visibility */
    float lum = lc.r * 0.299f + lc.g * 0.587f + lc.b * 0.114f;
    if (lum < 0.3f) {
      float boost = 0.3f / fmaxf(lum, 0.001f);
      lc.r = fminf(1.0f, lc.r * boost);
      lc.g = fminf(1.0f, lc.g * boost);
      lc.b = fminf(1.0f, lc.b * boost);
    }
    /* Linear-to-sRGB */
    uint8_t cr = (uint8_t)(sqrtf(lc.r) * 255.0f);
    uint8_t cg = (uint8_t)(sqrtf(lc.g) * 255.0f);
    uint8_t cb = (uint8_t)(sqrtf(lc.b) * 255.0f);

    /* Compute indicator world position */
    MopVec3 pos;
    if (light->type == MOP_LIGHT_DIRECTIONAL) {
      MopVec3 dir = mop_vec3_normalize(light->direction);
      pos = mop_vec3_add(vp->cam_target, mop_vec3_scale(dir, 3.0f));
    } else {
      pos = light->position;
    }

    float s = 1.0f;

    float csx, csy, csd;
    if (!project_to_screen_depth(pos, &vp_mat, w, h, &csx, &csy, &csd))
      continue;
    /* Pixel alignment */
    csx = roundf(csx);
    csy = roundf(csy);

    if (light->type == MOP_LIGHT_POINT) {
      float inner = 0.04f * s, outer = 0.14f * s;
      draw_filled_circle_depth(rgba, li_depth_buf, w, h, csx, csy, 2.5f, cr, cg,
                               cb, opacity, csd, li_rev_z, li_is_cpu);
      static const float ray_angles[8] = {0.0f,       0.7853982f, 1.5707963f,
                                          2.3561945f, 3.1415927f, 3.9269908f,
                                          4.7123890f, 5.4977871f};
      for (int r = 0; r < 8; r++) {
        float ca = cosf(ray_angles[r]), sa = sinf(ray_angles[r]);
        MopVec3 d = {ca, sa, 0.0f};
        MopVec3 a = {pos.x + d.x * inner, pos.y + d.y * inner,
                     pos.z + d.z * inner};
        MopVec3 b = {pos.x + d.x * outer, pos.y + d.y * outer,
                     pos.z + d.z * outer};
        float ax, ay, ad, bx, by, bd;
        if (project_to_screen_depth(a, &vp_mat, w, h, &ax, &ay, &ad) &&
            project_to_screen_depth(b, &vp_mat, w, h, &bx, &by, &bd))
          draw_aa_line_depth(rgba, li_depth_buf, w, h, roundf(ax), roundf(ay),
                             ad, roundf(bx), roundf(by), bd, cr, cg, cb, line_w,
                             opacity, li_rev_z, li_is_cpu);
      }
    } else if (light->type == MOP_LIGHT_DIRECTIONAL) {
      float radius = 0.10f * s;
      float ray_len = 0.30f * s;
      MopVec3 dir = mop_vec3_normalize(light->direction);
      MopVec3 up = {0, 1, 0};
      if (fabsf(mop_vec3_dot(dir, up)) > 0.99f)
        up = (MopVec3){1, 0, 0};
      MopVec3 right = mop_vec3_normalize(mop_vec3_cross(up, dir));
      MopVec3 u = mop_vec3_cross(dir, right);

      draw_filled_circle_depth(rgba, li_depth_buf, w, h, csx, csy, 2.0f, cr, cg,
                               cb, opacity, csd, li_rev_z, li_is_cpu);
      draw_circle_2d_ex(rgba, li_depth_buf, w, h, pos, radius, right, u,
                        &vp_mat, cr, cg, cb, line_w, opacity, li_rev_z,
                        li_is_cpu);

      float angles[4] = {0, 1.5707963f, 3.1415927f, 4.7123890f};
      for (int r = 0; r < 4; r++) {
        float ca = cosf(angles[r]), sa = sinf(angles[r]);
        MopVec3 base = {pos.x + (right.x * ca + u.x * sa) * radius,
                        pos.y + (right.y * ca + u.y * sa) * radius,
                        pos.z + (right.z * ca + u.z * sa) * radius};
        MopVec3 tip = {base.x + dir.x * ray_len, base.y + dir.y * ray_len,
                       base.z + dir.z * ray_len};
        float bsx, bsy, bd, tsx, tsy, td;
        if (project_to_screen_depth(base, &vp_mat, w, h, &bsx, &bsy, &bd) &&
            project_to_screen_depth(tip, &vp_mat, w, h, &tsx, &tsy, &td))
          draw_aa_line_depth(rgba, li_depth_buf, w, h, roundf(bsx), roundf(bsy),
                             bd, roundf(tsx), roundf(tsy), td, cr, cg, cb,
                             line_w, opacity, li_rev_z, li_is_cpu);
      }
    } else if (light->type == MOP_LIGHT_SPOT) {
      float base_r = 0.15f * s;
      float height = 0.30f * s;
      MopVec3 dir = mop_vec3_normalize(light->direction);
      MopVec3 up = {0, 1, 0};
      if (fabsf(mop_vec3_dot(dir, up)) > 0.99f)
        up = (MopVec3){1, 0, 0};
      MopVec3 right = mop_vec3_normalize(mop_vec3_cross(up, dir));
      MopVec3 u = mop_vec3_cross(dir, right);

      draw_filled_circle_depth(rgba, li_depth_buf, w, h, csx, csy, 2.0f, cr, cg,
                               cb, opacity, csd, li_rev_z, li_is_cpu);

      MopVec3 base_center = {pos.x + dir.x * height, pos.y + dir.y * height,
                             pos.z + dir.z * height};
      draw_circle_2d_ex(rgba, li_depth_buf, w, h, base_center, base_r, right, u,
                        &vp_mat, cr, cg, cb, line_w, opacity, li_rev_z,
                        li_is_cpu);

      float angles[4] = {0, 1.5707963f, 3.1415927f, 4.7123890f};
      for (int e = 0; e < 4; e++) {
        float ca = cosf(angles[e]), sa = sinf(angles[e]);
        MopVec3 bp = {base_center.x + (right.x * ca + u.x * sa) * base_r,
                      base_center.y + (right.y * ca + u.y * sa) * base_r,
                      base_center.z + (right.z * ca + u.z * sa) * base_r};
        float psx, psy, pd, bsx, bsy, bd;
        if (project_to_screen_depth(pos, &vp_mat, w, h, &psx, &psy, &pd) &&
            project_to_screen_depth(bp, &vp_mat, w, h, &bsx, &bsy, &bd))
          draw_aa_line_depth(rgba, li_depth_buf, w, h, roundf(psx), roundf(psy),
                             pd, roundf(bsx), roundf(bsy), bd, cr, cg, cb,
                             line_w, opacity, li_rev_z, li_is_cpu);
      }
    }
  }
}

/* =========================================================================
 * 2D screen-space gizmo overlay (anti-aliased lines)
 *
 * Draws translate arrows / rotate rings / scale handles as clean 2D
 * anti-aliased lines directly on the framebuffer.  The 3D geometry
 * handles remain at opacity=0 for picking (object_id buffer).
 * ========================================================================= */

/* Draw a small diamond (4 lines) at a screen-space position */
static void draw_diamond_2d(uint8_t *rgba, int w, int h, float cx, float cy,
                            float radius, uint8_t cr, uint8_t cg, uint8_t cb,
                            float line_w, float opacity) {
  draw_aa_line(rgba, w, h, cx - radius, cy, cx, cy - radius, cr, cg, cb, line_w,
               opacity);
  draw_aa_line(rgba, w, h, cx, cy - radius, cx + radius, cy, cr, cg, cb, line_w,
               opacity);
  draw_aa_line(rgba, w, h, cx + radius, cy, cx, cy + radius, cr, cg, cb, line_w,
               opacity);
  draw_aa_line(rgba, w, h, cx, cy + radius, cx - radius, cy, cr, cg, cb, line_w,
               opacity);
}

/* Forward declarations for stroke letter drawing (defined later in file) */
static void draw_stroke_X(uint8_t *, int, int, float, float, float, float,
                          uint8_t, uint8_t, uint8_t, float);
static void draw_stroke_Y(uint8_t *, int, int, float, float, float, float,
                          uint8_t, uint8_t, uint8_t, float);
static void draw_stroke_Z(uint8_t *, int, int, float, float, float, float,
                          uint8_t, uint8_t, uint8_t, float);
typedef void (*StrokeLetterFn)(uint8_t *, int, int, float, float, float, float,
                               uint8_t, uint8_t, uint8_t, float);
static const StrokeLetterFn gizmo_stroke_letters[3] = {
    draw_stroke_X, draw_stroke_Y, draw_stroke_Z};

void mop_overlay_builtin_gizmo_2d(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp || !vp->gizmo)
    return;
  if (!mop_gizmo_is_visible(vp->gizmo))
    return;

  int w, h;
  const uint8_t *color_ro =
      vp->rhi->framebuffer_read_color(vp->device, vp->framebuffer, &w, &h);
  if (!color_ro || w <= 0 || h <= 0)
    return;
  uint8_t *rgba = (uint8_t *)(uintptr_t)color_ro;

  MopMat4 vp_mat = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);

  MopVec3 pos = mop_gizmo_get_position_internal(vp->gizmo);
  MopGizmoMode mode = mop_gizmo_get_mode(vp->gizmo);
  MopGizmoAxis hover = mop_gizmo_get_hover_axis(vp->gizmo);

  /* Screen-space scaling: constant screen size regardless of distance */
  MopVec3 to_gizmo = {pos.x - vp->cam_eye.x, pos.y - vp->cam_eye.y,
                      pos.z - vp->cam_eye.z};
  float cam_dist = sqrtf(to_gizmo.x * to_gizmo.x + to_gizmo.y * to_gizmo.y +
                         to_gizmo.z * to_gizmo.z);
  if (cam_dist < 0.1f)
    cam_dist = 0.1f;
  float s = cam_dist * 0.15f;
  if (s < 0.3f)
    s = 0.3f;

  float line_w = vp->theme.gizmo_line_width;
  float opacity = vp->theme.gizmo_opacity;
  float ssaa = (float)vp->ssaa_factor;

  /* View rotation for depth sorting (Blender draws back axes first) */
  MopMat4 view_rot = vp->view_matrix;
  view_rot.d[12] = 0.0f;
  view_rot.d[13] = 0.0f;
  view_rot.d[14] = 0.0f;
  view_rot.d[15] = 1.0f;

  /* Compute depth of each axis for back-to-front ordering */
  MopVec3 axis_dirs[3];
  float axis_depth[3];
  for (int a = 0; a < 3; a++) {
    axis_dirs[a] = mop_gizmo_get_axis_dir(vp->gizmo, a);
    MopVec4 d4 = {axis_dirs[a].x, axis_dirs[a].y, axis_dirs[a].z, 0.0f};
    MopVec4 rot = mop_mat4_mul_vec4(view_rot, d4);
    axis_depth[a] = rot.z; /* negative = pointing away */
  }

  /* Sort indices back-to-front (lowest depth first) */
  int draw_order[3] = {0, 1, 2};
  for (int i = 0; i < 2; i++) {
    for (int j = i + 1; j < 3; j++) {
      if (axis_depth[draw_order[j]] < axis_depth[draw_order[i]]) {
        int tmp = draw_order[i];
        draw_order[i] = draw_order[j];
        draw_order[j] = tmp;
      }
    }
  }

  /* Per-axis handles — drawn back-to-front */
  for (int di = 0; di < 3; di++) {
    int a = draw_order[di];
    MopColor c;
    switch (a) {
    case 0:
      c = vp->theme.gizmo_x;
      break;
    case 1:
      c = vp->theme.gizmo_y;
      break;
    default:
      c = vp->theme.gizmo_z;
      break;
    }

    /* Depth-based opacity: back axes slightly dimmer (Blender style) */
    float depth_fade = (axis_depth[a] + 1.0f) * 0.15f + 0.7f;
    if (depth_fade > 1.0f)
      depth_fade = 1.0f;
    if (depth_fade < 0.5f)
      depth_fade = 0.5f;
    float ax_opacity = opacity * depth_fade;

    if ((MopGizmoAxis)a == hover) {
      MopColor hc = vp->theme.gizmo_hover;
      float t = 0.5f;
      c.r += (hc.r - c.r) * t;
      c.g += (hc.g - c.g) * t;
      c.b += (hc.b - c.b) * t;
      ax_opacity = opacity; /* hovered axis always full brightness */
    }
    uint8_t cr = (uint8_t)(c.r * 255.0f);
    uint8_t cg = (uint8_t)(c.g * 255.0f);
    uint8_t cb = (uint8_t)(c.b * 255.0f);

    MopVec3 dir = axis_dirs[a];

    if (mode == MOP_GIZMO_TRANSLATE || mode == MOP_GIZMO_SCALE) {
      /* Shaft from pos + 0.20*s*dir to pos + 1.05*s*dir */
      MopVec3 shaft_s = {pos.x + dir.x * 0.20f * s, pos.y + dir.y * 0.20f * s,
                         pos.z + dir.z * 0.20f * s};
      MopVec3 shaft_e = {pos.x + dir.x * 1.05f * s, pos.y + dir.y * 1.05f * s,
                         pos.z + dir.z * 1.05f * s};
      float ss_x, ss_y, se_x, se_y;
      if (!project_to_screen(shaft_s, &vp_mat, w, h, &ss_x, &ss_y) ||
          !project_to_screen(shaft_e, &vp_mat, w, h, &se_x, &se_y))
        continue;
      /* Pixel alignment */
      ss_x = roundf(ss_x);
      ss_y = roundf(ss_y);
      se_x = roundf(se_x);
      se_y = roundf(se_y);

      draw_aa_line(rgba, w, h, ss_x, ss_y, se_x, se_y, cr, cg, cb, line_w,
                   ax_opacity);

      if (mode == MOP_GIZMO_TRANSLATE) {
        /* Axis ball at tip: filled circle + axis letter (like navigator) */
        MopVec3 tip = {pos.x + dir.x * 1.20f * s, pos.y + dir.y * 1.20f * s,
                       pos.z + dir.z * 1.20f * s};
        float tx, ty;
        if (project_to_screen(tip, &vp_mat, w, h, &tx, &ty)) {
          tx = roundf(tx);
          ty = roundf(ty);
          draw_aa_line(rgba, w, h, se_x, se_y, tx, ty, cr, cg, cb,
                       line_w + 0.5f, ax_opacity);
          float ball_r = 7.0f * ssaa;
          draw_filled_circle(rgba, w, h, tx, ty, ball_r, cr, cg, cb,
                             ax_opacity);
          /* Axis letter (dark text on colored disc) */
          float lsz = 3.0f * ssaa;
          float llw = 1.2f * ssaa;
          gizmo_stroke_letters[a](rgba, w, h, tx, ty, lsz, llw, 15, 15, 15,
                                  ax_opacity * 0.9f);
        }
      } else {
        /* Scale mode: filled square (diamond) at tip */
        draw_diamond_2d(rgba, w, h, se_x, se_y, 4.0f * ssaa, cr, cg, cb, line_w,
                        ax_opacity);
      }
    } else {
      /* Rotate mode: torus ring as a circle */
      MopVec3 up = {0, 1, 0};
      if (fabsf(mop_vec3_dot(dir, up)) > 0.99f)
        up = (MopVec3){0, 0, 1};
      MopVec3 axis_u = mop_vec3_normalize(mop_vec3_cross(up, dir));
      MopVec3 axis_v = mop_vec3_cross(dir, axis_u);

      draw_circle_2d(rgba, w, h, pos, 1.0f * s, axis_u, axis_v, &vp_mat, cr, cg,
                     cb, line_w, ax_opacity);
    }
  }

  /* Center handle: filled circle (Blender uses a filled disc, not a ring) */
  MopColor cc = vp->theme.gizmo_center;
  if (hover == MOP_GIZMO_AXIS_CENTER) {
    MopColor hc = vp->theme.gizmo_hover;
    float t = 0.5f;
    cc.r += (hc.r - cc.r) * t;
    cc.g += (hc.g - cc.g) * t;
    cc.b += (hc.b - cc.b) * t;
  }
  float csx, csy;
  if (project_to_screen(pos, &vp_mat, w, h, &csx, &csy)) {
    csx = roundf(csx);
    csy = roundf(csy);
    uint8_t ccr = (uint8_t)(cc.r * 255.0f);
    uint8_t ccg = (uint8_t)(cc.g * 255.0f);
    uint8_t ccb = (uint8_t)(cc.b * 255.0f);
    draw_filled_circle(rgba, w, h, csx, csy, 3.5f * ssaa, ccr, ccg, ccb,
                       opacity * 0.85f);
  }
}

/* =========================================================================
 * 2D camera object overlay
 *
 * Draws camera frustum wireframes and camera icons for all camera objects
 * in the scene using clean AA lines (same style as gizmo/light overlays).
 * ========================================================================= */

void mop_overlay_builtin_camera_objects(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp || vp->camera_count == 0)
    return;

  int w, h;
  const uint8_t *color_ro =
      vp->rhi->framebuffer_read_color(vp->device, vp->framebuffer, &w, &h);
  if (!color_ro || w <= 0 || h <= 0)
    return;
  uint8_t *rgba = (uint8_t *)(uintptr_t)color_ro;

  const float *depth_buf =
      vp->rhi->framebuffer_read_depth(vp->device, vp->framebuffer, &w, &h);
  bool is_cpu = (vp->backend_type == MOP_BACKEND_CPU);
  bool rev_z = vp->reverse_z && !is_cpu;

  MopColor ac = vp->theme.camera_frustum_color;
  uint8_t cr = (uint8_t)(sqrtf(ac.r) * 255.0f);
  uint8_t cg = (uint8_t)(sqrtf(ac.g) * 255.0f);
  uint8_t cb = (uint8_t)(sqrtf(ac.b) * 255.0f);

  MopMat4 vp_mat = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);
  float line_w = vp->theme.camera_frustum_line_width;
  float opacity = 0.85f;

  for (uint32_t ci = 0; ci < MOP_MAX_CAMERAS; ci++) {
    const struct MopCameraObject *cam = &vp->cameras[ci];
    if (!cam->active)
      continue;
    if (vp->active_camera == cam)
      continue;
    if (!cam->frustum_visible)
      continue;

    /* Blender-style camera: frame rectangle + 4 pyramid lines + up triangle.
     *
     * Build camera local axes from position/target/up, then place the
     * frame at a fixed display distance (drawsize) from the camera origin.
     * The frame is sized by FOV and aspect ratio at that depth. */

    MopVec3 pos = cam->position;
    MopVec3 fwd = {cam->target.x - pos.x, cam->target.y - pos.y,
                   cam->target.z - pos.z};
    float fwd_len = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    if (fwd_len < 1e-6f)
      continue;
    fwd.x /= fwd_len;
    fwd.y /= fwd_len;
    fwd.z /= fwd_len;

    /* Right = normalize(fwd × up) */
    MopVec3 right = {fwd.y * cam->up.z - fwd.z * cam->up.y,
                     fwd.z * cam->up.x - fwd.x * cam->up.z,
                     fwd.x * cam->up.y - fwd.y * cam->up.x};
    float rlen =
        sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    if (rlen < 1e-6f)
      continue;
    right.x /= rlen;
    right.y /= rlen;
    right.z /= rlen;

    /* True up = right × fwd */
    MopVec3 up = {right.y * fwd.z - right.z * fwd.y,
                  right.z * fwd.x - right.x * fwd.z,
                  right.x * fwd.y - right.y * fwd.x};

    /* Frame at drawsize depth, sized by FOV */
    float drawsize = 1.0f;
    float fov_rad = cam->fov_degrees * (3.14159265f / 180.0f);
    float half_h = drawsize * tanf(fov_rad * 0.5f);
    float half_w = half_h * cam->aspect_ratio;

    /* Frame center = pos + fwd * drawsize */
    MopVec3 fc = {pos.x + fwd.x * drawsize, pos.y + fwd.y * drawsize,
                  pos.z + fwd.z * drawsize};

    /* 4 frame corners: fc ± right*half_w ± up*half_h */
    MopVec3 frame[4];
    frame[0] = (MopVec3){fc.x - right.x * half_w - up.x * half_h,
                         fc.y - right.y * half_w - up.y * half_h,
                         fc.z - right.z * half_w - up.z * half_h}; /* BL */
    frame[1] = (MopVec3){fc.x + right.x * half_w - up.x * half_h,
                         fc.y + right.y * half_w - up.y * half_h,
                         fc.z + right.z * half_w - up.z * half_h}; /* BR */
    frame[2] = (MopVec3){fc.x + right.x * half_w + up.x * half_h,
                         fc.y + right.y * half_w + up.y * half_h,
                         fc.z + right.z * half_w + up.z * half_h}; /* TR */
    frame[3] = (MopVec3){fc.x - right.x * half_w + up.x * half_h,
                         fc.y - right.y * half_w + up.y * half_h,
                         fc.z - right.z * half_w + up.z * half_h}; /* TL */

    /* --- Draw frame rectangle (4 edges) with per-pixel depth --- */
    float sx[4], sy[4], sd[4];
    bool frame_ok = true;
    for (int i = 0; i < 4; i++) {
      if (!project_to_screen_depth(frame[i], &vp_mat, w, h, &sx[i], &sy[i],
                                   &sd[i])) {
        frame_ok = false;
        break;
      }
      sx[i] = roundf(sx[i]);
      sy[i] = roundf(sy[i]);
    }

    float psx, psy, psd; /* camera origin screen pos + depth */
    bool pos_ok = project_to_screen_depth(pos, &vp_mat, w, h, &psx, &psy, &psd);
    if (pos_ok) {
      psx = roundf(psx);
      psy = roundf(psy);
    }

    if (frame_ok) {
      for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        draw_aa_line_depth(rgba, depth_buf, w, h, sx[i], sy[i], sd[i], sx[j],
                           sy[j], sd[j], cr, cg, cb, line_w, opacity, rev_z,
                           is_cpu);
      }

      if (pos_ok) {
        for (int i = 0; i < 4; i++)
          draw_aa_line_depth(rgba, depth_buf, w, h, sx[i], sy[i], sd[i], psx,
                             psy, psd, cr, cg, cb, line_w, opacity, rev_z,
                             is_cpu);
      }

      /* --- Up triangle above the top edge (Blender style) --- */
      float tria_size = 0.7f * drawsize;
      float tria_margin = 0.1f * drawsize;

      MopVec3 top_mid = {(frame[2].x + frame[3].x) * 0.5f,
                         (frame[2].y + frame[3].y) * 0.5f,
                         (frame[2].z + frame[3].z) * 0.5f};
      MopVec3 tria_base_center = {top_mid.x + up.x * tria_margin,
                                  top_mid.y + up.y * tria_margin,
                                  top_mid.z + up.z * tria_margin};
      MopVec3 tria_apex = {tria_base_center.x + up.x * tria_size,
                           tria_base_center.y + up.y * tria_size,
                           tria_base_center.z + up.z * tria_size};
      float tria_half_w = tria_size * 0.5f;
      MopVec3 tria_left = {tria_base_center.x - right.x * tria_half_w,
                           tria_base_center.y - right.y * tria_half_w,
                           tria_base_center.z - right.z * tria_half_w};
      MopVec3 tria_right = {tria_base_center.x + right.x * tria_half_w,
                            tria_base_center.y + right.y * tria_half_w,
                            tria_base_center.z + right.z * tria_half_w};

      float tlx, tly, tld, trx, try_, trd, tax, tay, tad;
      if (project_to_screen_depth(tria_left, &vp_mat, w, h, &tlx, &tly, &tld) &&
          project_to_screen_depth(tria_right, &vp_mat, w, h, &trx, &try_,
                                  &trd) &&
          project_to_screen_depth(tria_apex, &vp_mat, w, h, &tax, &tay, &tad)) {
        tlx = roundf(tlx);
        tly = roundf(tly);
        trx = roundf(trx);
        try_ = roundf(try_);
        tax = roundf(tax);
        tay = roundf(tay);
        draw_aa_line_depth(rgba, depth_buf, w, h, tlx, tly, tld, trx, try_, trd,
                           cr, cg, cb, line_w, opacity, rev_z, is_cpu);
        draw_aa_line_depth(rgba, depth_buf, w, h, trx, try_, trd, tax, tay, tad,
                           cr, cg, cb, line_w, opacity, rev_z, is_cpu);
        draw_aa_line_depth(rgba, depth_buf, w, h, tax, tay, tad, tlx, tly, tld,
                           cr, cg, cb, line_w, opacity, rev_z, is_cpu);
      }
    }
  }
}

/* =========================================================================
 * 2D screen-space axis indicator overlay (bottom-left corner navigator)
 *
 * Replaces the geometry-based axis indicator (pass_hud) with clean 2D
 * anti-aliased lines.  Uses the view rotation matrix for orientation.
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * Stroke-based axis label rendering (resolution-independent)
 *
 * Draw "X", "Y", "Z" as AA line strokes.  Each letter is defined as line
 * segments in a ±1 unit box, scaled and centered at the given position.
 * Uses draw_aa_line for identical quality to Blender's polyline shader:
 * linear 1px alpha ramp from perpendicular distance.
 * ------------------------------------------------------------------------- */

static void draw_stroke_X(uint8_t *rgba, int w, int h, float cx, float cy,
                          float sz, float lw, uint8_t cr, uint8_t cg,
                          uint8_t cb, float op) {
  draw_aa_line(rgba, w, h, cx - sz, cy - sz, cx + sz, cy + sz, cr, cg, cb, lw,
               op);
  draw_aa_line(rgba, w, h, cx + sz, cy - sz, cx - sz, cy + sz, cr, cg, cb, lw,
               op);
}

static void draw_stroke_Y(uint8_t *rgba, int w, int h, float cx, float cy,
                          float sz, float lw, uint8_t cr, uint8_t cg,
                          uint8_t cb, float op) {
  draw_aa_line(rgba, w, h, cx - sz, cy - sz, cx, cy, cr, cg, cb, lw, op);
  draw_aa_line(rgba, w, h, cx + sz, cy - sz, cx, cy, cr, cg, cb, lw, op);
  draw_aa_line(rgba, w, h, cx, cy, cx, cy + sz, cr, cg, cb, lw, op);
}

static void draw_stroke_Z(uint8_t *rgba, int w, int h, float cx, float cy,
                          float sz, float lw, uint8_t cr, uint8_t cg,
                          uint8_t cb, float op) {
  draw_aa_line(rgba, w, h, cx - sz, cy - sz, cx + sz, cy - sz, cr, cg, cb, lw,
               op);
  draw_aa_line(rgba, w, h, cx + sz, cy - sz, cx - sz, cy + sz, cr, cg, cb, lw,
               op);
  draw_aa_line(rgba, w, h, cx - sz, cy + sz, cx + sz, cy + sz, cr, cg, cb, lw,
               op);
}

typedef void (*StrokeLetterFn)(uint8_t *, int, int, float, float, float, float,
                               uint8_t, uint8_t, uint8_t, float);
static const StrokeLetterFn stroke_letters[3] = {draw_stroke_X, draw_stroke_Y,
                                                 draw_stroke_Z};

/* Draw a filled anti-aliased circle with optional per-pixel depth testing.
 * ndc_depth is the NDC depth at the circle center. */
static void draw_filled_circle_depth(uint8_t *rgba, const float *depth_buf,
                                     int w, int h, float cx, float cy,
                                     float radius, uint8_t cr, uint8_t cg,
                                     uint8_t cb, float opacity, float ndc_depth,
                                     bool reverse_z, bool is_cpu) {
  int bx0 = (int)(cx - radius - 1.5f);
  int by0 = (int)(cy - radius - 1.5f);
  int bx1 = (int)(cx + radius + 1.5f);
  int by1 = (int)(cy + radius + 1.5f);
  if (bx0 < 0)
    bx0 = 0;
  if (by0 < 0)
    by0 = 0;
  if (bx1 >= w)
    bx1 = w - 1;
  if (by1 >= h)
    by1 = h - 1;
  float gd = is_cpu ? ndc_depth * 0.5f + 0.5f : ndc_depth;
  float bias = 0.002f;
  for (int py = by0; py <= by1; py++) {
    for (int px = bx0; px <= bx1; px++) {
      float fx = (float)px + 0.5f - cx;
      float fy = (float)py + 0.5f - cy;
      float d = sqrtf(fx * fx + fy * fy);
      float a = radius + 0.5f - d;
      if (a <= 0.0f)
        continue;
      if (a > 1.0f)
        a = 1.0f;
      a *= opacity;
      if (a < 0.004f)
        continue;
      /* Per-pixel depth test */
      if (depth_buf) {
        float scene_d = depth_buf[py * w + px];
        bool occluded;
        if (reverse_z)
          occluded = (scene_d > 0.0001f) && (gd + bias < scene_d);
        else
          occluded = (scene_d < 0.9999f) && (gd - bias > scene_d);
        if (occluded)
          continue;
      }
      int idx = (py * w + px) * 4;
      float ia = 1.0f - a;
      rgba[idx + 0] = (uint8_t)((float)rgba[idx + 0] * ia + (float)cr * a);
      rgba[idx + 1] = (uint8_t)((float)rgba[idx + 1] * ia + (float)cg * a);
      rgba[idx + 2] = (uint8_t)((float)rgba[idx + 2] * ia + (float)cb * a);
    }
  }
}

static void draw_filled_circle(uint8_t *rgba, int w, int h, float cx, float cy,
                               float radius, uint8_t cr, uint8_t cg, uint8_t cb,
                               float opacity) {
  draw_filled_circle_depth(rgba, NULL, w, h, cx, cy, radius, cr, cg, cb,
                           opacity, 0.0f, false, false);
}

/* Interpolate two colors: result = a + (b - a) * t */
static inline void lerp_color(float *out, const float *a, const float *b,
                              float t) {
  out[0] = a[0] + (b[0] - a[0]) * t;
  out[1] = a[1] + (b[1] - a[1]) * t;
  out[2] = a[2] + (b[2] - a[2]) * t;
}

void mop_overlay_builtin_axis_indicator_2d(MopViewport *vp, void *user_data) {
  (void)user_data;
  if (!vp)
    return;

  int w, h;
  const uint8_t *color_ro =
      vp->rhi->framebuffer_read_color(vp->device, vp->framebuffer, &w, &h);
  if (!color_ro || w <= 0 || h <= 0)
    return;
  uint8_t *rgba = (uint8_t *)(uintptr_t)color_ro;

  /* View rotation only (strip translation) */
  MopMat4 view_rot = vp->view_matrix;
  view_rot.d[12] = 0.0f;
  view_rot.d[13] = 0.0f;
  view_rot.d[14] = 0.0f;
  view_rot.d[15] = 1.0f;

  /* Pixel-aligned center position (Blender uses roundf for all screen coords)
   */
  float cx = roundf(0.09f * (float)w);
  float cy = roundf(0.89f * (float)h);
  float scale = fminf((float)w, (float)h) * 0.06f;
  float ssaa = (float)vp->ssaa_factor;

  /* Background color (viewport bg for depth desaturation) */
  float bg_col[3] = {sqrtf(vp->theme.bg_bottom.r) * 255.0f,
                     sqrtf(vp->theme.bg_bottom.g) * 255.0f,
                     sqrtf(vp->theme.bg_bottom.b) * 255.0f};

  /* Axis colors as floats for interpolation */
  float axis_col[3][3] = {
      {vp->theme.axis_x.r * 255.0f, vp->theme.axis_x.g * 255.0f,
       vp->theme.axis_x.b * 255.0f},
      {vp->theme.axis_y.r * 255.0f, vp->theme.axis_y.g * 255.0f,
       vp->theme.axis_y.b * 255.0f},
      {vp->theme.axis_z.r * 255.0f, vp->theme.axis_z.g * 255.0f,
       vp->theme.axis_z.b * 255.0f},
  };

  MopVec3 dirs[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

  /* Build 6 axis entries with Blender's depth-based properties */
  struct {
    float sx, sy; /* pixel-aligned screen tip */
    float depth;  /* view-space Z: -1=back, +1=front */
    float col[3]; /* depth-faded sRGB color */
    float ball_r; /* circle radius with size parallax */
    bool positive;
    int axis;
  } axes[6];

  for (int a = 0; a < 3; a++) {
    MopVec4 dir4 = {dirs[a].x, dirs[a].y, dirs[a].z, 0.0f};
    MopVec4 rot = mop_mat4_mul_vec4(view_rot, dir4);

    /* Positive axis */
    float depth_pos = rot.z; /* -1..+1 */
    axes[a].sx = roundf(cx + rot.x * scale);
    axes[a].sy = roundf(cy - rot.y * scale);
    axes[a].depth = depth_pos;
    axes[a].positive = true;
    axes[a].axis = a;

    /* Blender color fading: lerp(bg, axis_color, (depth+1)*0.25 + 0.5)
     * Maps depth in [-1,+1] to lerp factor [0.5, 1.0] — back desaturates
     * toward background, front is fully saturated */
    float fade_t = (depth_pos + 1.0f) * 0.25f + 0.5f;
    lerp_color(axes[a].col, bg_col, axis_col[a], fade_t);

    /* Blender size parallax: scale = (depth+1)*0.08 + 0.92
     * Gives ±8% size variation from back to front */
    float size_scale = (depth_pos + 1.0f) * 0.08f + 0.92f;
    axes[a].ball_r = 8.0f * ssaa * size_scale;

    /* Negative axis */
    float depth_neg = -rot.z;
    axes[a + 3].sx = roundf(cx - rot.x * scale);
    axes[a + 3].sy = roundf(cy + rot.y * scale);
    axes[a + 3].depth = depth_neg;
    axes[a + 3].positive = false;
    axes[a + 3].axis = a;

    float fade_t_neg = (depth_neg + 1.0f) * 0.25f + 0.5f;
    /* Negative axes: same color as positive (no pale tint) */
    lerp_color(axes[a + 3].col, bg_col, axis_col[a], fade_t_neg);

    float size_scale_neg = (depth_neg + 1.0f) * 0.08f + 0.92f;
    axes[a + 3].ball_r = 8.0f * ssaa * size_scale_neg;
  }

  /* Depth sort back-to-front (Blender uses qsort on float depth) */
  int order[6] = {0, 1, 2, 3, 4, 5};
  for (int i = 0; i < 5; i++) {
    for (int j = i + 1; j < 6; j++) {
      if (axes[order[j]].depth < axes[order[i]].depth) {
        int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
      }
    }
  }

  /* Draw back-to-front */
  for (int i = 0; i < 6; i++) {
    int ai = order[i];
    uint8_t r8 = (uint8_t)fminf(255.0f, fmaxf(0.0f, axes[ai].col[0]));
    uint8_t g8 = (uint8_t)fminf(255.0f, fmaxf(0.0f, axes[ai].col[1]));
    uint8_t b8 = (uint8_t)fminf(255.0f, fmaxf(0.0f, axes[ai].col[2]));
    float lw = axes[ai].positive ? 4.0f : 3.5f;

    /* Shaft line — depth-faded color */
    draw_aa_line(rgba, w, h, cx, cy, axes[ai].sx, axes[ai].sy, r8, g8, b8, lw,
                 0.9f);

    if (axes[ai].positive) {
      /* Filled circle at tip */
      draw_filled_circle(rgba, w, h, axes[ai].sx, axes[ai].sy, axes[ai].ball_r,
                         r8, g8, b8, 0.95f);

      /* Stroke-based letter label (dark text on colored disc) */
      float letter_sz = 3.4f * ssaa;
      float letter_lw = 1.4f * ssaa;
      stroke_letters[axes[ai].axis](rgba, w, h, axes[ai].sx, axes[ai].sy,
                                    letter_sz, letter_lw, 15, 15, 15, 0.88f);
    } else {
      /* Filled circle at negative tip — no text, just circle */
      draw_filled_circle(rgba, w, h, axes[ai].sx, axes[ai].sy, axes[ai].ball_r,
                         r8, g8, b8, 0.85f);
    }
  }
}
