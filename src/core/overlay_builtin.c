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
  float grid_half = 7.5f; /* 15x15 */

  /* Accent color → sRGB */
  MopColor ac = vp->theme.accent;
  uint8_t cr = (uint8_t)(sqrtf(ac.r) * 255.0f);
  uint8_t cg_c = (uint8_t)(sqrtf(ac.g) * 255.0f);
  uint8_t cb = (uint8_t)(sqrtf(ac.b) * 255.0f);

  /* Dimmed accent for minor grid lines */
  uint8_t dr = (uint8_t)(sqrtf(ac.r * 0.25f) * 255.0f);
  uint8_t dg = (uint8_t)(sqrtf(ac.g * 0.25f) * 255.0f);
  uint8_t db = (uint8_t)(sqrtf(ac.b * 0.25f) * 255.0f);

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

      /* --- Three grid levels: subgrid (1/3), major (1), axis (0) ---
       * For each: compute proximity in pixels, then smoothstep AA.
       * proximity_px = world_distance_to_line / fwidth */

      /* Subgrid: every 1/3 unit */
      float sub3x = wx * 3.0f, sub3z = wz * 3.0f;
      float sub_px_x = fabsf(sub3x - roundf(sub3x)) / (3.0f * fw_x);
      float sub_px_z = fabsf(sub3z - roundf(sub3z)) / (3.0f * fw_z);
      float a_sub = ov_smoothstep(0.0f, 1.5f, fminf(sub_px_x, sub_px_z));

      /* Major grid: every 1 unit */
      float maj_px_x = fabsf(wx - roundf(wx)) / fw_x;
      float maj_px_z = fabsf(wz - roundf(wz)) / fw_z;
      float a_maj = ov_smoothstep(0.0f, 1.5f, fminf(maj_px_x, maj_px_z));

      /* Axis: X-axis (wz=0), Z-axis (wx=0) — 2px wide */
      float ax_px_x = fabsf(wz) / fw_z;
      float ax_px_z = fabsf(wx) / fw_x;
      float a_ax = ov_smoothstep(0.5f, 2.0f, fminf(ax_px_x, ax_px_z));

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

      /* Edge fade — last 1.5 units of grid extent */
      float edge = fmaxf(fabsf(wx), fabsf(wz));
      float edge_fade = ov_smoothstep(grid_half - 1.5f, grid_half, edge);

      /* Pick dominant level: axis > major > subgrid */
      float alpha;
      uint8_t fr, fg, fb;
      if (a_ax > 0.01f) {
        alpha = a_ax * 0.9f;
        fr = cr;
        fg = cg_c;
        fb = cb;
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

/* Project world point to screen pixel coordinates. Returns false if behind
 * camera. */
static bool project_to_screen(MopVec3 p, const MopMat4 *vp_mat, int w, int h,
                              float *sx, float *sy) {
  MopVec4 clip = mop_mat4_mul_vec4(*vp_mat, (MopVec4){p.x, p.y, p.z, 1.0f});
  if (clip.w <= 0.001f)
    return false;
  float nx = clip.x / clip.w;
  float ny = clip.y / clip.w;
  *sx = (nx * 0.5f + 0.5f) * (float)w;
  *sy = (1.0f - (ny * 0.5f + 0.5f)) * (float)h;
  return true;
}

/* Draw a circle as line segments between projected points */
static void draw_circle_2d(uint8_t *rgba, int w, int h, MopVec3 center,
                           float radius, MopVec3 axis_u, MopVec3 axis_v,
                           const MopMat4 *vp_mat, uint8_t cr, uint8_t cg,
                           uint8_t cb, float line_w, float opacity) {
  int segs = 24;
  float prev_sx, prev_sy;
  bool prev_ok = false;
  for (int i = 0; i <= segs; i++) {
    float a = (float)i * 2.0f * 3.14159265f / (float)segs;
    float ca = cosf(a), sa = sinf(a);
    MopVec3 pt = {center.x + (axis_u.x * ca + axis_v.x * sa) * radius,
                  center.y + (axis_u.y * ca + axis_v.y * sa) * radius,
                  center.z + (axis_u.z * ca + axis_v.z * sa) * radius};
    float cur_sx, cur_sy;
    bool cur_ok = project_to_screen(pt, vp_mat, w, h, &cur_sx, &cur_sy);
    if (prev_ok && cur_ok)
      draw_aa_line(rgba, w, h, prev_sx, prev_sy, cur_sx, cur_sy, cr, cg, cb,
                   line_w, opacity);
    prev_sx = cur_sx;
    prev_sy = cur_sy;
    prev_ok = cur_ok;
  }
}

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

  /* Light indicators use theme accent (UI object color) */
  MopColor lac = vp->theme.accent;
  uint8_t cr = (uint8_t)(sqrtf(lac.r) * 255.0f);
  uint8_t cg = (uint8_t)(sqrtf(lac.g) * 255.0f);
  uint8_t cb = (uint8_t)(sqrtf(lac.b) * 255.0f);

  MopMat4 vp_mat = mop_mat4_multiply(vp->projection_matrix, vp->view_matrix);

  float line_w = 1.5f;
  float opacity = 0.9f;

  for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
    if (!vp->lights[i].active)
      continue;
    const MopLight *light = &vp->lights[i];

    /* Compute indicator world position */
    MopVec3 pos;
    if (light->type == MOP_LIGHT_DIRECTIONAL) {
      MopVec3 dir = mop_vec3_normalize(light->direction);
      pos = mop_vec3_add(vp->cam_target, mop_vec3_scale(dir, 3.0f));
    } else {
      pos = light->position;
    }

    /* Fixed world-size — indicators scale with the scene like regular objects
     */
    (void)vp;
    float s = 1.0f;

    float csx, csy;
    if (!project_to_screen(pos, &vp_mat, w, h, &csx, &csy))
      continue;

    if (light->type == MOP_LIGHT_POINT) {
      /* 6 rays: ±X, ±Y, ±Z */
      float inner = 0.04f * s, outer = 0.14f * s;
      static const float dirs[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
      for (int r = 0; r < 6; r++) {
        MopVec3 d = {dirs[r][0], dirs[r][1], dirs[r][2]};
        MopVec3 a = {pos.x + d.x * inner, pos.y + d.y * inner,
                     pos.z + d.z * inner};
        MopVec3 b = {pos.x + d.x * outer, pos.y + d.y * outer,
                     pos.z + d.z * outer};
        float ax, ay, bx, by;
        if (project_to_screen(a, &vp_mat, w, h, &ax, &ay) &&
            project_to_screen(b, &vp_mat, w, h, &bx, &by))
          draw_aa_line(rgba, w, h, ax, ay, bx, by, cr, cg, cb, line_w, opacity);
      }
    } else if (light->type == MOP_LIGHT_DIRECTIONAL) {
      /* Circle in XY plane + 4 rays along direction */
      float radius = 0.10f * s;
      float ray_len = 0.30f * s;
      MopVec3 dir = mop_vec3_normalize(light->direction);
      MopVec3 up = {0, 1, 0};
      if (fabsf(mop_vec3_dot(dir, up)) > 0.99f)
        up = (MopVec3){1, 0, 0};
      MopVec3 right = mop_vec3_normalize(mop_vec3_cross(up, dir));
      MopVec3 u = mop_vec3_cross(dir, right);

      draw_circle_2d(rgba, w, h, pos, radius, right, u, &vp_mat, cr, cg, cb,
                     line_w, opacity);

      /* 4 rays */
      float angles[4] = {0, 1.5707963f, 3.1415927f, 4.7123890f};
      for (int r = 0; r < 4; r++) {
        float ca = cosf(angles[r]), sa = sinf(angles[r]);
        MopVec3 base = {pos.x + (right.x * ca + u.x * sa) * radius,
                        pos.y + (right.y * ca + u.y * sa) * radius,
                        pos.z + (right.z * ca + u.z * sa) * radius};
        MopVec3 tip = {base.x + dir.x * ray_len, base.y + dir.y * ray_len,
                       base.z + dir.z * ray_len};
        float bsx, bsy, tsx, tsy;
        if (project_to_screen(base, &vp_mat, w, h, &bsx, &bsy) &&
            project_to_screen(tip, &vp_mat, w, h, &tsx, &tsy))
          draw_aa_line(rgba, w, h, bsx, bsy, tsx, tsy, cr, cg, cb, line_w,
                       opacity);
      }
    } else if (light->type == MOP_LIGHT_SPOT) {
      /* Cone: apex at position, base circle + 4 edge lines */
      float base_r = 0.15f * s;
      float height = 0.30f * s;
      MopVec3 dir = mop_vec3_normalize(light->direction);
      MopVec3 up = {0, 1, 0};
      if (fabsf(mop_vec3_dot(dir, up)) > 0.99f)
        up = (MopVec3){1, 0, 0};
      MopVec3 right = mop_vec3_normalize(mop_vec3_cross(up, dir));
      MopVec3 u = mop_vec3_cross(dir, right);

      MopVec3 base_center = {pos.x + dir.x * height, pos.y + dir.y * height,
                             pos.z + dir.z * height};
      draw_circle_2d(rgba, w, h, base_center, base_r, right, u, &vp_mat, cr, cg,
                     cb, line_w, opacity);

      /* 4 edge lines from apex to base ring */
      float angles[4] = {0, 1.5707963f, 3.1415927f, 4.7123890f};
      for (int e = 0; e < 4; e++) {
        float ca = cosf(angles[e]), sa = sinf(angles[e]);
        MopVec3 bp = {base_center.x + (right.x * ca + u.x * sa) * base_r,
                      base_center.y + (right.y * ca + u.y * sa) * base_r,
                      base_center.z + (right.z * ca + u.z * sa) * base_r};
        float psx, psy, bsx, bsy;
        if (project_to_screen(pos, &vp_mat, w, h, &psx, &psy) &&
            project_to_screen(bp, &vp_mat, w, h, &bsx, &bsy))
          draw_aa_line(rgba, w, h, psx, psy, bsx, bsy, cr, cg, cb, line_w,
                       opacity);
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

/* Draw an arrowhead "V" at a screen-space tip position */
static void draw_arrowhead_2d(uint8_t *rgba, int w, int h, float tip_x,
                              float tip_y, float from_x, float from_y,
                              uint8_t cr, uint8_t cg, uint8_t cb, float line_w,
                              float opacity, float wing_len) {
  float dx = tip_x - from_x, dy = tip_y - from_y;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f)
    return;
  float ux = dx / len, uy = dy / len;
  float bx = -ux, by = -uy;
  /* ±25° wings */
  float cos25 = 0.906f, sin25 = 0.423f;
  float lx = bx * cos25 - by * sin25;
  float ly = bx * sin25 + by * cos25;
  float rx = bx * cos25 + by * sin25;
  float ry = -bx * sin25 + by * cos25;
  draw_aa_line(rgba, w, h, tip_x, tip_y, tip_x + lx * wing_len,
               tip_y + ly * wing_len, cr, cg, cb, line_w, opacity);
  draw_aa_line(rgba, w, h, tip_x, tip_y, tip_x + rx * wing_len,
               tip_y + ry * wing_len, cr, cg, cb, line_w, opacity);
}

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

  /* Fixed world-size — gizmo scales with the scene like regular objects */
  float s = 1.2f;

  float line_w = vp->theme.gizmo_line_width;
  float opacity = vp->theme.gizmo_opacity;
  float ssaa = (float)vp->ssaa_factor;

  /* Per-axis handles */
  for (int a = 0; a < 3; a++) {
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
    if ((MopGizmoAxis)a == hover) {
      MopColor hc = vp->theme.gizmo_hover;
      float t = 0.5f;
      c.r += (hc.r - c.r) * t;
      c.g += (hc.g - c.g) * t;
      c.b += (hc.b - c.b) * t;
    }
    uint8_t cr = (uint8_t)(c.r * 255.0f);
    uint8_t cg = (uint8_t)(c.g * 255.0f);
    uint8_t cb = (uint8_t)(c.b * 255.0f);

    MopVec3 dir = mop_gizmo_get_axis_dir(vp->gizmo, a);

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

      draw_aa_line(rgba, w, h, ss_x, ss_y, se_x, se_y, cr, cg, cb, line_w,
                   opacity);

      if (mode == MOP_GIZMO_TRANSLATE) {
        /* Arrowhead at tip */
        MopVec3 tip = {pos.x + dir.x * 1.20f * s, pos.y + dir.y * 1.20f * s,
                       pos.z + dir.z * 1.20f * s};
        float tx, ty;
        if (project_to_screen(tip, &vp_mat, w, h, &tx, &ty)) {
          draw_aa_line(rgba, w, h, se_x, se_y, tx, ty, cr, cg, cb,
                       line_w + 0.5f, opacity);
          draw_arrowhead_2d(rgba, w, h, tx, ty, se_x, se_y, cr, cg, cb, line_w,
                            opacity, 8.0f * ssaa);
        }
      } else {
        /* Scale mode: diamond at tip */
        draw_diamond_2d(rgba, w, h, se_x, se_y, 4.0f * ssaa, cr, cg, cb, line_w,
                        opacity);
      }
    } else {
      /* Rotate mode: torus ring as a circle */
      MopVec3 up = {0, 1, 0};
      if (fabsf(mop_vec3_dot(dir, up)) > 0.99f)
        up = (MopVec3){0, 0, 1};
      MopVec3 axis_u = mop_vec3_normalize(mop_vec3_cross(up, dir));
      MopVec3 axis_v = mop_vec3_cross(dir, axis_u);

      draw_circle_2d(rgba, w, h, pos, 1.0f * s, axis_u, axis_v, &vp_mat, cr, cg,
                     cb, line_w, opacity);
    }
  }

  /* Center handle: small circle */
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
    uint8_t ccr = (uint8_t)(cc.r * 255.0f);
    uint8_t ccg = (uint8_t)(cc.g * 255.0f);
    uint8_t ccb = (uint8_t)(cc.b * 255.0f);
    float r = 3.5f * ssaa;
    int segs = 8;
    for (int i = 0; i < segs; i++) {
      float a0 = (float)i * 2.0f * 3.14159265f / (float)segs;
      float a1 = (float)(i + 1) * 2.0f * 3.14159265f / (float)segs;
      draw_aa_line(rgba, w, h, csx + cosf(a0) * r, csy + sinf(a0) * r,
                   csx + cosf(a1) * r, csy + sinf(a1) * r, ccr, ccg, ccb,
                   line_w, opacity);
    }
  }
}

/* =========================================================================
 * 2D screen-space axis indicator overlay (bottom-left corner navigator)
 *
 * Replaces the geometry-based axis indicator (pass_hud) with clean 2D
 * anti-aliased lines.  Uses the view rotation matrix for orientation.
 * ========================================================================= */

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

  /* Match geometry HUD position: NDC center = (-0.82, -0.78)
   * NDC to screen: sx = (ndc_x * 0.5 + 0.5) * w
   *                sy = (1.0 - (ndc_y * 0.5 + 0.5)) * h */
  float cx = 0.09f * (float)w;
  float cy = 0.89f * (float)h;
  float scale_x = 0.06f * (float)w;
  float scale_y = 0.06f * (float)h;

  MopColor colors[3] = {vp->theme.axis_x, vp->theme.axis_y, vp->theme.axis_z};
  MopVec3 dirs[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

  float line_w = 1.5f;
  float opacity = 0.9f;
  float ssaa = (float)vp->ssaa_factor;

  for (int a = 0; a < 3; a++) {
    /* Transform axis direction by view rotation */
    MopVec4 dir4 = {dirs[a].x, dirs[a].y, dirs[a].z, 0.0f};
    MopVec4 rot = mop_mat4_mul_vec4(view_rot, dir4);

    float sx = cx + rot.x * scale_x;
    float sy = cy - rot.y * scale_y; /* Y inverted for screen */

    uint8_t cr = (uint8_t)(colors[a].r * 255.0f);
    uint8_t cg = (uint8_t)(colors[a].g * 255.0f);
    uint8_t cb = (uint8_t)(colors[a].b * 255.0f);

    draw_aa_line(rgba, w, h, cx, cy, sx, sy, cr, cg, cb, line_w, opacity);
    draw_arrowhead_2d(rgba, w, h, sx, sy, cx, cy, cr, cg, cb, line_w, opacity,
                      5.0f * ssaa);
  }
}
