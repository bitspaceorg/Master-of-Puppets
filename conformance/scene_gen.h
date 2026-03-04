/*
 * Master of Puppets — Conformance Framework
 * scene_gen.h — Procedural mesh generation and stress-scene builder
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CONF_SCENE_GEN_H
#define MOP_CONF_SCENE_GEN_H

#include <mop/types.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Mesh generation — caller owns returned arrays, free with free()
 *
 * All generators return a vertex array and optionally fill index data.
 * Winding order is counter-clockwise (CCW) throughout.
 * ------------------------------------------------------------------------- */

/* UV sphere with `rings` latitude bands and `sectors` longitude segments.
 * Generates proper outward-facing normals and white vertex color. */
MopVertex *mop_gen_sphere(uint32_t rings, uint32_t sectors,
                          uint32_t *out_vertex_count, uint32_t **out_indices,
                          uint32_t *out_index_count);

/* Cylinder: `segments` around the circumference, `stacks` subdivisions along
 * the height axis.  Unit height (y in [0,1]), unit radius.  Includes top and
 * bottom caps. */
MopVertex *mop_gen_cylinder(uint32_t segments, uint32_t stacks,
                            uint32_t *out_vertex_count, uint32_t **out_indices,
                            uint32_t *out_index_count);

/* Single quad (2 triangles), centered at origin in the XY plane, facing +Z.
 * Width `w` along X, height `h` along Y. */
MopVertex *mop_gen_quad(float w, float h, uint32_t *out_vertex_count,
                        uint32_t **out_indices, uint32_t *out_index_count);

/* Axis-aligned cube with side length `size`, centered at origin.
 * 24 vertices (separate normals per face), 36 indices. */
MopVertex *mop_gen_cube(float size, uint32_t *out_vertex_count,
                        uint32_t **out_indices, uint32_t *out_index_count);

/* Segmented chain mesh for animation stress testing.
 * bone_count segments of length segment_len each, connected end-to-end
 * along the Y axis.  Each segment is a small cylinder (8 sides, 1 stack).
 * Returns vertex array; caller frees. */
MopVertex *mop_gen_bone_chain(uint32_t bone_count, float segment_len,
                              uint32_t *out_vertex_count,
                              uint32_t **out_indices,
                              uint32_t *out_index_count);

/* -------------------------------------------------------------------------
 * Procedural textures — returns RGBA8 data, caller frees with free()
 * ------------------------------------------------------------------------- */

/* Alternating black/white checkerboard with `tiles` tiles per axis. */
uint8_t *mop_gen_checker_texture(int width, int height, int tiles);

/* Tangent-space brick normal map.  Mortar grooves appear as recessed
 * channels; brick faces point straight out (0, 0, 1). */
uint8_t *mop_gen_brick_normal_map(int width, int height);

/* -------------------------------------------------------------------------
 * Conformance stress scene (zones A-G from the spec)
 *
 * The scene builder pre-generates all shared mesh data, instance transforms,
 * hierarchy chains, and procedural textures needed by the conformance runner.
 * Individual zone rendering is orchestrated by the runner itself.
 * ------------------------------------------------------------------------- */

typedef struct MopConfScene {
  /* Zone A: instancing grid — 100x100 sphere instances */
  MopVertex *sphere_verts;
  uint32_t sphere_vert_count;
  uint32_t *sphere_indices;
  uint32_t sphere_index_count;
  MopMat4 *sphere_transforms;
  uint32_t sphere_instance_count;

  /* Zone B: hierarchy tower — 24-level parent-child chain */
  MopVertex *cylinder_verts;
  uint32_t cylinder_vert_count;
  uint32_t *cylinder_indices;
  uint32_t cylinder_index_count;
  MopMat4 tower_local_transforms[24];
  MopMat4 tower_world_transforms[24];

  /* Zone C: precision stress (basic meshes; specific variants at render time)
   */
  MopVertex *quad_verts;
  uint32_t quad_vert_count;
  uint32_t *quad_indices;
  uint32_t quad_index_count;
  MopVertex *cube_verts;
  uint32_t cube_vert_count;
  uint32_t *cube_indices;
  uint32_t cube_index_count;

  /* Zone C: precision stress variants */
  MopVertex
      *coplanar_quad_verts; /* 8 quads stacked at same z with tiny offsets */
  uint32_t coplanar_quad_vert_count;
  uint32_t *coplanar_quad_indices;
  uint32_t coplanar_quad_index_count;
  uint32_t coplanar_quad_count;   /* number of quads (8) */
  MopMat4 coplanar_transforms[8]; /* transforms for each coplanar quad */

  MopVertex *degenerate_verts; /* 64 degenerate triangles */
  uint32_t degenerate_vert_count;
  uint32_t *degenerate_indices;
  uint32_t degenerate_index_count;

  MopMat4 huge_plane_transform;     /* S(1e6, 1, 1e6) at z=0 */
  MopMat4 micro_cube_transform;     /* S(1e-6) near origin */
  MopMat4 macro_cube_transform;     /* S(1e3) at position (1e4, 0, 0) */
  MopMat4 neg_scale_cube_transform; /* S(-1, 1, 1) */

  /* Zone D: transparency stress */
  MopMat4 alpha_plane_transforms[16]; /* 16 intersecting alpha planes */
  float alpha_plane_opacities[16];    /* opacity 0.1..0.9 */
  MopMat4 alpha_clip_transforms[16];  /* 4x4 billboard grid */
  MopMat4 double_sided_transforms[8]; /* 8 double-sided alpha planes */

  /* Zone F: material stress grid -- 6x4 spheres with varying materials */
  MopMat4 material_sphere_transforms[24]; /* 6 columns x 4 rows */
  float material_metallic[24];
  float material_roughness[24];

  /* Zone G: animation stress -- bone chain */
  MopVertex *bone_chain_verts;
  uint32_t bone_chain_vert_count;
  uint32_t *bone_chain_indices;
  uint32_t bone_chain_index_count;
  uint32_t bone_chain_bone_count;
  float bone_chain_segment_len;

  /* Procedural textures */
  uint8_t *checker_tex;
  int checker_tex_size;
  uint8_t *brick_nm_tex;
  int brick_nm_tex_size;
} MopConfScene;

/* Allocate and populate the full conformance scene.  Returns NULL on OOM. */
MopConfScene *mop_conf_scene_create(void);

/* Release all memory owned by the scene. */
void mop_conf_scene_destroy(MopConfScene *scene);

#endif /* MOP_CONF_SCENE_GEN_H */
