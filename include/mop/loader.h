/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * loader.h — Mesh file loaders (OBJ)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_LOADER_H
#define MOP_LOADER_H

#include "types.h"

/* -------------------------------------------------------------------------
 * Loaded mesh data
 *
 * Returned by mop_obj_load.  The caller must free resources with
 * mop_obj_free when done.  The vertex and index arrays can be passed
 * directly to MopMeshDesc for use with mop_viewport_add_mesh.
 * ------------------------------------------------------------------------- */

typedef struct MopObjMesh {
    MopVertex *vertices;
    uint32_t   vertex_count;
    uint32_t  *indices;
    uint32_t   index_count;
    MopVec3    bbox_min;      /* axis-aligned bounding box (post-normalization) */
    MopVec3    bbox_max;
    MopVec3   *tangents;      /* parallel array to vertices (for normal mapping) */
} MopObjMesh;

/* Load a Wavefront .obj file.  Supports v, vn, and f directives.
 * Triangulates quads automatically.  Assigns a default light-gray
 * vertex color (no material/texture support).
 *
 * Returns true on success; on failure the out struct is zeroed. */
bool mop_obj_load(const char *path, MopObjMesh *out);

/* Free memory allocated by mop_obj_load. */
void mop_obj_free(MopObjMesh *mesh);

/* -------------------------------------------------------------------------
 * .mop binary mesh format
 *
 * A compact binary format that stores MopVertex and uint32_t index data
 * with a 128-byte header.  On POSIX platforms the file is memory-mapped
 * for zero-copy loading; on other platforms a malloc+fread fallback is used.
 * ------------------------------------------------------------------------- */

typedef struct MopBinaryMesh {
    MopVertex  *vertices;
    uint32_t    vertex_count;
    uint32_t   *indices;
    uint32_t    index_count;
    MopVec3     bbox_min, bbox_max;
    uint32_t    submesh_count;
    bool        is_mmapped;
    void       *_mmap_base;
    size_t      _mmap_size;
} MopBinaryMesh;

bool mop_binary_load(const char *path, MopBinaryMesh *out);
void mop_binary_free(MopBinaryMesh *mesh);

#endif /* MOP_LOADER_H */
