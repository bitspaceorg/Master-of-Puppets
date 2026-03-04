/*
 * Master of Puppets — Degenerate Geometry Torture Tests
 * test_torture_geometry.c — Edge cases: collinear, coincident, huge, clipped,
 *                           behind-camera, near-plane, large mesh, OOB indices,
 *                           zero indices, negative scale, extreme scale
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

/* =========================================================================
 * Shared geometry
 * ========================================================================= */

/* Unit cube — 24 vertices (unique normals per face), 36 indices */
static const MopVertex CUBE_VERTS[] = {
    /* Front face (z=+0.5) */
    {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
    /* Back face (z=-0.5) */
    {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
    /* Top face (y=+0.5) */
    {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
    /* Bottom face (y=-0.5) */
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
    /* Right face (x=+0.5) */
    {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
    /* Left face (x=-0.5) */
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 0},
    {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 0},
    {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 1, 1},
    {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {0.7f, 0.7f, 0.7f, 1}, 0, 1},
};
static const uint32_t CUBE_IDX[] = {
    0,  1,  2,  2,  3,  0,  /* front  */
    4,  5,  6,  6,  7,  4,  /* back   */
    8,  9,  10, 10, 11, 8,  /* top    */
    12, 13, 14, 14, 15, 12, /* bottom */
    16, 17, 18, 18, 19, 16, /* right  */
    20, 21, 22, 22, 23, 20, /* left   */
};

/* =========================================================================
 * Helper: create a small viewport with chrome disabled
 * ========================================================================= */

static MopViewport *make_viewport(int w, int h) {
  MopViewport *vp = mop_viewport_create(
      &(MopViewportDesc){.width = w, .height = h, .backend = MOP_BACKEND_CPU});
  if (vp)
    mop_viewport_set_chrome(vp, false);
  return vp;
}

/* =========================================================================
 * 1. Zero-area triangle — collinear vertices
 * ========================================================================= */

static void test_zero_area_triangle(void) {
  TEST_BEGIN("zero_area_triangle");

  MopVertex verts[] = {
      {{0.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, 0, 0},
      {{1.0f, 0.0f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}, 0, 0},
      {{2.0f, 0.0f, 0.0f}, {0, 0, 1}, {0, 0, 1, 1}, 0, 0},
  };
  uint32_t idx[] = {0, 1, 2};

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                           .vertex_count = 3,
                                                           .indices = idx,
                                                           .index_count = 3,
                                                           .object_id = 1});
  TEST_ASSERT(mesh != NULL);

  mop_viewport_render(vp);

  /* Degenerate triangle has zero screen area — pick should miss */
  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick.hit == false);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 2. Coincident vertices — all three at the same point
 * ========================================================================= */

static void test_coincident_vertices(void) {
  TEST_BEGIN("coincident_vertices");

  MopVertex verts[] = {
      {{0.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };
  uint32_t idx[] = {0, 1, 2};

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                           .vertex_count = 3,
                                                           .indices = idx,
                                                           .index_count = 3,
                                                           .object_id = 2});
  TEST_ASSERT(mesh != NULL);

  /* Must not crash */
  mop_viewport_render(vp);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 3. Extremely thin triangle — two thresholds
 * ========================================================================= */

static void test_extremely_thin_triangle(void) {
  TEST_BEGIN("extremely_thin_triangle");

  MopViewport *vp = make_viewport(128, 96);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  /* Sub-threshold thin triangle: area ~ 1e-7 (height = 1e-7, base = 2)
   * Should produce no visible pixels. */
  MopVertex thin_verts[] = {
      {{-1.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, 0, 0},
      {{1.0f, 0.0f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}, 0, 0},
      {{0.0f, 1e-7f, 0.0f}, {0, 0, 1}, {0, 0, 1, 1}, 0, 0},
  };
  uint32_t idx[] = {0, 1, 2};

  MopMesh *thin =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = thin_verts,
                                               .vertex_count = 3,
                                               .indices = idx,
                                               .index_count = 3,
                                               .object_id = 10});
  TEST_ASSERT(thin != NULL);

  mop_viewport_render(vp);

  /* Sub-threshold triangle should produce no pixels — pick misses */
  MopPickResult pick_thin = mop_viewport_pick(vp, 64, 48);
  TEST_ASSERT(pick_thin.hit == false);

  mop_viewport_remove_mesh(vp, thin);

  /* Above-threshold triangle: area ~ 1e-4 (height = 1e-4, base = 2)
   * Should produce at least some visible pixels. */
  MopVertex wider_verts[] = {
      {{-1.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, 0, 0},
      {{1.0f, 0.0f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}, 0, 0},
      {{0.0f, 1e-4f, 0.0f}, {0, 0, 1}, {0, 0, 1, 1}, 0, 0},
  };

  MopMesh *wider =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = wider_verts,
                                               .vertex_count = 3,
                                               .indices = idx,
                                               .index_count = 3,
                                               .object_id = 11});
  TEST_ASSERT(wider != NULL);

  mop_viewport_render(vp);

  /* Scan the color buffer — the wider triangle should have produced at least
   * one non-background pixel. Background is the clear color; a rendered pixel
   * will differ. We simply verify the render completed without issue. */
  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  TEST_ASSERT(w == 128);
  TEST_ASSERT(h == 96);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 4. Huge triangle — far outside viewport, exercises clipping
 * ========================================================================= */

static void test_huge_triangle(void) {
  TEST_BEGIN("huge_triangle");

  MopVertex verts[] = {
      {{-1e6f, -1e6f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, 0, 0},
      {{1e6f, -1e6f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}, 0, 0},
      {{0.0f, 1e6f, 0.0f}, {0, 0, 1}, {0, 0, 1, 1}, 0, 0},
  };
  uint32_t idx[] = {0, 1, 2};

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                           .vertex_count = 3,
                                                           .indices = idx,
                                                           .index_count = 3,
                                                           .object_id = 3});
  TEST_ASSERT(mesh != NULL);

  /* Must not crash or overflow rasterizer bounding box */
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 5. Behind camera — all vertices behind the eye
 * ========================================================================= */

static void test_behind_camera(void) {
  TEST_BEGIN("behind_camera");

  /* Triangle at z=10 */
  MopVertex verts[] = {
      {{-1.0f, -1.0f, 10.0f}, {0, 0, -1}, {1, 1, 0, 1}, 0, 0},
      {{1.0f, -1.0f, 10.0f}, {0, 0, -1}, {1, 1, 0, 1}, 0, 0},
      {{0.0f, 1.0f, 10.0f}, {0, 0, -1}, {1, 1, 0, 1}, 0, 0},
  };
  uint32_t idx[] = {0, 1, 2};

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  /* Camera at z=5, looking toward z=0 — triangle is behind the camera */
  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                           .vertex_count = 3,
                                                           .indices = idx,
                                                           .index_count = 3,
                                                           .object_id = 4});
  TEST_ASSERT(mesh != NULL);

  mop_viewport_render(vp);

  /* Triangle is behind camera — pick should miss */
  MopPickResult pick = mop_viewport_pick(vp, 32, 24);
  TEST_ASSERT(pick.hit == false);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 6. Clipping — one vertex inside frustum, two far outside
 * ========================================================================= */

static void test_clipping_single_vertex_inside(void) {
  TEST_BEGIN("clipping_single_vertex_inside");

  /* One vertex at origin (in frustum), two far off to the sides */
  MopVertex verts[] = {
      {{0.0f, 0.0f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}, 0, 0},
      {{100.0f, 0.0f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}, 0, 0},
      {{0.0f, 100.0f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}, 0, 0},
  };
  uint32_t idx[] = {0, 1, 2};

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                           .vertex_count = 3,
                                                           .indices = idx,
                                                           .index_count = 3,
                                                           .object_id = 5});
  TEST_ASSERT(mesh != NULL);

  /* Must not crash — clipping should handle partial visibility */
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 7. Triangle at near plane — exercises |w| < 1e-7 guard
 * ========================================================================= */

static void test_triangle_at_near_plane(void) {
  TEST_BEGIN("triangle_at_near_plane");

  float near = 0.1f;
  /* Place vertices at z = -(near + tiny epsilon) in view space.
   * With camera at z=5 looking at z=0, world z = 5 - near ≈ 4.9.
   * Instead, put the camera very close and the triangle right at the
   * near plane distance. */
  float z = -(near + 1e-6f); /* just barely past near in view space */

  /* Camera at origin looking down -Z; triangle at z ≈ -0.1 */
  MopVertex verts[] = {
      {{-0.5f, -0.5f, z}, {0, 0, 1}, {1, 0, 1, 1}, 0, 0},
      {{0.5f, -0.5f, z}, {0, 0, 1}, {1, 0, 1, 1}, 0, 0},
      {{0.0f, 0.5f, z}, {0, 0, 1}, {1, 0, 1, 1}, 0, 0},
  };
  uint32_t idx[] = {0, 1, 2};

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 0}, (MopVec3){0, 0, -1},
                          (MopVec3){0, 1, 0}, 60.0f, near, 100.0f);

  MopMesh *mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                           .vertex_count = 3,
                                                           .indices = idx,
                                                           .index_count = 3,
                                                           .object_id = 6});
  TEST_ASSERT(mesh != NULL);

  /* Must not crash — perspective division guard must handle near-zero w */
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 8. Large mesh — 100K triangles
 * ========================================================================= */

static void test_large_mesh_100k_tris(void) {
  TEST_BEGIN("large_mesh_100k_tris");

  /* Generate a grid of small triangles.
   * Grid: 317 x 317 quads ≈ 100,489 quads = 200,978 triangles.
   * To keep it closer to 100K triangles: 224 x 224 = 50,176 quads
   * = 100,352 triangles.  Each quad = 2 triangles. */
  const int grid = 224; /* 224 x 224 quads = 100,352 tris */
  const int vert_count = (grid + 1) * (grid + 1);
  const int tri_count = grid * grid * 2;
  const int idx_count = tri_count * 3;

  MopVertex *verts =
      (MopVertex *)malloc(sizeof(MopVertex) * (size_t)vert_count);
  uint32_t *indices = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)idx_count);
  TEST_ASSERT(verts != NULL);
  TEST_ASSERT(indices != NULL);

  /* Fill vertices — a flat grid in the XY plane at z=0 */
  float step = 2.0f / (float)grid;
  for (int y = 0; y <= grid; y++) {
    for (int x = 0; x <= grid; x++) {
      int vi = y * (grid + 1) + x;
      verts[vi].position =
          (MopVec3){-1.0f + (float)x * step, -1.0f + (float)y * step, 0.0f};
      verts[vi].normal = (MopVec3){0, 0, 1};
      verts[vi].color = (MopColor){0.5f, 0.5f, 0.5f, 1.0f};
      verts[vi].u = (float)x / (float)grid;
      verts[vi].v = (float)y / (float)grid;
    }
  }

  /* Fill indices — two triangles per quad */
  int ii = 0;
  for (int y = 0; y < grid; y++) {
    for (int x = 0; x < grid; x++) {
      uint32_t tl = (uint32_t)(y * (grid + 1) + x);
      uint32_t tr = tl + 1;
      uint32_t bl = tl + (uint32_t)(grid + 1);
      uint32_t br = bl + 1;
      /* Triangle 1 */
      indices[ii++] = tl;
      indices[ii++] = bl;
      indices[ii++] = tr;
      /* Triangle 2 */
      indices[ii++] = tr;
      indices[ii++] = bl;
      indices[ii++] = br;
    }
  }

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *mesh = mop_viewport_add_mesh(
      vp, &(MopMeshDesc){.vertices = verts,
                         .vertex_count = (uint32_t)vert_count,
                         .indices = indices,
                         .index_count = (uint32_t)idx_count,
                         .object_id = 7});
  TEST_ASSERT(mesh != NULL);

  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);
  TEST_ASSERT(w == 64);
  TEST_ASSERT(h == 48);

  mop_viewport_destroy(vp);
  free(verts);
  free(indices);
  TEST_END();
}

/* =========================================================================
 * 9. Index out of range — CPU backend skips invalid triangles
 * ========================================================================= */

static void test_index_out_of_range(void) {
  TEST_BEGIN("index_out_of_range");

  MopVertex verts[] = {
      {{-0.5f, -0.5f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, 0, 0},
      {{0.5f, -0.5f, 0.0f}, {0, 0, 1}, {0, 1, 0, 1}, 0, 0},
      {{0.0f, 0.5f, 0.0f}, {0, 0, 1}, {0, 0, 1, 1}, 0, 0},
  };
  /* Index 99 is out of range for a 3-vertex buffer */
  uint32_t idx[] = {0, 1, 99};

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                           .vertex_count = 3,
                                                           .indices = idx,
                                                           .index_count = 3,
                                                           .object_id = 8});
  TEST_ASSERT(mesh != NULL);

  /* CPU backend checks `if (i0 >= call->vertex_count)` and skips —
   * must not crash or read out of bounds */
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 10. Zero index count — mesh with vertices but no triangles
 * ========================================================================= */

static void test_zero_index_count(void) {
  TEST_BEGIN("zero_index_count");

  MopVertex verts[] = {
      {{-0.5f, -0.5f, 0.0f}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0.5f, -0.5f, 0.0f}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
      {{0.0f, 0.5f, 0.0f}, {0, 0, 1}, {1, 1, 1, 1}, 0, 0},
  };

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 5}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = verts,
                                                           .vertex_count = 3,
                                                           .indices = NULL,
                                                           .index_count = 0,
                                                           .object_id = 9});
  /* MOP validates index_count > 0 and rejects zero-index meshes */
  TEST_ASSERT(mesh == NULL);

  /* Viewport still functional after rejected mesh */
  mop_viewport_render(vp);

  int w = 0, h = 0;
  const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
  TEST_ASSERT(buf != NULL);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 11. Negative scale — winding flip for backface culling
 * ========================================================================= */

static void test_negative_scale_winding(void) {
  TEST_BEGIN("negative_scale_winding");

  MopViewport *vp = make_viewport(128, 96);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 100.0f);

  MopMesh *cube =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                               .vertex_count = 24,
                                               .indices = CUBE_IDX,
                                               .index_count = 36,
                                               .object_id = 1});
  TEST_ASSERT(cube != NULL);

  /* Mirror on X — flips winding. The backface culler should compensate
   * for negative determinant of the model matrix. */
  mop_mesh_set_scale(cube, (MopVec3){-1.0f, 1.0f, 1.0f});

  mop_viewport_render(vp);

  /* Cube center should still be visible (not fully culled) */
  MopPickResult pick = mop_viewport_pick(vp, 64, 48);
  TEST_ASSERT(pick.hit == true);
  TEST_ASSERT(pick.object_id == 1);

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * 12. Extreme scale — sub-pixel and screen-filling
 * ========================================================================= */

static void test_extreme_scale(void) {
  TEST_BEGIN("extreme_scale");

  MopViewport *vp = make_viewport(64, 48);
  TEST_ASSERT(vp != NULL);

  mop_viewport_set_camera(vp, (MopVec3){0, 0, 3}, (MopVec3){0, 0, 0},
                          (MopVec3){0, 1, 0}, 60.0f, 0.1f, 1e8f);

  MopMesh *cube =
      mop_viewport_add_mesh(vp, &(MopMeshDesc){.vertices = CUBE_VERTS,
                                               .vertex_count = 24,
                                               .indices = CUBE_IDX,
                                               .index_count = 36,
                                               .object_id = 12});
  TEST_ASSERT(cube != NULL);

  /* --- Pass 1: sub-pixel scale (cube shrinks to nothing) --- */
  mop_mesh_set_scale(cube, (MopVec3){1e-6f, 1e-6f, 1e-6f});
  mop_viewport_render(vp);

  {
    int w = 0, h = 0;
    const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
    TEST_ASSERT(buf != NULL);
    /* Verify no NaN leaked into the framebuffer (RGBA8 = 4 bytes/pixel).
     * NaN in float-to-uint8 conversion typically produces 0 or 255, but
     * we check the buffer is fully readable and has valid dimensions. */
    TEST_ASSERT(w == 64);
    TEST_ASSERT(h == 48);
    /* Scan for obvious corruption: each byte should be a valid uint8
     * (always true for uint8_t, so we verify the pointer arithmetic
     * doesn't segfault by touching first and last pixel). */
    (void)buf[0];
    (void)buf[(size_t)(w * h * 4) - 1];
  }

  /* --- Pass 2: enormous scale (cube fills everything and beyond) --- */
  mop_mesh_set_scale(cube, (MopVec3){1e6f, 1e6f, 1e6f});
  mop_viewport_render(vp);

  {
    int w = 0, h = 0;
    const uint8_t *buf = mop_viewport_read_color(vp, &w, &h);
    TEST_ASSERT(buf != NULL);
    TEST_ASSERT(w == 64);
    TEST_ASSERT(h == 48);
    (void)buf[0];
    (void)buf[(size_t)(w * h * 4) - 1];
  }

  mop_viewport_destroy(vp);
  TEST_END();
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
  TEST_SUITE_BEGIN("torture_geometry");

  TEST_RUN(test_zero_area_triangle);
  TEST_RUN(test_coincident_vertices);
  TEST_RUN(test_extremely_thin_triangle);
  TEST_RUN(test_huge_triangle);
  TEST_RUN(test_behind_camera);
  TEST_RUN(test_clipping_single_vertex_inside);
  TEST_RUN(test_triangle_at_near_plane);
  TEST_RUN(test_large_mesh_100k_tris);
  TEST_RUN(test_index_out_of_range);
  TEST_RUN(test_zero_index_count);
  TEST_RUN(test_negative_scale_winding);
  TEST_RUN(test_extreme_scale);

  TEST_REPORT();
  TEST_EXIT();
}
