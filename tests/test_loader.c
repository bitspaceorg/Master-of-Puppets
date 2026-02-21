/*
 * Master of Puppets — Loader Tests
 * test_loader.c — OBJ loading, vertex/index counts, AABB, missing file
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include "test_harness.h"

static void test_load_cube(void) {
    TEST_BEGIN("loader_load_cube_obj");
    MopObjMesh mesh;
    bool ok = mop_obj_load("tests/fixtures/cube.obj", &mesh);
    TEST_ASSERT(ok);
    /* 12 faces (triangles) × 3 verts = 36 vertices (OBJ duplicates per-face) */
    TEST_ASSERT(mesh.vertex_count > 0);
    TEST_ASSERT(mesh.index_count == 36);
    TEST_ASSERT(mesh.index_count % 3 == 0);
    mop_obj_free(&mesh);
    TEST_END();
}

static void test_cube_aabb(void) {
    TEST_BEGIN("loader_cube_aabb");
    MopObjMesh mesh;
    bool ok = mop_obj_load("tests/fixtures/cube.obj", &mesh);
    TEST_ASSERT(ok);
    /* After normalization the cube should fit within ~[-1,1] */
    TEST_ASSERT(mesh.bbox_min.x >= -1.1f);
    TEST_ASSERT(mesh.bbox_max.x <=  1.1f);
    TEST_ASSERT(mesh.bbox_min.y >= -1.1f);
    TEST_ASSERT(mesh.bbox_max.y <=  1.1f);
    TEST_ASSERT(mesh.bbox_min.z >= -1.1f);
    TEST_ASSERT(mesh.bbox_max.z <=  1.1f);
    mop_obj_free(&mesh);
    TEST_END();
}

static void test_cube_normals_nonzero(void) {
    TEST_BEGIN("loader_cube_normals_nonzero");
    MopObjMesh mesh;
    bool ok = mop_obj_load("tests/fixtures/cube.obj", &mesh);
    TEST_ASSERT(ok);
    for (uint32_t i = 0; i < mesh.vertex_count; i++) {
        float len = mop_vec3_length(mesh.vertices[i].normal);
        TEST_ASSERT(len > 0.5f);
    }
    mop_obj_free(&mesh);
    TEST_END();
}

static void test_missing_file(void) {
    TEST_BEGIN("loader_missing_file_returns_false");
    MopObjMesh mesh;
    bool ok = mop_obj_load("tests/fixtures/nonexistent.obj", &mesh);
    TEST_ASSERT(!ok);
    TEST_ASSERT(mesh.vertices == NULL);
    TEST_ASSERT(mesh.indices == NULL);
    TEST_END();
}

static void test_free_zeroes_mesh(void) {
    TEST_BEGIN("loader_free_zeroes_mesh");
    MopObjMesh mesh;
    mop_obj_load("tests/fixtures/cube.obj", &mesh);
    mop_obj_free(&mesh);
    TEST_ASSERT(mesh.vertices == NULL);
    TEST_ASSERT(mesh.indices == NULL);
    TEST_ASSERT(mesh.vertex_count == 0);
    TEST_ASSERT(mesh.index_count == 0);
    TEST_END();
}

/* -------------------------------------------------------------------------
 * Factory (mop_load / mop_load_free)
 * ------------------------------------------------------------------------- */

static void test_factory_obj(void) {
    TEST_BEGIN("factory_load_obj");
    MopLoadedMesh mesh;
    bool ok = mop_load("tests/fixtures/cube.obj", &mesh);
    TEST_ASSERT(ok);
    TEST_ASSERT(mesh.vertex_count > 0);
    TEST_ASSERT(mesh.index_count == 36);
    TEST_ASSERT(mesh._format == MOP_FORMAT_OBJ);
    mop_load_free(&mesh);
    TEST_ASSERT(mesh.vertices == NULL);
    TEST_END();
}

static void test_factory_unknown_ext(void) {
    TEST_BEGIN("factory_unknown_extension");
    MopLoadedMesh mesh;
    bool ok = mop_load("tests/fixtures/cube.fbx", &mesh);
    TEST_ASSERT(!ok);
    TEST_ASSERT(mesh.vertices == NULL);
    TEST_END();
}

int main(void) {
    TEST_SUITE_BEGIN("loader");

    TEST_RUN(test_load_cube);
    TEST_RUN(test_cube_aabb);
    TEST_RUN(test_cube_normals_nonzero);
    TEST_RUN(test_missing_file);
    TEST_RUN(test_free_zeroes_mesh);
    TEST_RUN(test_factory_obj);
    TEST_RUN(test_factory_unknown_ext);

    TEST_REPORT();
    TEST_EXIT();
}
