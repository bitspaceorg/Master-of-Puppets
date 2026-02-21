/*
 * Master of Puppets — Example: Picking Tool (Interactive)
 *
 * Click anywhere to raycast.  Reports hit object, distance, normal,
 * and triangle index.  Also shows AABB and frustum query results.
 * Click=raycast  F=frustum report  A=AABB report  W=wireframe  Q/Esc=quit
 *
 * APIs: spatial.h, camera_query.h, query.h
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <mop/mop.h>
#include <stdio.h>

#include "geometry.h"
#include "sdl_harness.h"

/* -------------------------------------------------------------------------
 * Context
 * ------------------------------------------------------------------------- */

typedef struct {
  MopMesh *cubes[5];
} PickingCtx;

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static const char *frustum_str(int r) {
  switch (r) {
  case 1:
    return "INSIDE";
  case 0:
    return "INTERSECT";
  case -1:
    return "OUTSIDE";
  default:
    return "?";
  }
}

/* -------------------------------------------------------------------------
 * Scene layout — 5 cubes at known positions
 * ------------------------------------------------------------------------- */

static const MopVec3 CUBE_POSITIONS[] = {
    {0.0f, 0.0f, 0.0f},  /* id=1: origin       */
    {3.0f, 0.0f, 0.0f},  /* id=2: right         */
    {-3.0f, 0.0f, 0.0f}, /* id=3: left          */
    {0.0f, 2.0f, 0.0f},  /* id=4: above origin  */
    {0.0f, 0.0f, -6.0f}, /* id=5: far behind    */
};

static const MopMaterial CUBE_MATERIALS[] = {
    {.base_color = {0.9f, 0.2f, 0.2f, 1.0f},
     .metallic = 0.0f,
     .roughness = 0.5f},
    {.base_color = {0.2f, 0.8f, 0.3f, 1.0f},
     .metallic = 0.0f,
     .roughness = 0.5f},
    {.base_color = {0.2f, 0.3f, 0.9f, 1.0f},
     .metallic = 0.0f,
     .roughness = 0.5f},
    {.base_color = {0.9f, 0.8f, 0.1f, 1.0f},
     .metallic = 0.2f,
     .roughness = 0.4f},
    {.base_color = {0.8f, 0.2f, 0.8f, 1.0f},
     .metallic = 0.1f,
     .roughness = 0.6f},
};

#define NUM_CUBES 5

/* -------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------- */

static void pick_setup(MopViewport *vp, void *data) {
  PickingCtx *ctx = data;

  /* Camera */
  mop_viewport_set_camera(vp, (MopVec3){4.0f, 3.0f, 6.0f},
                          (MopVec3){0.0f, 0.0f, 0.0f},
                          (MopVec3){0.0f, 1.0f, 0.0f}, 60.0f, 0.1f, 100.0f);

  /* Lighting */
  mop_viewport_set_ambient(vp, 0.2f);

  MopLight dir = {.type = MOP_LIGHT_DIRECTIONAL,
                  .direction = {0.4f, 1.0f, 0.5f},
                  .color = {1.0f, 1.0f, 0.95f, 1.0f},
                  .intensity = 1.0f,
                  .active = true};
  mop_viewport_add_light(vp, &dir);

  /* 5 cubes at known positions */
  for (int i = 0; i < NUM_CUBES; i++) {
    MopMeshDesc md = {.vertices = CUBE_VERTICES,
                      .vertex_count = CUBE_VERTEX_COUNT,
                      .indices = CUBE_INDICES,
                      .index_count = CUBE_INDEX_COUNT,
                      .object_id = (uint32_t)(i + 1)};
    ctx->cubes[i] = mop_viewport_add_mesh(vp, &md);
    mop_mesh_set_position(ctx->cubes[i], CUBE_POSITIONS[i]);
    mop_mesh_set_material(ctx->cubes[i], &CUBE_MATERIALS[i]);
  }

  printf("Scene: 5 cubes (ids 1-5)\n");
  printf("  id=1 (0,0,0)  id=2 (3,0,0)  id=3 (-3,0,0)\n");
  printf("  id=4 (0,2,0)  id=5 (0,0,-6)\n");
  printf("Controls: Click=raycast  F=frustum  A=AABB  W=wireframe  "
         "Q/Esc=quit\n\n");
}

static void pick_on_click(MopViewport *vp, float x, float y, void *data) {
  PickingCtx *ctx = data;
  (void)ctx;

  printf("--- Click at (%.0f, %.0f) ---\n", x, y);

  /* Full raycast */
  MopRayHit hit = mop_viewport_raycast(vp, x, y);
  if (hit.hit) {
    printf("  Raycast: HIT  id=%u  dist=%.4f\n", hit.object_id, hit.distance);
    printf("    position = (%.3f, %.3f, %.3f)\n", hit.position.x,
           hit.position.y, hit.position.z);
    printf("    normal   = (%.3f, %.3f, %.3f)\n", hit.normal.x, hit.normal.y,
           hit.normal.z);
    printf("    triangle = %u  uv = (%.3f, %.3f)\n", hit.triangle_index, hit.u,
           hit.v);
  } else {
    printf("  Raycast: MISS\n");
  }

  /* Pixel-to-ray */
  MopRay ray = mop_viewport_pixel_to_ray(vp, x, y);
  printf("  Ray origin:    (%.4f, %.4f, %.4f)\n", ray.origin.x, ray.origin.y,
         ray.origin.z);
  printf("  Ray direction: (%.4f, %.4f, %.4f)\n", ray.direction.x,
         ray.direction.y, ray.direction.z);

  /* Ray-AABB test for each mesh */
  uint32_t mc = mop_viewport_mesh_count(vp);
  printf("  Ray-AABB tests:\n");
  for (uint32_t i = 0; i < mc; i++) {
    MopMesh *m = mop_viewport_mesh_at(vp, i);
    if (!m)
      continue;
    uint32_t oid = mop_mesh_get_object_id(m);
    MopAABB aabb = mop_mesh_get_aabb_world(m, vp);
    float t_near = 0.0f, t_far = 0.0f;
    bool intersects = mop_ray_intersect_aabb(ray, aabb, &t_near, &t_far);
    if (intersects)
      printf("    id=%u  HIT  t_near=%.4f  t_far=%.4f\n", oid, t_near, t_far);
    else
      printf("    id=%u  MISS\n", oid);
  }
  printf("\n");
}

static bool pick_on_key(MopViewport *vp, SDL_Keycode key, void *data) {
  PickingCtx *ctx = data;
  (void)ctx;

  switch (key) {

  case SDLK_F: {
    /* Frustum report */
    printf("--- Frustum Report ---\n");
    MopFrustum frustum = mop_viewport_get_frustum(vp);

    printf("  Planes:\n");
    for (int i = 0; i < 6; i++) {
      MopVec4 p = frustum.planes[i];
      printf("    [%d] (%.4f, %.4f, %.4f, %.4f)\n", i, p.x, p.y, p.z, p.w);
    }

    uint32_t mc = mop_viewport_mesh_count(vp);
    printf("  Per-mesh frustum test:\n");
    for (uint32_t i = 0; i < mc; i++) {
      MopMesh *m = mop_viewport_mesh_at(vp, i);
      if (!m)
        continue;
      uint32_t oid = mop_mesh_get_object_id(m);
      MopAABB world = mop_mesh_get_aabb_world(m, vp);
      int result = mop_frustum_test_aabb(&frustum, world);
      printf("    id=%u  %s\n", oid, frustum_str(result));
    }

    uint32_t vis = mop_viewport_visible_mesh_count(vp);
    printf("  Visible: %u / %u\n", vis, mc);

    /* Scene AABB */
    MopAABB scene = mop_viewport_get_scene_aabb(vp);
    MopVec3 center = mop_aabb_center(scene);
    MopVec3 extents = mop_aabb_extents(scene);
    float area = mop_aabb_surface_area(scene);
    printf("  Scene AABB:\n");
    printf("    min = (%.3f, %.3f, %.3f)\n", scene.min.x, scene.min.y,
           scene.min.z);
    printf("    max = (%.3f, %.3f, %.3f)\n", scene.max.x, scene.max.y,
           scene.max.z);
    printf("    center  = (%.3f, %.3f, %.3f)\n", center.x, center.y, center.z);
    printf("    extents = (%.3f, %.3f, %.3f)\n", extents.x, extents.y,
           extents.z);
    printf("    surface_area = %.3f\n\n", area);
    return true;
  }

  case SDLK_A: {
    /* Per-mesh AABB report */
    uint32_t mc = mop_viewport_mesh_count(vp);
    printf("--- AABB Report (%u meshes) ---\n", mc);
    for (uint32_t i = 0; i < mc; i++) {
      MopMesh *m = mop_viewport_mesh_at(vp, i);
      if (!m)
        continue;
      uint32_t oid = mop_mesh_get_object_id(m);
      MopAABB local = mop_mesh_get_aabb_local(m, vp);
      MopAABB world = mop_mesh_get_aabb_world(m, vp);
      printf("  id=%u\n", oid);
      printf("    local  min=(%.3f,%.3f,%.3f)  max=(%.3f,%.3f,%.3f)\n",
             local.min.x, local.min.y, local.min.z, local.max.x, local.max.y,
             local.max.z);
      printf("    world  min=(%.3f,%.3f,%.3f)  max=(%.3f,%.3f,%.3f)\n",
             world.min.x, world.min.y, world.min.z, world.max.x, world.max.y,
             world.max.z);
      printf("    center=(%.3f,%.3f,%.3f)  area=%.3f\n",
             mop_aabb_center(world).x, mop_aabb_center(world).y,
             mop_aabb_center(world).z, mop_aabb_surface_area(world));
    }
    printf("\n");
    return true;
  }

  default:
    return false;
  }
}

static void pick_cleanup(void *data) {
  (void)data;
  printf("Picking tool shutdown.\n");
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  PickingCtx ctx = {0};

  MopSDLApp app = {.title = "MOP -- Picking Tool",
                   .width = 800,
                   .height = 600,
                   .setup = pick_setup,
                   .on_click = pick_on_click,
                   .on_key = pick_on_key,
                   .cleanup = pick_cleanup,
                   .ctx = &ctx};

  return mop_sdl_run(&app);
}
