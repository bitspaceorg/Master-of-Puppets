/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * loader.c — Unified loader factory (dispatches by file extension)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/loader.h>
#include <mop/log.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

static const char *get_extension(const char *path) {
  const char *dot = strrchr(path, '.');
  return dot ? dot : "";
}

bool mop_load(const char *path, MopLoadedMesh *out) {
  memset(out, 0, sizeof(*out));

  if (!path || !*path) {
    MOP_ERROR("mop_load: NULL or empty path");
    return false;
  }

  const char *ext = get_extension(path);

  if (strcmp(ext, ".obj") == 0) {
    MopObjMesh obj;
    if (!mop_obj_load(path, &obj))
      return false;
    out->vertices = obj.vertices;
    out->vertex_count = obj.vertex_count;
    out->indices = obj.indices;
    out->index_count = obj.index_count;
    out->bbox_min = obj.bbox_min;
    out->bbox_max = obj.bbox_max;
    out->tangents = obj.tangents;
    out->_format = MOP_FORMAT_OBJ;
    out->_mmapped = false;
    return true;
  }

  if (strcmp(ext, ".mop") == 0) {
    MopBinaryMesh bin;
    if (!mop_binary_load(path, &bin))
      return false;
    out->vertices = bin.vertices;
    out->vertex_count = bin.vertex_count;
    out->indices = bin.indices;
    out->index_count = bin.index_count;
    out->bbox_min = bin.bbox_min;
    out->bbox_max = bin.bbox_max;
    out->tangents = NULL;
    out->_format = MOP_FORMAT_MOP_BINARY;
    out->_mmapped = bin.is_mmapped;
    out->_mmap_base = bin._mmap_base;
    out->_mmap_size = bin._mmap_size;
    return true;
  }

  MOP_ERROR("mop_load: unsupported file extension '%s'", ext);
  return false;
}

void mop_load_free(MopLoadedMesh *mesh) {
  if (!mesh)
    return;

  if (mesh->_mmapped) {
#if defined(__unix__) || defined(__APPLE__)
    if (mesh->_mmap_base)
      munmap(mesh->_mmap_base, mesh->_mmap_size);
#endif
  } else {
    free(mesh->vertices);
    free(mesh->indices);
    free(mesh->tangents);
  }

  memset(mesh, 0, sizeof(*mesh));
}
