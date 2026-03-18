/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * material_graph.c — Node-based material description (Phase 8A)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/core/material_graph.h>
#include <mop/util/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------------- */

void mop_mat_graph_init(MopMaterialGraph *graph, const char *name) {
  if (!graph)
    return;
  memset(graph, 0, sizeof(*graph));
  if (name)
    snprintf(graph->name, sizeof(graph->name), "%s", name);

  /* Every graph starts with exactly one output node at index 0 */
  graph->nodes[0].type = MOP_MAT_NODE_OUTPUT;
  snprintf(graph->nodes[0].name, MOP_MAT_NODE_NAME_MAX, "output");
  graph->node_count = 1;
  graph->preset = MOP_MAT_GRAPH_CUSTOM;
}

/* -------------------------------------------------------------------------
 * Node management
 * ------------------------------------------------------------------------- */

uint32_t mop_mat_graph_add_node(MopMaterialGraph *graph,
                                const MopMatNode *node) {
  if (!graph || !node) {
    MOP_WARN("mop_mat_graph_add_node: NULL graph or node");
    return UINT32_MAX;
  }
  if (graph->node_count >= MOP_MAT_MAX_NODES) {
    MOP_WARN("mop_mat_graph_add_node: max nodes reached (%u)",
             MOP_MAT_MAX_NODES);
    return UINT32_MAX;
  }
  if (node->type == MOP_MAT_NODE_OUTPUT) {
    MOP_WARN("mop_mat_graph_add_node: only one output node allowed");
    return UINT32_MAX;
  }

  uint32_t idx = graph->node_count++;
  graph->nodes[idx] = *node;
  graph->compiled = false;
  return idx;
}

/* -------------------------------------------------------------------------
 * Connections
 * ------------------------------------------------------------------------- */

bool mop_mat_graph_connect(MopMaterialGraph *graph, uint32_t src_node,
                           uint32_t src_output, uint32_t dst_node,
                           uint32_t dst_input) {
  if (!graph)
    return false;
  if (src_node >= graph->node_count || dst_node >= graph->node_count) {
    MOP_WARN("mop_mat_graph_connect: node index out of range");
    return false;
  }
  if (src_node == dst_node) {
    MOP_WARN("mop_mat_graph_connect: self-connection not allowed");
    return false;
  }
  if (graph->connection_count >= MOP_MAT_MAX_CONNECTIONS) {
    MOP_WARN("mop_mat_graph_connect: max connections reached (%u)",
             MOP_MAT_MAX_CONNECTIONS);
    return false;
  }

  MopMatConnection *c = &graph->connections[graph->connection_count++];
  c->src_node = src_node;
  c->src_output = src_output;
  c->dst_node = dst_node;
  c->dst_input = dst_input;
  graph->compiled = false;
  return true;
}

/* -------------------------------------------------------------------------
 * Preset: metallic-roughness PBR
 * ------------------------------------------------------------------------- */

void mop_mat_graph_preset_pbr(MopMaterialGraph *graph) {
  if (!graph)
    return;
  mop_mat_graph_init(graph, "PBR Metallic-Roughness");
  graph->preset = MOP_MAT_GRAPH_METALLIC_ROUGHNESS;

  /* Node 0 is output (already created by init) */

  /* Node 1: base color texture */
  MopMatNode base_color = {.type = MOP_MAT_NODE_TEXTURE_SAMPLE};
  snprintf(base_color.name, MOP_MAT_NODE_NAME_MAX, "base_color_tex");
  base_color.params.texture_sample.texture_index = 0;
  base_color.params.texture_sample.uv_set = 0;
  uint32_t n1 = mop_mat_graph_add_node(graph, &base_color);

  /* Node 2: metallic-roughness texture */
  MopMatNode mr = {.type = MOP_MAT_NODE_TEXTURE_SAMPLE};
  snprintf(mr.name, MOP_MAT_NODE_NAME_MAX, "mr_tex");
  mr.params.texture_sample.texture_index = 1;
  mr.params.texture_sample.uv_set = 0;
  uint32_t n2 = mop_mat_graph_add_node(graph, &mr);

  /* Node 3: normal map */
  MopMatNode normal = {.type = MOP_MAT_NODE_NORMAL_MAP};
  snprintf(normal.name, MOP_MAT_NODE_NAME_MAX, "normal_map");
  normal.params.normal_map.strength = 1.0f;
  uint32_t n3 = mop_mat_graph_add_node(graph, &normal);

  /* Node 4: normal map texture */
  MopMatNode normal_tex = {.type = MOP_MAT_NODE_TEXTURE_SAMPLE};
  snprintf(normal_tex.name, MOP_MAT_NODE_NAME_MAX, "normal_tex");
  normal_tex.params.texture_sample.texture_index = 2;
  normal_tex.params.texture_sample.uv_set = 0;
  uint32_t n4 = mop_mat_graph_add_node(graph, &normal_tex);

  /* Connect: base_color_tex:0 -> output:0 (base_color) */
  mop_mat_graph_connect(graph, n1, 0, 0, 0);
  /* Connect: mr_tex:0 -> output:1 (metallic) — blue channel */
  mop_mat_graph_connect(graph, n2, 0, 0, 1);
  /* Connect: mr_tex:0 -> output:2 (roughness) — green channel */
  mop_mat_graph_connect(graph, n2, 0, 0, 2);
  /* Connect: normal_tex:0 -> normal_map:0 */
  mop_mat_graph_connect(graph, n4, 0, n3, 0);
  /* Connect: normal_map:0 -> output:3 (normal) */
  mop_mat_graph_connect(graph, n3, 0, 0, 3);
}

/* -------------------------------------------------------------------------
 * JSON serialization
 * ------------------------------------------------------------------------- */

/* Node type name table */
static const char *s_node_type_names[] = {
    [MOP_MAT_NODE_OUTPUT] = "output",
    [MOP_MAT_NODE_CONSTANT_FLOAT] = "constant_float",
    [MOP_MAT_NODE_CONSTANT_VEC3] = "constant_vec3",
    [MOP_MAT_NODE_CONSTANT_VEC4] = "constant_vec4",
    [MOP_MAT_NODE_TEXTURE_SAMPLE] = "texture_sample",
    [MOP_MAT_NODE_NORMAL_MAP] = "normal_map",
    [MOP_MAT_NODE_MIX] = "mix",
    [MOP_MAT_NODE_MULTIPLY] = "multiply",
    [MOP_MAT_NODE_ADD] = "add",
    [MOP_MAT_NODE_FRESNEL] = "fresnel",
    [MOP_MAT_NODE_UV_TRANSFORM] = "uv_transform",
    [MOP_MAT_NODE_VERTEX_COLOR] = "vertex_color",
};

static MopMatNodeType node_type_from_name(const char *name) {
  for (int i = 0; i < MOP_MAT_NODE_COUNT; i++) {
    if (s_node_type_names[i] && strcmp(s_node_type_names[i], name) == 0)
      return (MopMatNodeType)i;
  }
  return MOP_MAT_NODE_COUNT; /* invalid */
}

/* Minimal JSON writer — appends to a growable buffer */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} JsonBuf;

static void jb_init(JsonBuf *jb) {
  jb->cap = 4096;
  jb->buf = (char *)malloc(jb->cap);
  jb->len = 0;
  if (jb->buf)
    jb->buf[0] = '\0';
}

static void jb_append(JsonBuf *jb, const char *fmt, ...) {
  if (!jb->buf)
    return;
  va_list ap;
  va_start(ap, fmt);
  int needed = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (needed < 0)
    return;

  while (jb->len + (size_t)needed + 1 > jb->cap) {
    jb->cap *= 2;
    char *nb = (char *)realloc(jb->buf, jb->cap);
    if (!nb) {
      free(jb->buf);
      jb->buf = NULL;
      return;
    }
    jb->buf = nb;
  }

  va_start(ap, fmt);
  vsnprintf(jb->buf + jb->len, jb->cap - jb->len, fmt, ap);
  va_end(ap);
  jb->len += (size_t)needed;
}

static void write_node_params(JsonBuf *jb, const MopMatNode *n) {
  switch (n->type) {
  case MOP_MAT_NODE_CONSTANT_FLOAT:
    jb_append(jb, ",\"value\":%.6g", (double)n->params.constant_float.value);
    break;
  case MOP_MAT_NODE_CONSTANT_VEC3:
    jb_append(jb, ",\"rgb\":[%.6g,%.6g,%.6g]",
              (double)n->params.constant_vec3.rgb[0],
              (double)n->params.constant_vec3.rgb[1],
              (double)n->params.constant_vec3.rgb[2]);
    break;
  case MOP_MAT_NODE_CONSTANT_VEC4:
    jb_append(jb, ",\"rgba\":[%.6g,%.6g,%.6g,%.6g]",
              (double)n->params.constant_vec4.rgba[0],
              (double)n->params.constant_vec4.rgba[1],
              (double)n->params.constant_vec4.rgba[2],
              (double)n->params.constant_vec4.rgba[3]);
    break;
  case MOP_MAT_NODE_TEXTURE_SAMPLE:
    jb_append(jb, ",\"texture_index\":%d,\"uv_set\":%d",
              n->params.texture_sample.texture_index,
              n->params.texture_sample.uv_set);
    break;
  case MOP_MAT_NODE_NORMAL_MAP:
    jb_append(jb, ",\"strength\":%.6g", (double)n->params.normal_map.strength);
    break;
  case MOP_MAT_NODE_MIX:
    jb_append(jb, ",\"factor\":%.6g", (double)n->params.mix.factor);
    break;
  case MOP_MAT_NODE_FRESNEL:
    jb_append(jb, ",\"ior\":%.6g", (double)n->params.fresnel.ior);
    break;
  case MOP_MAT_NODE_UV_TRANSFORM:
    jb_append(jb,
              ",\"scale\":[%.6g,%.6g],\"offset\":[%.6g,%.6g]"
              ",\"rotation\":%.6g",
              (double)n->params.uv_transform.scale[0],
              (double)n->params.uv_transform.scale[1],
              (double)n->params.uv_transform.offset[0],
              (double)n->params.uv_transform.offset[1],
              (double)n->params.uv_transform.rotation);
    break;
  default:
    break;
  }
}

char *mop_mat_graph_to_json(const MopMaterialGraph *graph) {
  if (!graph)
    return NULL;

  JsonBuf jb;
  jb_init(&jb);

  jb_append(&jb, "{\"name\":\"%s\",\"preset\":%d,\"nodes\":[", graph->name,
            graph->preset);

  for (uint32_t i = 0; i < graph->node_count; i++) {
    const MopMatNode *n = &graph->nodes[i];
    if (i > 0)
      jb_append(&jb, ",");
    jb_append(&jb, "{\"type\":\"%s\",\"name\":\"%s\"",
              s_node_type_names[n->type], n->name);
    write_node_params(&jb, n);
    jb_append(&jb, "}");
  }

  jb_append(&jb, "],\"connections\":[");
  for (uint32_t i = 0; i < graph->connection_count; i++) {
    const MopMatConnection *c = &graph->connections[i];
    if (i > 0)
      jb_append(&jb, ",");
    jb_append(&jb,
              "{\"src_node\":%u,\"src_output\":%u,"
              "\"dst_node\":%u,\"dst_input\":%u}",
              c->src_node, c->src_output, c->dst_node, c->dst_input);
  }

  jb_append(&jb, "],\"textures\":[");
  for (uint32_t i = 0; i < graph->texture_count; i++) {
    if (i > 0)
      jb_append(&jb, ",");
    jb_append(&jb, "\"%s\"", graph->texture_paths[i]);
  }
  jb_append(&jb, "]}");

  return jb.buf;
}

/* -------------------------------------------------------------------------
 * JSON deserialization (minimal hand-rolled parser)
 *
 * We support the exact schema produced by mop_mat_graph_to_json.
 * This is intentionally simple — no general JSON library needed.
 * ------------------------------------------------------------------------- */

/* Skip whitespace */
static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  return p;
}

/* Read a JSON string value into dst (max dstlen-1 chars).
 * p should point at the opening '"'.  Returns pointer past closing '"'. */
static const char *read_string(const char *p, char *dst, size_t dstlen) {
  if (*p != '"')
    return p;
  p++;
  size_t i = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && *(p + 1))
      p++; /* skip escape */
    if (i + 1 < dstlen)
      dst[i++] = *p;
    p++;
  }
  dst[i] = '\0';
  if (*p == '"')
    p++;
  return p;
}

/* Read a number (int or float).  Returns pointer past the number. */
static const char *read_number(const char *p, double *out) {
  char *end;
  *out = strtod(p, &end);
  return end;
}

/* Skip a JSON value (string, number, object, array, bool, null). */
static const char *skip_value(const char *p) {
  p = skip_ws(p);
  if (*p == '"') {
    p++;
    while (*p && *p != '"') {
      if (*p == '\\')
        p++;
      p++;
    }
    if (*p == '"')
      p++;
  } else if (*p == '{') {
    int depth = 1;
    p++;
    while (*p && depth > 0) {
      if (*p == '{')
        depth++;
      else if (*p == '}')
        depth--;
      else if (*p == '"') {
        p++;
        while (*p && *p != '"') {
          if (*p == '\\')
            p++;
          p++;
        }
      }
      p++;
    }
  } else if (*p == '[') {
    int depth = 1;
    p++;
    while (*p && depth > 0) {
      if (*p == '[')
        depth++;
      else if (*p == ']')
        depth--;
      else if (*p == '"') {
        p++;
        while (*p && *p != '"') {
          if (*p == '\\')
            p++;
          p++;
        }
      }
      p++;
    }
  } else {
    while (*p && *p != ',' && *p != '}' && *p != ']')
      p++;
  }
  return p;
}

bool mop_mat_graph_from_json(MopMaterialGraph *graph, const char *json) {
  if (!graph || !json)
    return false;
  memset(graph, 0, sizeof(*graph));

  const char *p = skip_ws(json);
  if (*p != '{')
    return false;
  p++;

  while (*p && *p != '}') {
    p = skip_ws(p);
    if (*p != '"') {
      p++;
      continue;
    }

    char key[64] = {0};
    p = read_string(p, key, sizeof(key));
    p = skip_ws(p);
    if (*p == ':')
      p++;
    p = skip_ws(p);

    if (strcmp(key, "name") == 0) {
      p = read_string(p, graph->name, sizeof(graph->name));
    } else if (strcmp(key, "preset") == 0) {
      double v;
      p = read_number(p, &v);
      graph->preset = (int)v;
    } else if (strcmp(key, "nodes") == 0 && *p == '[') {
      p++; /* skip '[' */
      while (*p && *p != ']') {
        p = skip_ws(p);
        if (*p == ',') {
          p++;
          continue;
        }
        if (*p != '{')
          break;
        p++; /* skip '{' */

        MopMatNode node;
        memset(&node, 0, sizeof(node));
        char type_name[64] = {0};

        while (*p && *p != '}') {
          p = skip_ws(p);
          if (*p == ',') {
            p++;
            continue;
          }
          if (*p != '"')
            break;

          char nkey[64] = {0};
          p = read_string(p, nkey, sizeof(nkey));
          p = skip_ws(p);
          if (*p == ':')
            p++;
          p = skip_ws(p);

          if (strcmp(nkey, "type") == 0) {
            p = read_string(p, type_name, sizeof(type_name));
          } else if (strcmp(nkey, "name") == 0) {
            p = read_string(p, node.name, MOP_MAT_NODE_NAME_MAX);
          } else if (strcmp(nkey, "value") == 0) {
            double v;
            p = read_number(p, &v);
            node.params.constant_float.value = (float)v;
          } else if (strcmp(nkey, "rgb") == 0 && *p == '[') {
            p++;
            for (int i = 0; i < 3; i++) {
              p = skip_ws(p);
              double v;
              p = read_number(p, &v);
              node.params.constant_vec3.rgb[i] = (float)v;
              p = skip_ws(p);
              if (*p == ',')
                p++;
            }
            p = skip_ws(p);
            if (*p == ']')
              p++;
          } else if (strcmp(nkey, "rgba") == 0 && *p == '[') {
            p++;
            for (int i = 0; i < 4; i++) {
              p = skip_ws(p);
              double v;
              p = read_number(p, &v);
              node.params.constant_vec4.rgba[i] = (float)v;
              p = skip_ws(p);
              if (*p == ',')
                p++;
            }
            p = skip_ws(p);
            if (*p == ']')
              p++;
          } else if (strcmp(nkey, "texture_index") == 0) {
            double v;
            p = read_number(p, &v);
            node.params.texture_sample.texture_index = (int32_t)v;
          } else if (strcmp(nkey, "uv_set") == 0) {
            double v;
            p = read_number(p, &v);
            node.params.texture_sample.uv_set = (int32_t)v;
          } else if (strcmp(nkey, "strength") == 0) {
            double v;
            p = read_number(p, &v);
            node.params.normal_map.strength = (float)v;
          } else if (strcmp(nkey, "factor") == 0) {
            double v;
            p = read_number(p, &v);
            node.params.mix.factor = (float)v;
          } else if (strcmp(nkey, "ior") == 0) {
            double v;
            p = read_number(p, &v);
            node.params.fresnel.ior = (float)v;
          } else if (strcmp(nkey, "scale") == 0 && *p == '[') {
            p++;
            for (int i = 0; i < 2; i++) {
              p = skip_ws(p);
              double v;
              p = read_number(p, &v);
              node.params.uv_transform.scale[i] = (float)v;
              p = skip_ws(p);
              if (*p == ',')
                p++;
            }
            p = skip_ws(p);
            if (*p == ']')
              p++;
          } else if (strcmp(nkey, "offset") == 0 && *p == '[') {
            p++;
            for (int i = 0; i < 2; i++) {
              p = skip_ws(p);
              double v;
              p = read_number(p, &v);
              node.params.uv_transform.offset[i] = (float)v;
              p = skip_ws(p);
              if (*p == ',')
                p++;
            }
            p = skip_ws(p);
            if (*p == ']')
              p++;
          } else if (strcmp(nkey, "rotation") == 0) {
            double v;
            p = read_number(p, &v);
            node.params.uv_transform.rotation = (float)v;
          } else {
            p = skip_value(p);
          }
        }
        if (*p == '}')
          p++;

        node.type = node_type_from_name(type_name);
        if (node.type < MOP_MAT_NODE_COUNT) {
          if (graph->node_count < MOP_MAT_MAX_NODES)
            graph->nodes[graph->node_count++] = node;
        }

        p = skip_ws(p);
        if (*p == ',')
          p++;
      }
      if (*p == ']')
        p++;
    } else if (strcmp(key, "connections") == 0 && *p == '[') {
      p++;
      while (*p && *p != ']') {
        p = skip_ws(p);
        if (*p == ',') {
          p++;
          continue;
        }
        if (*p != '{')
          break;
        p++;

        MopMatConnection conn = {0};
        while (*p && *p != '}') {
          p = skip_ws(p);
          if (*p == ',') {
            p++;
            continue;
          }
          if (*p != '"')
            break;

          char ckey[64] = {0};
          p = read_string(p, ckey, sizeof(ckey));
          p = skip_ws(p);
          if (*p == ':')
            p++;
          p = skip_ws(p);
          double v;
          p = read_number(p, &v);

          if (strcmp(ckey, "src_node") == 0)
            conn.src_node = (uint32_t)v;
          else if (strcmp(ckey, "src_output") == 0)
            conn.src_output = (uint32_t)v;
          else if (strcmp(ckey, "dst_node") == 0)
            conn.dst_node = (uint32_t)v;
          else if (strcmp(ckey, "dst_input") == 0)
            conn.dst_input = (uint32_t)v;
        }
        if (*p == '}')
          p++;

        if (graph->connection_count < MOP_MAT_MAX_CONNECTIONS)
          graph->connections[graph->connection_count++] = conn;

        p = skip_ws(p);
        if (*p == ',')
          p++;
      }
      if (*p == ']')
        p++;
    } else if (strcmp(key, "textures") == 0 && *p == '[') {
      p++;
      while (*p && *p != ']') {
        p = skip_ws(p);
        if (*p == ',') {
          p++;
          continue;
        }
        if (*p != '"')
          break;
        if (graph->texture_count < MOP_MAT_MAX_TEXTURES) {
          p = read_string(p, graph->texture_paths[graph->texture_count],
                          sizeof(graph->texture_paths[graph->texture_count]));
          graph->texture_count++;
        } else {
          p = skip_value(p);
        }
        p = skip_ws(p);
        if (*p == ',')
          p++;
      }
      if (*p == ']')
        p++;
    } else {
      p = skip_value(p);
    }

    p = skip_ws(p);
    if (*p == ',')
      p++;
  }

  return graph->node_count > 0;
}

/* -------------------------------------------------------------------------
 * Cleanup
 * ------------------------------------------------------------------------- */

void mop_mat_graph_destroy(MopMaterialGraph *graph) {
  if (!graph)
    return;
  free(graph->_compiled_glsl);
  graph->_compiled_glsl = NULL;
  graph->compiled = false;
}
