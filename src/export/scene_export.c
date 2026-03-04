/*
 * Master of Puppets — Scene JSON Export
 * scene_export.c — Export viewport state to JSON scene definition
 *
 * Manual JSON serialization (snprintf) — no library dependency.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/core/light.h>
#include <mop/core/material.h>
#include <mop/core/scene.h>
#include <mop/core/viewport.h>
#include <mop/export/scene_export.h>
#include <mop/query/camera_query.h>
#include <mop/query/scene_query.h>
#include <mop/util/log.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * JSON writer helpers
 * ------------------------------------------------------------------------- */

static void json_vec3(FILE *fp, const char *key, float x, float y, float z) {
  fprintf(fp, "    \"%s\": [%.6g, %.6g, %.6g]", key, x, y, z);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int mop_export_scene_json(const MopViewport *vp, const char *path) {
  if (!vp || !path) {
    MOP_ERROR("mop_export_scene_json: NULL argument");
    return -1;
  }

  FILE *fp = fopen(path, "w");
  if (!fp) {
    MOP_ERROR("mop_export_scene_json: failed to open '%s'", path);
    return -1;
  }

  /* Camera state */
  MopCameraState cam = mop_viewport_get_camera_state(vp);
  float fov_degrees = cam.fov_radians * (180.0f / 3.14159265f);

  fprintf(fp, "{\n");
  fprintf(fp, "  \"name\": \"exported_scene\",\n");

  int w = 0, h = 0;
  mop_viewport_read_color((MopViewport *)vp, &w, &h);
  fprintf(fp, "  \"width\": %d, \"height\": %d,\n", w > 0 ? w : 800,
          h > 0 ? h : 600);

  /* Camera */
  fprintf(fp, "  \"camera\": {\n");
  json_vec3(fp, "eye", cam.eye.x, cam.eye.y, cam.eye.z);
  fprintf(fp, ",\n");
  json_vec3(fp, "target", cam.target.x, cam.target.y, cam.target.z);
  fprintf(fp, ",\n");
  json_vec3(fp, "up", cam.up.x, cam.up.y, cam.up.z);
  fprintf(fp, ",\n");
  fprintf(fp, "    \"fov\": %.6g,\n", fov_degrees);
  fprintf(fp, "    \"near\": %.6g,\n", cam.near_plane);
  fprintf(fp, "    \"far\": %.6g\n", cam.far_plane);
  fprintf(fp, "  },\n");

  /* Lights */
  uint32_t nlight = mop_viewport_light_count(vp);
  fprintf(fp, "  \"lights\": [\n");
  uint32_t lights_written = 0;
  for (uint32_t i = 0; i < nlight; i++) {
    const MopLight *l = mop_viewport_light_at(vp, i);
    if (!l)
      continue;
    if (lights_written > 0)
      fprintf(fp, ",\n");
    fprintf(fp, "    {\n");

    const char *type_str = "directional";
    if (l->type == MOP_LIGHT_POINT)
      type_str = "point";
    else if (l->type == MOP_LIGHT_SPOT)
      type_str = "spot";
    fprintf(fp, "      \"type\": \"%s\"", type_str);

    if (l->type == MOP_LIGHT_DIRECTIONAL || l->type == MOP_LIGHT_SPOT) {
      fprintf(fp, ",\n      \"dir\": [%.6g, %.6g, %.6g]", l->direction.x,
              l->direction.y, l->direction.z);
    }
    if (l->type == MOP_LIGHT_POINT || l->type == MOP_LIGHT_SPOT) {
      fprintf(fp, ",\n      \"position\": [%.6g, %.6g, %.6g]", l->position.x,
              l->position.y, l->position.z);
    }
    fprintf(fp, ",\n      \"color\": [%.6g, %.6g, %.6g]", l->color.r,
            l->color.g, l->color.b);
    fprintf(fp, ",\n      \"intensity\": %.6g", l->intensity);

    if (l->type == MOP_LIGHT_POINT || l->type == MOP_LIGHT_SPOT) {
      fprintf(fp, ",\n      \"range\": %.6g", l->range);
    }
    if (l->type == MOP_LIGHT_SPOT) {
      fprintf(fp, ",\n      \"spot_inner_cos\": %.6g", l->spot_inner_cos);
      fprintf(fp, ",\n      \"spot_outer_cos\": %.6g", l->spot_outer_cos);
    }

    fprintf(fp, "\n    }");
    lights_written++;
  }
  fprintf(fp, "\n  ],\n");

  /* Objects (meshes) */
  uint32_t nmesh = mop_viewport_mesh_count(vp);
  fprintf(fp, "  \"objects\": [\n");
  for (uint32_t m = 0; m < nmesh; m++) {
    MopMesh *mesh = mop_viewport_mesh_at(vp, m);
    if (!mesh)
      continue;

    if (m > 0)
      fprintf(fp, ",\n");
    fprintf(fp, "    {\n");
    fprintf(fp, "      \"type\": \"mesh\",\n");
    fprintf(fp, "      \"object_id\": %u,\n", mop_mesh_get_object_id(mesh));

    /* Transform */
    MopVec3 pos = mop_mesh_get_position(mesh);
    MopVec3 rot = mop_mesh_get_rotation(mesh);
    MopVec3 scl = mop_mesh_get_scale(mesh);
    fprintf(fp, "      \"transform\": {\n");
    fprintf(fp, "        \"position\": [%.6g, %.6g, %.6g],\n", pos.x, pos.y,
            pos.z);
    fprintf(fp, "        \"rotation\": [%.6g, %.6g, %.6g],\n", rot.x, rot.y,
            rot.z);
    fprintf(fp, "        \"scale\": [%.6g, %.6g, %.6g]\n", scl.x, scl.y, scl.z);
    fprintf(fp, "      },\n");

    /* Material / color */
    MopMaterial mat = mop_mesh_get_material(mesh);
    fprintf(fp, "      \"color\": [%.6g, %.6g, %.6g, %.6g],\n",
            mat.base_color.r, mat.base_color.g, mat.base_color.b,
            mat.base_color.a);
    fprintf(fp, "      \"metallic\": %.6g,\n", mat.metallic);
    fprintf(fp, "      \"roughness\": %.6g\n", mat.roughness);

    fprintf(fp, "    }");
  }
  fprintf(fp, "\n  ],\n");

  /* Render settings */
  fprintf(fp, "  \"render_mode\": \"shaded\",\n");
  fprintf(fp, "  \"shading_mode\": \"smooth\"\n");

  fprintf(fp, "}\n");

  fclose(fp);
  MOP_INFO("mop_export_scene_json: wrote '%s'", path);
  return 0;
}
