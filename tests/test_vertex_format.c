/*
 * Master of Puppets — Vertex Format Tests
 * test_vertex_format.c
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"
#include <mop/mop.h>

static void test_standard_format(void) {
    TEST_BEGIN("vertex_format_standard");
    MopVertexFormat fmt = mop_vertex_format_standard();
    TEST_ASSERT(fmt.attrib_count == 4);
    TEST_ASSERT(fmt.stride == 48);
    TEST_ASSERT(fmt.stride == sizeof(MopVertex));
    TEST_END();
}

static void test_attrib_format_sizes(void) {
    TEST_BEGIN("attrib_format_sizes");
    TEST_ASSERT(mop_attrib_format_size(MOP_FORMAT_FLOAT)  == 4);
    TEST_ASSERT(mop_attrib_format_size(MOP_FORMAT_FLOAT2) == 8);
    TEST_ASSERT(mop_attrib_format_size(MOP_FORMAT_FLOAT3) == 12);
    TEST_ASSERT(mop_attrib_format_size(MOP_FORMAT_FLOAT4) == 16);
    TEST_ASSERT(mop_attrib_format_size(MOP_FORMAT_UBYTE4) == 4);
    TEST_END();
}

static void test_find_attrib(void) {
    TEST_BEGIN("vertex_format_find");
    MopVertexFormat fmt = mop_vertex_format_standard();

    const MopVertexAttrib *pos = mop_vertex_format_find(&fmt, MOP_ATTRIB_POSITION);
    TEST_ASSERT(pos != NULL);
    TEST_ASSERT(pos->offset == 0);
    TEST_ASSERT(pos->format == MOP_FORMAT_FLOAT3);

    const MopVertexAttrib *nrm = mop_vertex_format_find(&fmt, MOP_ATTRIB_NORMAL);
    TEST_ASSERT(nrm != NULL);
    TEST_ASSERT(nrm->offset == 12);

    const MopVertexAttrib *col = mop_vertex_format_find(&fmt, MOP_ATTRIB_COLOR);
    TEST_ASSERT(col != NULL);
    TEST_ASSERT(col->offset == 24);

    const MopVertexAttrib *uv = mop_vertex_format_find(&fmt, MOP_ATTRIB_TEXCOORD0);
    TEST_ASSERT(uv != NULL);
    TEST_ASSERT(uv->offset == 40);

    /* Missing attrib returns NULL */
    TEST_ASSERT(mop_vertex_format_find(&fmt, MOP_ATTRIB_TANGENT) == NULL);
    TEST_ASSERT(mop_vertex_format_find(&fmt, MOP_ATTRIB_JOINTS) == NULL);
    TEST_ASSERT(mop_vertex_format_find(&fmt, MOP_ATTRIB_CUSTOM0) == NULL);
    TEST_END();
}

static void test_find_null_format(void) {
    TEST_BEGIN("vertex_format_find_null");
    TEST_ASSERT(mop_vertex_format_find(NULL, MOP_ATTRIB_POSITION) == NULL);
    TEST_END();
}

static void test_custom_format(void) {
    TEST_BEGIN("vertex_format_custom");
    /* Build a custom format: position + UV0 + weights */
    MopVertexFormat fmt = {
        .attrib_count = 3,
        .stride = 28,  /* 12 + 8 + 16 = 36... no, let's be accurate */
    };
    fmt.attribs[0] = (MopVertexAttrib){ MOP_ATTRIB_POSITION, MOP_FORMAT_FLOAT3, 0 };
    fmt.attribs[1] = (MopVertexAttrib){ MOP_ATTRIB_TEXCOORD0, MOP_FORMAT_FLOAT2, 12 };
    fmt.attribs[2] = (MopVertexAttrib){ MOP_ATTRIB_WEIGHTS, MOP_FORMAT_FLOAT4, 20 };
    fmt.stride = 36;

    const MopVertexAttrib *w = mop_vertex_format_find(&fmt, MOP_ATTRIB_WEIGHTS);
    TEST_ASSERT(w != NULL);
    TEST_ASSERT(w->offset == 20);
    TEST_ASSERT(w->format == MOP_FORMAT_FLOAT4);
    TEST_ASSERT(mop_attrib_format_size(w->format) == 16);

    /* No normal in this format */
    TEST_ASSERT(mop_vertex_format_find(&fmt, MOP_ATTRIB_NORMAL) == NULL);
    TEST_END();
}

int main(void) {
    TEST_SUITE_BEGIN("vertex_format");

    TEST_RUN(test_standard_format);
    TEST_RUN(test_attrib_format_sizes);
    TEST_RUN(test_find_attrib);
    TEST_RUN(test_find_null_format);
    TEST_RUN(test_custom_format);

    TEST_REPORT();
    TEST_EXIT();
}
