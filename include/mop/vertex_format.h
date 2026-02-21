/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * vertex_format.h — Flexible per-vertex attribute descriptor
 *
 * MopVertexFormat describes the layout of interleaved vertex data.
 * It enables arbitrary per-vertex attributes (multiple UV sets,
 * bone weights, tangent frames, custom float channels) without
 * changing the fixed MopVertex struct used by existing code.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_VERTEX_FORMAT_H
#define MOP_VERTEX_FORMAT_H

#include "types.h"

/* -------------------------------------------------------------------------
 * Attribute semantics
 * ------------------------------------------------------------------------- */

typedef enum MopAttribSemantic {
  MOP_ATTRIB_POSITION = 0,  /* float3, required */
  MOP_ATTRIB_NORMAL = 1,    /* float3 */
  MOP_ATTRIB_COLOR = 2,     /* float4 (RGBA) */
  MOP_ATTRIB_TEXCOORD0 = 3, /* float2 */
  MOP_ATTRIB_TEXCOORD1 = 4, /* float2 */
  MOP_ATTRIB_TANGENT = 5,   /* float4 (xyz + handedness w) */
  MOP_ATTRIB_JOINTS = 6,    /* ubyte4 (bone indices) */
  MOP_ATTRIB_WEIGHTS = 7,   /* float4 (bone weights) */
  MOP_ATTRIB_CUSTOM0 = 8,   /* float4 (app-defined) */
  MOP_ATTRIB_CUSTOM1 = 9,   /* float4 */
  MOP_ATTRIB_CUSTOM2 = 10,  /* float4 */
  MOP_ATTRIB_CUSTOM3 = 11,  /* float4 */
  MOP_ATTRIB_COUNT = 12
} MopAttribSemantic;

/* -------------------------------------------------------------------------
 * Attribute data formats
 * ------------------------------------------------------------------------- */

typedef enum MopAttribFormat {
  MOP_FORMAT_FLOAT = 0,  /*  4 bytes (1 float)  */
  MOP_FORMAT_FLOAT2 = 1, /*  8 bytes (2 floats) */
  MOP_FORMAT_FLOAT3 = 2, /* 12 bytes (3 floats) */
  MOP_FORMAT_FLOAT4 = 3, /* 16 bytes (4 floats) */
  MOP_FORMAT_UBYTE4 = 4, /*  4 bytes (packed)   */
} MopAttribFormat;

/* -------------------------------------------------------------------------
 * Single vertex attribute descriptor
 * ------------------------------------------------------------------------- */

typedef struct MopVertexAttrib {
  MopAttribSemantic semantic;
  MopAttribFormat format;
  uint32_t offset; /* byte offset within one vertex */
} MopVertexAttrib;

/* -------------------------------------------------------------------------
 * Vertex format — describes the full interleaved layout
 * ------------------------------------------------------------------------- */

#define MOP_MAX_VERTEX_ATTRIBS 12

typedef struct MopVertexFormat {
  MopVertexAttrib attribs[MOP_MAX_VERTEX_ATTRIBS];
  uint32_t attrib_count;
  uint32_t stride; /* bytes per vertex */
} MopVertexFormat;

/* -------------------------------------------------------------------------
 * Utilities
 * ------------------------------------------------------------------------- */

/* Returns the format matching the standard MopVertex layout:
 *   POSITION (float3) + NORMAL (float3) + COLOR (float4) + TEXCOORD0 (float2)
 * stride = sizeof(MopVertex) = 48 bytes */
MopVertexFormat mop_vertex_format_standard(void);

/* Find an attribute by semantic.  Returns NULL if not present. */
const MopVertexAttrib *mop_vertex_format_find(const MopVertexFormat *fmt,
                                              MopAttribSemantic sem);

/* Return the byte size of a given attribute format. */
uint32_t mop_attrib_format_size(MopAttribFormat fmt);

#endif /* MOP_VERTEX_FORMAT_H */
