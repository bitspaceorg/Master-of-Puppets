/*
 * Master of Puppets — Wavefront OBJ Export
 * obj_export.c — Export meshes and scenes to Wavefront OBJ format
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/export/obj_export.h>
#include <mop/query/camera_query.h>
#include <mop/query/scene_query.h>
#include <mop/util/log.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Helper: write a single mesh to an open FILE
 * ------------------------------------------------------------------------- */

static int write_mesh_obj(FILE *fp, const MopVertex *verts,
                          const uint32_t *indices, uint32_t vertex_count,
                          uint32_t index_count, const MopMat4 *world,
                          const char *group_name, uint32_t index_base) {
  if (group_name) {
    fprintf(fp, "o %s\n", group_name);
  }

  /* Write vertices with world transform baked in */
  for (uint32_t i = 0; i < vertex_count; i++) {
    float x = verts[i].position.x;
    float y = verts[i].position.y;
    float z = verts[i].position.z;

    if (world) {
      MopVec4 p = mop_mat4_mul_vec4(*world, (MopVec4){x, y, z, 1.0f});
      x = p.x;
      y = p.y;
      z = p.z;
    }
    fprintf(fp, "v %f %f %f\n", x, y, z);
  }

  /* Write normals (transformed by inverse-transpose for correctness,
   * but for uniform scaling the normal matrix == model matrix rotation) */
  for (uint32_t i = 0; i < vertex_count; i++) {
    float nx = verts[i].normal.x;
    float ny = verts[i].normal.y;
    float nz = verts[i].normal.z;

    if (world) {
      /* Transform normal by upper-3x3 of world matrix.
       * For non-uniform scale, this is approximate but sufficient
       * for OBJ export. */
      float tnx = world->d[0] * nx + world->d[4] * ny + world->d[8] * nz;
      float tny = world->d[1] * nx + world->d[5] * ny + world->d[9] * nz;
      float tnz = world->d[2] * nx + world->d[6] * ny + world->d[10] * nz;
      float len = sqrtf(tnx * tnx + tny * tny + tnz * tnz);
      if (len > 1e-8f) {
        tnx /= len;
        tny /= len;
        tnz /= len;
      }
      nx = tnx;
      ny = tny;
      nz = tnz;
    }
    fprintf(fp, "vn %f %f %f\n", nx, ny, nz);
  }

  /* Write texture coordinates */
  for (uint32_t i = 0; i < vertex_count; i++) {
    fprintf(fp, "vt %f %f\n", verts[i].u, verts[i].v);
  }

  /* Write faces (1-based OBJ indices) */
  for (uint32_t i = 0; i + 2 < index_count; i += 3) {
    uint32_t a = indices[i + 0] + index_base + 1;
    uint32_t b = indices[i + 1] + index_base + 1;
    uint32_t c = indices[i + 2] + index_base + 1;
    fprintf(fp, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
  }

  return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int mop_export_obj_mesh(const MopMesh *mesh, const MopViewport *vp,
                        const char *path) {
  if (!mesh || !vp || !path) {
    MOP_ERROR("mop_export_obj_mesh: NULL argument");
    return -1;
  }

  const MopVertex *verts = mop_mesh_get_vertices(mesh, vp);
  const uint32_t *indices = mop_mesh_get_indices(mesh, vp);
  uint32_t vcount = mop_mesh_get_vertex_count(mesh);
  uint32_t icount = mop_mesh_get_index_count(mesh);

  if (!verts || !indices || vcount == 0) {
    MOP_ERROR("mop_export_obj_mesh: no vertex data");
    return -1;
  }

  FILE *fp = fopen(path, "w");
  if (!fp) {
    MOP_ERROR("mop_export_obj_mesh: failed to open '%s'", path);
    return -1;
  }

  fprintf(fp, "# Exported by Master of Puppets\n");
  MopMat4 world = mop_mesh_get_world_transform(mesh);
  write_mesh_obj(fp, verts, indices, vcount, icount, &world, "mesh", 0);

  fclose(fp);
  MOP_INFO("mop_export_obj_mesh: wrote '%s' (%u verts, %u tris)", path, vcount,
           icount / 3);
  return 0;
}

int mop_export_obj_scene(const MopViewport *vp, const char *path) {
  if (!vp || !path) {
    MOP_ERROR("mop_export_obj_scene: NULL argument");
    return -1;
  }

  FILE *fp = fopen(path, "w");
  if (!fp) {
    MOP_ERROR("mop_export_obj_scene: failed to open '%s'", path);
    return -1;
  }

  fprintf(fp, "# Exported by Master of Puppets\n");

  uint32_t nmesh = mop_viewport_mesh_count(vp);
  uint32_t index_base = 0;

  for (uint32_t m = 0; m < nmesh; m++) {
    MopMesh *mesh = mop_viewport_mesh_at(vp, m);
    if (!mesh)
      continue;

    const MopVertex *verts = mop_mesh_get_vertices(mesh, vp);
    const uint32_t *indices = mop_mesh_get_indices(mesh, vp);
    uint32_t vcount = mop_mesh_get_vertex_count(mesh);
    uint32_t icount = mop_mesh_get_index_count(mesh);

    if (!verts || !indices || vcount == 0)
      continue;

    char group_name[64];
    snprintf(group_name, sizeof(group_name), "mesh_%u",
             mop_mesh_get_object_id(mesh));

    MopMat4 world = mop_mesh_get_world_transform(mesh);
    write_mesh_obj(fp, verts, indices, vcount, icount, &world, group_name,
                   index_base);
    index_base += vcount;
  }

  fclose(fp);
  MOP_INFO("mop_export_obj_scene: wrote '%s' (%u meshes)", path, nmesh);
  return 0;
}
