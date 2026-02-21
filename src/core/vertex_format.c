/*
 * Master of Puppets — Vertex Format Utilities
 * vertex_format.c — Standard format construction and query helpers
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/vertex_format.h>
#include <stddef.h>

uint32_t mop_attrib_format_size(MopAttribFormat fmt) {
    switch (fmt) {
    case MOP_FORMAT_FLOAT:  return 4;
    case MOP_FORMAT_FLOAT2: return 8;
    case MOP_FORMAT_FLOAT3: return 12;
    case MOP_FORMAT_FLOAT4: return 16;
    case MOP_FORMAT_UBYTE4: return 4;
    }
    return 0;
}

MopVertexFormat mop_vertex_format_standard(void) {
    /*
     * Matches struct MopVertex:
     *   MopVec3  position;   offset  0, float3 (12 bytes)
     *   MopVec3  normal;     offset 12, float3 (12 bytes)
     *   MopColor color;      offset 24, float4 (16 bytes)
     *   float    u, v;       offset 40, float2 ( 8 bytes)
     *                        stride = 48 bytes
     */
    MopVertexFormat fmt = {
        .attrib_count = 4,
        .stride       = 48,
        .attribs = {
            [0] = { MOP_ATTRIB_POSITION,  MOP_FORMAT_FLOAT3,  0 },
            [1] = { MOP_ATTRIB_NORMAL,    MOP_FORMAT_FLOAT3, 12 },
            [2] = { MOP_ATTRIB_COLOR,     MOP_FORMAT_FLOAT4, 24 },
            [3] = { MOP_ATTRIB_TEXCOORD0, MOP_FORMAT_FLOAT2, 40 },
        }
    };
    return fmt;
}

const MopVertexAttrib *mop_vertex_format_find(const MopVertexFormat *fmt,
                                               MopAttribSemantic sem) {
    if (!fmt) return NULL;
    for (uint32_t i = 0; i < fmt->attrib_count; i++) {
        if (fmt->attribs[i].semantic == sem) {
            return &fmt->attribs[i];
        }
    }
    return NULL;
}
