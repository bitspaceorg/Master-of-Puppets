/*
 * Master of Puppets — Test Suite
 * test_asset_pipeline.c — Phase 8: Asset Pipeline Tests
 *
 * Tests validate:
 *   - Material graph initialization, node management, connections
 *   - Material graph JSON serialization round-trip
 *   - Material graph presets
 *   - Texture pipeline format enum and stream state
 *   - Texture cache stats struct
 *   - glTF loader header structures
 *   - glTF scene struct sizes and defaults
 *   - Unified loader format enum (MOP_FORMAT_GLTF)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Material Graph — Phase 8A
 * ------------------------------------------------------------------------- */

static void test_mat_graph_init(void) {
  TEST_BEGIN("mat_graph_init");
  MopMaterialGraph graph;
  mop_mat_graph_init(&graph, "test_material");

  TEST_ASSERT(strcmp(graph.name, "test_material") == 0);
  TEST_ASSERT(graph.node_count == 1);
  TEST_ASSERT(graph.nodes[0].type == MOP_MAT_NODE_OUTPUT);
  TEST_ASSERT(graph.connection_count == 0);
  TEST_ASSERT(graph.texture_count == 0);
  TEST_ASSERT(graph.compiled == false);
  TEST_ASSERT(graph.preset == MOP_MAT_GRAPH_CUSTOM);
  mop_mat_graph_destroy(&graph);
  TEST_END();
}

static void test_mat_graph_add_node(void) {
  TEST_BEGIN("mat_graph_add_node");
  MopMaterialGraph graph;
  mop_mat_graph_init(&graph, "test");

  MopMatNode n = {.type = MOP_MAT_NODE_CONSTANT_FLOAT};
  snprintf(n.name, MOP_MAT_NODE_NAME_MAX, "my_float");
  n.params.constant_float.value = 0.5f;

  uint32_t idx = mop_mat_graph_add_node(&graph, &n);
  TEST_ASSERT(idx == 1);
  TEST_ASSERT(graph.node_count == 2);
  TEST_ASSERT(graph.nodes[1].type == MOP_MAT_NODE_CONSTANT_FLOAT);
  TEST_ASSERT(strcmp(graph.nodes[1].name, "my_float") == 0);
  TEST_ASSERT(graph.nodes[1].params.constant_float.value > 0.49f &&
              graph.nodes[1].params.constant_float.value < 0.51f);

  /* Cannot add a second output node */
  MopMatNode out = {.type = MOP_MAT_NODE_OUTPUT};
  uint32_t bad = mop_mat_graph_add_node(&graph, &out);
  TEST_ASSERT(bad == UINT32_MAX);

  mop_mat_graph_destroy(&graph);
  TEST_END();
}

static void test_mat_graph_connect(void) {
  TEST_BEGIN("mat_graph_connect");
  MopMaterialGraph graph;
  mop_mat_graph_init(&graph, "test");

  MopMatNode n1 = {.type = MOP_MAT_NODE_CONSTANT_VEC3};
  snprintf(n1.name, MOP_MAT_NODE_NAME_MAX, "color");
  uint32_t idx1 = mop_mat_graph_add_node(&graph, &n1);

  bool ok = mop_mat_graph_connect(&graph, idx1, 0, 0, 0);
  TEST_ASSERT(ok);
  TEST_ASSERT(graph.connection_count == 1);
  TEST_ASSERT(graph.connections[0].src_node == idx1);
  TEST_ASSERT(graph.connections[0].dst_node == 0);

  /* Self-connection should fail */
  ok = mop_mat_graph_connect(&graph, 0, 0, 0, 0);
  TEST_ASSERT(!ok);

  /* Out-of-range should fail */
  ok = mop_mat_graph_connect(&graph, 999, 0, 0, 0);
  TEST_ASSERT(!ok);

  mop_mat_graph_destroy(&graph);
  TEST_END();
}

static void test_mat_graph_null_safety(void) {
  TEST_BEGIN("mat_graph_null_safety");
  mop_mat_graph_init(NULL, "test");
  mop_mat_graph_destroy(NULL);
  uint32_t idx = mop_mat_graph_add_node(NULL, NULL);
  TEST_ASSERT(idx == UINT32_MAX);
  bool ok = mop_mat_graph_connect(NULL, 0, 0, 0, 0);
  TEST_ASSERT(!ok);
  char *json = mop_mat_graph_to_json(NULL);
  TEST_ASSERT(json == NULL);
  ok = mop_mat_graph_from_json(NULL, "{}");
  TEST_ASSERT(!ok);
  TEST_END();
}

static void test_mat_graph_preset_pbr(void) {
  TEST_BEGIN("mat_graph_preset_pbr");
  MopMaterialGraph graph;
  mop_mat_graph_preset_pbr(&graph);

  TEST_ASSERT(graph.preset == MOP_MAT_GRAPH_METALLIC_ROUGHNESS);
  TEST_ASSERT(graph.node_count >= 5); /* output + 3 tex samples + normal_map */
  TEST_ASSERT(graph.connection_count >= 5);
  TEST_ASSERT(strcmp(graph.name, "PBR Metallic-Roughness") == 0);

  mop_mat_graph_destroy(&graph);
  TEST_END();
}

static void test_mat_graph_json_roundtrip(void) {
  TEST_BEGIN("mat_graph_json_roundtrip");
  MopMaterialGraph graph;
  mop_mat_graph_init(&graph, "roundtrip_test");

  /* Add some nodes */
  MopMatNode float_node = {.type = MOP_MAT_NODE_CONSTANT_FLOAT};
  snprintf(float_node.name, MOP_MAT_NODE_NAME_MAX, "roughness");
  float_node.params.constant_float.value = 0.75f;
  uint32_t n1 = mop_mat_graph_add_node(&graph, &float_node);

  MopMatNode vec3_node = {.type = MOP_MAT_NODE_CONSTANT_VEC3};
  snprintf(vec3_node.name, MOP_MAT_NODE_NAME_MAX, "base_color");
  vec3_node.params.constant_vec3.rgb[0] = 0.8f;
  vec3_node.params.constant_vec3.rgb[1] = 0.2f;
  vec3_node.params.constant_vec3.rgb[2] = 0.1f;
  uint32_t n2 = mop_mat_graph_add_node(&graph, &vec3_node);

  mop_mat_graph_connect(&graph, n2, 0, 0, 0);
  mop_mat_graph_connect(&graph, n1, 0, 0, 2);

  /* Serialize */
  char *json = mop_mat_graph_to_json(&graph);
  TEST_ASSERT(json != NULL);

  /* Deserialize into new graph */
  MopMaterialGraph graph2;
  bool ok = mop_mat_graph_from_json(&graph2, json);
  TEST_ASSERT(ok);

  /* Verify */
  TEST_ASSERT(strcmp(graph2.name, "roundtrip_test") == 0);
  TEST_ASSERT(graph2.node_count == graph.node_count);
  TEST_ASSERT(graph2.connection_count == graph.connection_count);
  TEST_ASSERT(graph2.nodes[0].type == MOP_MAT_NODE_OUTPUT);
  TEST_ASSERT(graph2.nodes[1].type == MOP_MAT_NODE_CONSTANT_FLOAT);
  TEST_ASSERT(graph2.nodes[1].params.constant_float.value > 0.74f &&
              graph2.nodes[1].params.constant_float.value < 0.76f);
  TEST_ASSERT(graph2.nodes[2].type == MOP_MAT_NODE_CONSTANT_VEC3);
  TEST_ASSERT(graph2.nodes[2].params.constant_vec3.rgb[0] > 0.79f);

  free(json);
  mop_mat_graph_destroy(&graph);
  mop_mat_graph_destroy(&graph2);
  TEST_END();
}

static void test_mat_graph_node_types(void) {
  TEST_BEGIN("mat_graph_node_types");
  /* Verify all node type enum values are sequential */
  TEST_ASSERT(MOP_MAT_NODE_OUTPUT == 0);
  TEST_ASSERT(MOP_MAT_NODE_CONSTANT_FLOAT == 1);
  TEST_ASSERT(MOP_MAT_NODE_CONSTANT_VEC3 == 2);
  TEST_ASSERT(MOP_MAT_NODE_CONSTANT_VEC4 == 3);
  TEST_ASSERT(MOP_MAT_NODE_TEXTURE_SAMPLE == 4);
  TEST_ASSERT(MOP_MAT_NODE_NORMAL_MAP == 5);
  TEST_ASSERT(MOP_MAT_NODE_MIX == 6);
  TEST_ASSERT(MOP_MAT_NODE_MULTIPLY == 7);
  TEST_ASSERT(MOP_MAT_NODE_ADD == 8);
  TEST_ASSERT(MOP_MAT_NODE_FRESNEL == 9);
  TEST_ASSERT(MOP_MAT_NODE_UV_TRANSFORM == 10);
  TEST_ASSERT(MOP_MAT_NODE_VERTEX_COLOR == 11);
  TEST_ASSERT(MOP_MAT_NODE_COUNT == 12);
  TEST_END();
}

static void test_mat_graph_max_constants(void) {
  TEST_BEGIN("mat_graph_max_constants");
  TEST_ASSERT(MOP_MAT_MAX_NODES >= 32);
  TEST_ASSERT(MOP_MAT_MAX_NODES <= 256);
  TEST_ASSERT(MOP_MAT_MAX_CONNECTIONS >= 64);
  TEST_ASSERT(MOP_MAT_MAX_CONNECTIONS <= 512);
  TEST_ASSERT(MOP_MAT_MAX_TEXTURES >= 4);
  TEST_ASSERT(MOP_MAT_MAX_TEXTURES <= 32);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Texture Pipeline — Phase 8B
 * ------------------------------------------------------------------------- */

static void test_tex_format_enum(void) {
  TEST_BEGIN("tex_format_enum");
  TEST_ASSERT(MOP_TEX_FORMAT_RGBA8 == 0);
  TEST_ASSERT(MOP_TEX_FORMAT_BC1 == 1);
  TEST_ASSERT(MOP_TEX_FORMAT_BC3 == 2);
  TEST_ASSERT(MOP_TEX_FORMAT_BC5 == 3);
  TEST_ASSERT(MOP_TEX_FORMAT_BC7 == 4);
  TEST_ASSERT(MOP_TEX_FORMAT_COUNT == 5);
  TEST_END();
}

static void test_tex_stream_state_enum(void) {
  TEST_BEGIN("tex_stream_state_enum");
  TEST_ASSERT(MOP_TEX_STREAM_PENDING == 0);
  TEST_ASSERT(MOP_TEX_STREAM_PARTIAL == 1);
  TEST_ASSERT(MOP_TEX_STREAM_COMPLETE == 2);
  TEST_ASSERT(MOP_TEX_STREAM_ERROR == 3);
  TEST_END();
}

static void test_tex_cache_stats_struct(void) {
  TEST_BEGIN("tex_cache_stats_struct");
  MopTexCacheStats stats;
  memset(&stats, 0, sizeof(stats));
  TEST_ASSERT(stats.total_textures == 0);
  TEST_ASSERT(stats.unique_textures == 0);
  TEST_ASSERT(stats.memory_used == 0);
  TEST_ASSERT(stats.pending_loads == 0);
  TEST_ASSERT(stats.cache_hits == 0);
  /* Struct should be compact */
  TEST_ASSERT(sizeof(MopTexCacheStats) <= 64);
  TEST_END();
}

static void test_tex_null_safety(void) {
  TEST_BEGIN("tex_null_safety");
  MopTexture *t = mop_tex_create(NULL, NULL);
  TEST_ASSERT(t == NULL);
  t = mop_tex_load_async(NULL, NULL);
  TEST_ASSERT(t == NULL);
  MopTexStreamState state = mop_tex_get_stream_state(NULL);
  TEST_ASSERT(state == MOP_TEX_STREAM_ERROR);
  uint64_t h = mop_tex_get_hash(NULL);
  TEST_ASSERT(h == 0);
  MopTexCacheStats s = mop_tex_cache_stats(NULL);
  TEST_ASSERT(s.total_textures == 0);
  mop_tex_cache_flush(NULL, 100);
  TEST_ASSERT(1); /* no crash */
  TEST_END();
}

static void test_tex_desc_struct(void) {
  TEST_BEGIN("tex_desc_struct");
  MopTextureDesc desc;
  memset(&desc, 0, sizeof(desc));
  desc.width = 512;
  desc.height = 512;
  desc.format = MOP_TEX_FORMAT_BC7;
  desc.mip_levels = 10;
  desc.srgb = true;
  desc.generate_mips = false;
  TEST_ASSERT(desc.width == 512);
  TEST_ASSERT(desc.format == MOP_TEX_FORMAT_BC7);
  TEST_ASSERT(desc.srgb == true);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * glTF Loader — Phase 8C
 * ------------------------------------------------------------------------- */

static void test_gltf_scene_struct(void) {
  TEST_BEGIN("gltf_scene_struct");
  MopGltfScene scene;
  memset(&scene, 0, sizeof(scene));
  TEST_ASSERT(scene.mesh_count == 0);
  TEST_ASSERT(scene.material_count == 0);
  TEST_ASSERT(scene.image_count == 0);
  TEST_ASSERT(scene.node_count == 0);
  TEST_ASSERT(scene.skin_count == 0);
  TEST_ASSERT(scene.meshes == NULL);
  TEST_ASSERT(scene._buffer_data == NULL);
  TEST_END();
}

static void test_gltf_material_defaults(void) {
  TEST_BEGIN("gltf_material_defaults");
  MopGltfMaterial mat;
  memset(&mat, 0, sizeof(mat));
  TEST_ASSERT(mat.alpha_mode == MOP_GLTF_ALPHA_OPAQUE);
  TEST_ASSERT(mat.double_sided == false);
  TEST_ASSERT(mat.unlit == false);
  /* Struct should be reasonably sized */
  TEST_ASSERT(sizeof(MopGltfMaterial) < 256);
  TEST_END();
}

static void test_gltf_tex_ref(void) {
  TEST_BEGIN("gltf_tex_ref");
  MopGltfTexRef ref;
  memset(&ref, 0, sizeof(ref));
  ref.image_index = -1;
  ref.tex_coord = 1;
  TEST_ASSERT(ref.image_index == -1);
  TEST_ASSERT(ref.tex_coord == 1);
  TEST_ASSERT(sizeof(MopGltfTexRef) <= 16);
  TEST_END();
}

static void test_gltf_primitive_struct(void) {
  TEST_BEGIN("gltf_primitive_struct");
  MopGltfPrimitive prim;
  memset(&prim, 0, sizeof(prim));
  prim.material_index = -1;
  TEST_ASSERT(prim.vertices == NULL);
  TEST_ASSERT(prim.indices == NULL);
  TEST_ASSERT(prim.tangents == NULL);
  TEST_ASSERT(prim.joints == NULL);
  TEST_ASSERT(prim.weights == NULL);
  TEST_ASSERT(prim.material_index == -1);
  TEST_END();
}

static void test_gltf_node_struct(void) {
  TEST_BEGIN("gltf_node_struct");
  MopGltfNode node;
  memset(&node, 0, sizeof(node));
  node.mesh_index = -1;
  node.skin_index = -1;
  node.parent_index = -1;
  TEST_ASSERT(node.mesh_index == -1);
  TEST_ASSERT(node.has_matrix == false);
  TEST_ASSERT(node.children == NULL);
  TEST_ASSERT(node.child_count == 0);
  TEST_END();
}

static void test_gltf_null_safety(void) {
  TEST_BEGIN("gltf_null_safety");
  bool ok = mop_gltf_load(NULL, NULL);
  TEST_ASSERT(!ok);

  MopGltfScene scene;
  ok = mop_gltf_load(NULL, &scene);
  TEST_ASSERT(!ok);

  ok = mop_gltf_load("/nonexistent/path.glb", &scene);
  TEST_ASSERT(!ok);

  mop_gltf_free(NULL); /* should not crash */

  uint32_t n = mop_gltf_import(NULL, NULL, 0);
  TEST_ASSERT(n == 0);
  TEST_ASSERT(1); /* no crash */
  TEST_END();
}

static void test_gltf_skin_struct(void) {
  TEST_BEGIN("gltf_skin_struct");
  MopGltfSkin skin;
  memset(&skin, 0, sizeof(skin));
  skin.skeleton_root = -1;
  TEST_ASSERT(skin.joints == NULL);
  TEST_ASSERT(skin.joint_count == 0);
  TEST_ASSERT(skin.inverse_bind_matrices == NULL);
  TEST_ASSERT(skin.skeleton_root == -1);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * Unified loader — glTF dispatch
 * ------------------------------------------------------------------------- */

static void test_loader_gltf_format(void) {
  TEST_BEGIN("loader_gltf_format");
  TEST_ASSERT(MOP_FORMAT_GLTF == 3);
  /* Verify the enum is sequential */
  TEST_ASSERT(MOP_FORMAT_UNKNOWN == 0);
  TEST_ASSERT(MOP_FORMAT_OBJ == 1);
  TEST_ASSERT(MOP_FORMAT_MOP_BINARY == 2);
  TEST_ASSERT(MOP_FORMAT_GLTF == 3);
  TEST_END();
}

static void test_loader_gltf_nonexistent(void) {
  TEST_BEGIN("loader_gltf_nonexistent");
  MopLoadedMesh mesh;
  bool ok = mop_load("/nonexistent.glb", &mesh);
  TEST_ASSERT(!ok);
  ok = mop_load("/nonexistent.gltf", &mesh);
  TEST_ASSERT(!ok);
  TEST_END();
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
  printf("=== Asset Pipeline Tests (Phase 8) ===\n");

  /* Phase 8A: Material Graph */
  test_mat_graph_init();
  test_mat_graph_add_node();
  test_mat_graph_connect();
  test_mat_graph_null_safety();
  test_mat_graph_preset_pbr();
  test_mat_graph_json_roundtrip();
  test_mat_graph_node_types();
  test_mat_graph_max_constants();

  /* Phase 8B: Texture Pipeline */
  test_tex_format_enum();
  test_tex_stream_state_enum();
  test_tex_cache_stats_struct();
  test_tex_null_safety();
  test_tex_desc_struct();

  /* Phase 8C: glTF Loader */
  test_gltf_scene_struct();
  test_gltf_material_defaults();
  test_gltf_tex_ref();
  test_gltf_primitive_struct();
  test_gltf_node_struct();
  test_gltf_null_safety();
  test_gltf_skin_struct();
  test_gltf_skin_struct();

  /* Unified loader glTF dispatch */
  test_loader_gltf_format();
  test_loader_gltf_nonexistent();

  TEST_REPORT();
  TEST_EXIT();
}
