/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * scene.h — Mesh management within a viewport
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_SCENE_H
#define MOP_CORE_SCENE_H

#include <mop/core/display.h>
#include <mop/core/vertex_format.h>
#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — defined in viewport.h */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Opaque mesh handle
 * ------------------------------------------------------------------------- */

typedef struct MopMesh MopMesh;

/* -------------------------------------------------------------------------
 * Mesh descriptor
 *
 * vertices     : array of MopVertex, owned by application
 * vertex_count : number of vertices
 * indices      : array of uint32_t triangle indices, owned by application
 * index_count  : number of indices (must be a multiple of 3)
 * object_id    : unique identifier for picking (0 = no object)
 *
 * The engine copies vertex and index data during mop_viewport_add_mesh.
 * The application may free its arrays after the call returns.
 * ------------------------------------------------------------------------- */

typedef struct MopMeshDesc {
  const MopVertex *vertices;
  uint32_t vertex_count;
  const uint32_t *indices;
  uint32_t index_count;
  uint32_t object_id;

  /* Optional flexible vertex format. When NULL (default), `vertices` is
   * interpreted as a standard MopVertex array. When non-NULL, `vertices`
   * is reinterpreted as raw bytes with the given layout — equivalent to
   * the old `MopMeshDescEx` path. Lets hosts pass custom attribute sets
   * through the primary API without switching descriptors. */
  const MopVertexFormat *vertex_format;
} MopMeshDesc;

/* -------------------------------------------------------------------------
 * Mesh management
 *
 * All mesh functions require a valid, non-NULL viewport.
 * Meshes are owned by their viewport and destroyed when the viewport is
 * destroyed.  Explicit removal is optional.
 * ------------------------------------------------------------------------- */

/* Add a mesh to the viewport.  Returns NULL on failure.
 *
 * Pointer stability: the returned MopMesh* is stable for the lifetime of the
 * mesh. The viewport stores meshes as an array of MopMesh* (each struct is
 * separately heap-allocated), so growing the pool reallocates the pointer
 * array, not the individual mesh structs. You may cache the returned pointer
 * across subsequent add/remove calls, including in ECS components. The
 * pointer is only invalidated by mop_viewport_remove_mesh on this mesh, or
 * when the owning viewport is destroyed. */
MopMesh *mop_viewport_add_mesh(MopViewport *viewport, const MopMeshDesc *desc);

/* -------------------------------------------------------------------------
 * Extended mesh descriptor — flexible vertex format
 *
 * Like MopMeshDesc but takes raw vertex bytes + a vertex format descriptor
 * instead of a MopVertex array.  The engine copies vertex and index data
 * during the call; the application may free its arrays afterward.
 * ------------------------------------------------------------------------- */

typedef struct MopMeshDescEx {
  const void *vertex_data; /* raw interleaved bytes */
  uint32_t vertex_count;
  const uint32_t *indices;
  uint32_t index_count;
  uint32_t object_id;
  const MopVertexFormat *vertex_format; /* required */
} MopMeshDescEx;

/* Add a mesh with a flexible vertex format.  Returns NULL on failure. */
MopMesh *mop_viewport_add_mesh_ex(MopViewport *viewport,
                                  const MopMeshDescEx *desc);

/* Remove a mesh from the viewport and free its resources. */
void mop_viewport_remove_mesh(MopViewport *viewport, MopMesh *mesh);

/* Update a mesh's vertex and index data in-place.
 * If the new data fits within the existing buffers, this is a fast memcpy
 * with no allocation.  If the buffers need to grow, they are reallocated
 * (2x growth strategy).  Vertex and index counts may differ from the
 * original.  The application may free its arrays after the call returns. */
void mop_mesh_update_geometry(MopMesh *mesh, MopViewport *viewport,
                              const MopVertex *vertices, uint32_t vertex_count,
                              const uint32_t *indices, uint32_t index_count);

/* Set the model transform for a mesh.  Identity by default. */
void mop_mesh_set_transform(MopMesh *mesh, const MopMat4 *transform);

/* Set the opacity for a mesh.  1.0 = fully opaque (default), 0.0 = invisible.
 *
 * Auto-promotion: setting opacity < 1.0 on a mesh whose blend mode is still
 * MOP_BLEND_OPAQUE (the default) automatically switches it to MOP_BLEND_ALPHA
 * so the alpha channel is honoured by the renderer. The opaque pass writes
 * RGB only; without this promotion, set_opacity would silently no-op.
 *
 * If you've explicitly set a blend mode (additive, multiply), it is left
 * alone — opacity then acts as the colour multiplier appropriate to that
 * mode. Use mop_mesh_set_blend_mode(mesh, MOP_BLEND_OPAQUE) after this call
 * if you want to force the opaque path. */
void mop_mesh_set_opacity(MopMesh *mesh, float opacity);

/* Set the blend mode for a mesh.  MOP_BLEND_OPAQUE by default. */
void mop_mesh_set_blend_mode(MopMesh *mesh, MopBlendMode mode);

/* -------------------------------------------------------------------------
 * Texture management
 *
 * Textures are owned by the viewport's RHI device.  Destroy them before
 * destroying the viewport.
 * ------------------------------------------------------------------------- */

typedef struct MopTexture MopTexture;

MopTexture *mop_viewport_create_texture(MopViewport *viewport, int width,
                                        int height, const uint8_t *rgba_data);
void mop_viewport_destroy_texture(MopViewport *viewport, MopTexture *texture);
void mop_mesh_set_texture(MopMesh *mesh, MopTexture *texture);

/* -------------------------------------------------------------------------
 * Per-mesh TRS (position / rotation / scale)
 *
 * MOP auto-computes the model matrix from TRS each frame.  Using these
 * functions is the recommended path; mop_mesh_set_transform is available
 * as an override for callers who compute their own matrix.
 *
 * Rotation components are euler angles in radians (Rz * Ry * Rx order).
 * Default: position = (0,0,0), rotation = (0,0,0), scale = (1,1,1).
 * ------------------------------------------------------------------------- */

void mop_mesh_set_position(MopMesh *mesh, MopVec3 position);
void mop_mesh_set_rotation(MopMesh *mesh, MopVec3 rotation);
void mop_mesh_set_scale(MopMesh *mesh, MopVec3 scale);
MopVec3 mop_mesh_get_position(const MopMesh *mesh);
MopVec3 mop_mesh_get_rotation(const MopMesh *mesh);
MopVec3 mop_mesh_get_scale(const MopMesh *mesh);

/* Override the viewport-level shading mode for this mesh.
 * Pass -1 to clear the override and use the viewport default. */
void mop_mesh_set_shading(MopMesh *mesh, MopShadingMode mode);

/* -------------------------------------------------------------------------
 * Hierarchical transforms (Scene Graph)
 *
 * A mesh may have a single parent.  When rendering, MOP computes the
 * world_transform as the product of all ancestor transforms down to the
 * mesh's own local transform.  This is done in two passes: roots first,
 * then children.
 *
 * parent_index = -1 means no parent (root).
 * ------------------------------------------------------------------------- */

/* Set a mesh's parent.  The parent must belong to the same viewport. */
void mop_mesh_set_parent(MopMesh *mesh, MopMesh *parent, MopViewport *viewport);

/* Remove a mesh's parent (make it a root node). */
void mop_mesh_clear_parent(MopMesh *mesh);

/* -------------------------------------------------------------------------
 * Skeletal skinning
 *
 * Set bone matrices for a skinned mesh.  The mesh must have been created
 * with mop_viewport_add_mesh_ex() using a vertex format that includes
 * MOP_ATTRIB_JOINTS (ubyte4) and MOP_ATTRIB_WEIGHTS (float4).
 *
 * On the first call, MOP copies the current vertex data as the "bind pose".
 * Subsequent calls update bone matrices and mark the mesh for re-skinning.
 * Skinning is applied automatically before the next render.
 *
 * matrices: array of bone_count transform matrices (bind-to-current).
 * Ownership: MOP copies the data; the caller may free after return.
 * ------------------------------------------------------------------------- */

void mop_mesh_set_bone_matrices(MopMesh *mesh, MopViewport *viewport,
                                const MopMat4 *matrices, uint32_t bone_count);

/* -------------------------------------------------------------------------
 * Bone hierarchy (for skeleton visualization)
 *
 * parent_indices: array of bone_count signed integers.  parent_indices[i]
 * is the parent bone index for bone i, or -1 if bone i is a root.
 * Ownership: MOP copies the data; the caller may free after return.
 * Must be called after mop_mesh_set_bone_matrices().
 * ------------------------------------------------------------------------- */

void mop_mesh_set_bone_hierarchy(MopMesh *mesh, const int32_t *parent_indices,
                                 uint32_t bone_count);

/* -------------------------------------------------------------------------
 * Morph targets / blend shapes
 *
 * Each morph target is a position-delta array (float3 per vertex,
 * tightly packed).  The engine blends targets using weights each frame:
 *   final_pos = base_pos + sum(weight[i] * target[i][vertex])
 *
 * targets: packed float array, target_count * vertex_count * 3 floats.
 *          target[t] starts at offset t * vertex_count * 3.
 * weights: array of target_count blend weights (0.0 = off, 1.0 = full).
 * Ownership: MOP copies all data; the caller may free after return.
 * ------------------------------------------------------------------------- */

void mop_mesh_set_morph_targets(MopMesh *mesh, MopViewport *viewport,
                                const float *targets, const float *weights,
                                uint32_t target_count);

/* Update morph weights without re-uploading target data.
 * weights: array of target_count floats. */
void mop_mesh_set_morph_weights(MopMesh *mesh, const float *weights,
                                uint32_t target_count);

/* -------------------------------------------------------------------------
 * Instanced mesh API (Phase 6B)
 *
 * An instanced mesh stores a single piece of geometry with multiple
 * per-instance transforms.  The engine draws the mesh once per instance
 * using the instanced draw path.
 * ------------------------------------------------------------------------- */

typedef struct MopInstancedMesh MopInstancedMesh;

/* Add an instanced mesh to the viewport.  Returns NULL on failure.
 * The engine copies transforms; the application may free its array. */
MopInstancedMesh *mop_viewport_add_instanced_mesh(MopViewport *viewport,
                                                  const MopMeshDesc *desc,
                                                  const MopMat4 *transforms,
                                                  uint32_t instance_count);

/* Update the per-instance transforms of an instanced mesh. */
void mop_instanced_mesh_update_transforms(MopInstancedMesh *mesh,
                                          const MopMat4 *transforms,
                                          uint32_t count);

/* Remove an instanced mesh from the viewport and free its resources. */
void mop_viewport_remove_instanced_mesh(MopViewport *viewport,
                                        MopInstancedMesh *mesh);

/* -------------------------------------------------------------------------
 * LOD (Level of Detail) — Phase 9C
 *
 * Each mesh can carry a chain of LOD levels.  LOD 0 is the base mesh
 * (the geometry provided at creation time).  Additional levels have
 * progressively fewer triangles.  The engine selects the active LOD
 * based on screen-space projected size each frame.
 *
 * screen_threshold: if the mesh's projected diameter in pixels is below
 *   this value, the engine switches to the next lower LOD.  LOD 0 has
 *   no threshold (always used when the mesh is large enough).
 *
 * lod_bias: global LOD bias offset.  Positive = prefer lower detail,
 *   negative = prefer higher detail.  Default 0.0.
 * ------------------------------------------------------------------------- */

#define MOP_MAX_LOD_LEVELS 8

/* Add a LOD level to a mesh.  Returns the LOD index (1..MOP_MAX_LOD_LEVELS-1),
 * or -1 on failure.  LOD 0 is always the original mesh geometry.
 * screen_threshold: projected diameter in pixels below which this LOD
 *   is preferred over the previous (higher-detail) level. */
int32_t mop_mesh_add_lod(MopMesh *mesh, MopViewport *viewport,
                         const MopMeshDesc *desc, float screen_threshold);

/* Set the global LOD bias for a viewport.
 * Positive values shift towards lower detail; negative towards higher. */
void mop_viewport_set_lod_bias(MopViewport *viewport, float bias);
float mop_viewport_get_lod_bias(const MopViewport *viewport);

/* Set debug visualization mode for the viewport. */
void mop_viewport_set_debug_viz(MopViewport *viewport, MopDebugViz mode);
MopDebugViz mop_viewport_get_debug_viz(const MopViewport *viewport);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_SCENE_H */
