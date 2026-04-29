/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * material_graph.h — Node-based material description (Phase 8A)
 *
 * Materials are described as a DAG of interconnected nodes.  Each node
 * represents an operation (texture sample, math, constant value, etc.)
 * and produces one or more outputs that can be connected to other nodes.
 *
 * The graph can be serialized to/from JSON for interchange with DCC
 * tools.  At compile time, the graph is flattened into GLSL fragment
 * shader source which is then compiled to SPIR-V and registered as a
 * shader plugin.
 *
 * Built-in material types (metallic-roughness, specular-glossiness,
 * unlit) are provided as pre-built graphs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_MATERIAL_GRAPH_H
#define MOP_CORE_MATERIAL_GRAPH_H

#include <mop/core/material.h>
#include <mop/types.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Node types
 * ------------------------------------------------------------------------- */

typedef enum MopMatNodeType {
  MOP_MAT_NODE_OUTPUT = 0,     /* final PBR output (always one per graph) */
  MOP_MAT_NODE_CONSTANT_FLOAT, /* float constant */
  MOP_MAT_NODE_CONSTANT_VEC3,  /* vec3 constant */
  MOP_MAT_NODE_CONSTANT_VEC4,  /* vec4 constant (color with alpha) */
  MOP_MAT_NODE_TEXTURE_SAMPLE, /* sample texture at UV */
  MOP_MAT_NODE_NORMAL_MAP,     /* tangent-space normal map */
  MOP_MAT_NODE_MIX,            /* linear interpolation: mix(A, B, factor) */
  MOP_MAT_NODE_MULTIPLY,       /* component-wise multiply */
  MOP_MAT_NODE_ADD,            /* component-wise add */
  MOP_MAT_NODE_FRESNEL,        /* Schlick fresnel (IOR or F0) */
  MOP_MAT_NODE_UV_TRANSFORM,   /* scale + offset UV coordinates */
  MOP_MAT_NODE_VERTEX_COLOR,   /* per-vertex color attribute */
  MOP_MAT_NODE_COUNT
} MopMatNodeType;

/* -------------------------------------------------------------------------
 * Connection — links one node's output to another node's input
 * ------------------------------------------------------------------------- */

typedef struct MopMatConnection {
  uint32_t src_node;   /* source node index */
  uint32_t src_output; /* source output slot (0-based) */
  uint32_t dst_node;   /* destination node index */
  uint32_t dst_input;  /* destination input slot (0-based) */
} MopMatConnection;

/* -------------------------------------------------------------------------
 * Node — single operation in the material graph
 * ------------------------------------------------------------------------- */

#define MOP_MAT_NODE_NAME_MAX 32

typedef struct MopMatNode {
  MopMatNodeType type;
  char name[MOP_MAT_NODE_NAME_MAX];

  /* Node-specific parameters (union by type) */
  union {
    struct {
      float value;
    } constant_float;
    struct {
      float rgb[3];
    } constant_vec3;
    struct {
      float rgba[4];
    } constant_vec4;
    struct {
      int32_t texture_index; /* index into material's texture list */
      int32_t uv_set;        /* 0 or 1 */
    } texture_sample;
    struct {
      float strength; /* normal map strength (default 1.0) */
    } normal_map;
    struct {
      float factor; /* mix factor (0..1), overridden by connection */
    } mix;
    struct {
      float ior; /* index of refraction (default 1.5) */
    } fresnel;
    struct {
      float scale[2];  /* UV scale */
      float offset[2]; /* UV offset */
      float rotation;  /* UV rotation (radians) */
    } uv_transform;
  } params;
} MopMatNode;

/* -------------------------------------------------------------------------
 * Material graph
 * ------------------------------------------------------------------------- */

#define MOP_MAT_MAX_NODES 64
#define MOP_MAT_MAX_CONNECTIONS 128
#define MOP_MAT_MAX_TEXTURES 8

typedef struct MopMaterialGraph {
  char name[64];

  MopMatNode nodes[MOP_MAT_MAX_NODES];
  uint32_t node_count;

  MopMatConnection connections[MOP_MAT_MAX_CONNECTIONS];
  uint32_t connection_count;

  /* Texture references used by texture_sample nodes.
   * Index corresponds to texture_sample.texture_index.
   * Actual MopTexture* are bound at render time. */
  char texture_paths[MOP_MAT_MAX_TEXTURES][256];
  uint32_t texture_count;

  /* Pre-built graph type for standard materials */
  enum {
    MOP_MAT_GRAPH_CUSTOM = 0,
    MOP_MAT_GRAPH_METALLIC_ROUGHNESS,
    MOP_MAT_GRAPH_SPECULAR_GLOSSINESS,
    MOP_MAT_GRAPH_UNLIT,
  } preset;

  /* Compiled state (set by mop_mat_graph_compile) */
  bool compiled;
  char *_compiled_glsl; /* generated GLSL source (internal, freed by graph) */
} MopMaterialGraph;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Initialize a material graph with an output node. */
void mop_mat_graph_init(MopMaterialGraph *graph, const char *name);

/* Add a node to the graph.  Returns the node index, or UINT32_MAX on failure.*/
uint32_t mop_mat_graph_add_node(MopMaterialGraph *graph,
                                const MopMatNode *node);

/* Connect src_node:output -> dst_node:input.  Returns true on success. */
bool mop_mat_graph_connect(MopMaterialGraph *graph, uint32_t src_node,
                           uint32_t src_output, uint32_t dst_node,
                           uint32_t dst_input);

/* Create a pre-built metallic-roughness PBR graph. */
void mop_mat_graph_preset_pbr(MopMaterialGraph *graph);

/* Serialize graph to JSON string.  Caller must free() the returned string. */
char *mop_mat_graph_to_json(const MopMaterialGraph *graph);

/* Deserialize graph from JSON string.  Returns true on success. */
bool mop_mat_graph_from_json(MopMaterialGraph *graph, const char *json);

/* Compile the material graph into a flat MopMaterial.
 * Evaluates constant nodes, resolves texture paths via the viewport's
 * texture pipeline, and folds math nodes into final PBR parameters.
 * Returns true on success.  On failure, out_material is zero-initialized
 * and a warning is logged. */
bool mop_mat_graph_compile(MopMaterialGraph *graph, MopViewport *viewport,
                           MopMaterial *out_material);

/* Free any allocated resources in the graph (compiled GLSL, etc.) */
void mop_mat_graph_destroy(MopMaterialGraph *graph);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_MATERIAL_GRAPH_H */
