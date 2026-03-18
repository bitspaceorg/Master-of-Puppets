/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * gltf.h — glTF 2.0 loader (Phase 8C)
 *
 * Loads .glb (binary glTF) and .gltf (JSON + external .bin) files.
 * Extracts meshes, materials, textures, skinning data, and scene hierarchy.
 *
 * Usage:
 *   MopGltfScene scene;
 *   if (mop_gltf_load("model.glb", &scene)) {
 *     for (uint32_t i = 0; i < scene.mesh_count; i++) {
 *       // scene.meshes[i] has vertices, indices, material index, etc.
 *     }
 *     mop_gltf_free(&scene);
 *   }
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_LOADER_GLTF_H
#define MOP_LOADER_GLTF_H

#include <mop/types.h>
#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * glTF material (metallic-roughness PBR)
 * ------------------------------------------------------------------------- */

typedef struct MopGltfTexRef {
  int32_t image_index; /* index into scene.images[], or -1 if none */
  int32_t tex_coord;   /* UV set index (0 or 1) */
} MopGltfTexRef;

typedef struct MopGltfMaterial {
  char name[64];
  float base_color[4]; /* RGBA linear */
  float metallic;
  float roughness;
  float emissive[3];
  MopGltfTexRef base_color_tex; /* albedo map */
  MopGltfTexRef normal_tex;     /* normal map */
  MopGltfTexRef mr_tex;         /* metallic-roughness: G=rough, B=metal */
  MopGltfTexRef occlusion_tex;  /* ambient occlusion (R channel) */
  MopGltfTexRef emissive_tex;   /* emissive map */
  bool double_sided;
  bool unlit; /* KHR_materials_unlit */
  float alpha_cutoff;
  enum {
    MOP_GLTF_ALPHA_OPAQUE,
    MOP_GLTF_ALPHA_MASK,
    MOP_GLTF_ALPHA_BLEND
  } alpha_mode;
} MopGltfMaterial;

/* -------------------------------------------------------------------------
 * glTF image (embedded or external)
 * ------------------------------------------------------------------------- */

typedef struct MopGltfImage {
  char name[64];
  char uri[256];       /* file path (for .gltf with external files) */
  char mime_type[32];  /* "image/png", "image/jpeg", etc. */
  const uint8_t *data; /* raw image data (points into buffer, do not free) */
  uint32_t data_size;  /* size in bytes */
} MopGltfImage;

/* -------------------------------------------------------------------------
 * glTF mesh primitive (one draw call)
 * ------------------------------------------------------------------------- */

typedef struct MopGltfPrimitive {
  MopVertex *vertices;
  uint32_t vertex_count;
  uint32_t *indices;
  uint32_t index_count;
  int32_t material_index; /* index into scene.materials[], or -1 */

  /* Tangents (float4: xyz + handedness in w) — NULL if not available */
  float *tangents; /* 4 floats per vertex */

  /* Skinning data — NULL if not skinned */
  uint8_t *joints; /* 4 uint8 per vertex (bone indices) */
  float *weights;  /* 4 floats per vertex (bone weights) */

  /* Bounding box */
  MopVec3 bbox_min;
  MopVec3 bbox_max;
} MopGltfPrimitive;

/* -------------------------------------------------------------------------
 * glTF mesh (collection of primitives)
 * ------------------------------------------------------------------------- */

typedef struct MopGltfMesh {
  char name[64];
  MopGltfPrimitive *primitives;
  uint32_t primitive_count;
} MopGltfMesh;

/* -------------------------------------------------------------------------
 * glTF node (scene hierarchy)
 * ------------------------------------------------------------------------- */

typedef struct MopGltfNode {
  char name[64];
  int32_t mesh_index;   /* index into scene.meshes[], or -1 */
  int32_t skin_index;   /* index into scene.skins[], or -1 */
  int32_t parent_index; /* index into scene.nodes[], or -1 (root) */
  float translation[3];
  float rotation[4]; /* quaternion (x,y,z,w) */
  float scale[3];
  float matrix[16];  /* column-major 4x4, or identity if TRS used */
  bool has_matrix;   /* true = use matrix, false = use TRS */
  int32_t *children; /* indices into scene.nodes[] */
  uint32_t child_count;
} MopGltfNode;

/* -------------------------------------------------------------------------
 * glTF skin (skeletal animation rig)
 * ------------------------------------------------------------------------- */

typedef struct MopGltfSkin {
  char name[64];
  int32_t *joints; /* node indices for each bone */
  uint32_t joint_count;
  float *inverse_bind_matrices; /* 16 floats per joint (column-major) */
  int32_t skeleton_root;        /* root node index, or -1 */
} MopGltfSkin;

/* -------------------------------------------------------------------------
 * glTF scene — top-level container
 * ------------------------------------------------------------------------- */

typedef struct MopGltfScene {
  /* Meshes (each may have multiple primitives) */
  MopGltfMesh *meshes;
  uint32_t mesh_count;

  /* Materials */
  MopGltfMaterial *materials;
  uint32_t material_count;

  /* Images (raw data — decode with stb_image) */
  MopGltfImage *images;
  uint32_t image_count;

  /* Scene hierarchy */
  MopGltfNode *nodes;
  uint32_t node_count;

  /* Skeletal rigs */
  MopGltfSkin *skins;
  uint32_t skin_count;

  /* Internal buffer storage (do not access directly) */
  uint8_t *_buffer_data;
  uint32_t _buffer_size;
  char *_json_data;
} MopGltfScene;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Load a glTF 2.0 file (.glb binary or .gltf JSON + .bin).
 * Returns true on success; on failure, out is zeroed. */
bool mop_gltf_load(const char *path, MopGltfScene *out);

/* Free all memory allocated by mop_gltf_load. */
void mop_gltf_free(MopGltfScene *scene);

/* Helper: import a glTF scene into a viewport.
 * Creates meshes, textures, and materials.  Populates bone hierarchy
 * for skinned meshes.  Returns the number of meshes created.
 * base_object_id: starting object ID for picking (auto-increments). */
uint32_t mop_gltf_import(const MopGltfScene *scene, MopViewport *viewport,
                         uint32_t base_object_id);

#endif /* MOP_LOADER_GLTF_H */
