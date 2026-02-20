/*
 * Master of Puppets — .mop Binary Mesh Loader
 * mop_loader.c — Load and free .mop binary mesh files
 *
 * File layout:
 *   [0..127]   MopBinaryHeader (128 bytes)
 *   [128..]    vertex data (vertex_count * sizeof(MopVertex))
 *   [..]       index data  (index_count  * sizeof(uint32_t))
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/loader.h>
#include <mop/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* POSIX mmap support */
#if defined(__unix__) || defined(__APPLE__)
#define MOP_HAS_MMAP 1
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* -------------------------------------------------------------------------
 * Binary header — fixed 128 bytes
 * ------------------------------------------------------------------------- */

#define MOP_BINARY_MAGIC   0x4D4F5001u   /* 'M' 'O' 'P' 0x01 */
#define MOP_BINARY_VERSION 1u
#define MOP_HEADER_SIZE    128

typedef struct MopBinaryHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t submesh_count;
    uint32_t vertex_offset;    /* byte offset from file start */
    uint32_t index_offset;     /* byte offset from file start */
    float    bbox_min[3];
    float    bbox_max[3];
    uint8_t  _reserved[128 - 56]; /* pad to 128 bytes total */
} MopBinaryHeader;

/* -------------------------------------------------------------------------
 * Load
 * ------------------------------------------------------------------------- */

bool mop_binary_load(const char *path, MopBinaryMesh *out) {
    memset(out, 0, sizeof(*out));

    if (!path) {
        MOP_ERROR("mop_binary_load: NULL path");
        return false;
    }

#if MOP_HAS_MMAP
    /* --- POSIX mmap path --- */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        MOP_ERROR("mop_binary_load: failed to open '%s'", path);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)MOP_HEADER_SIZE) {
        close(fd);
        MOP_ERROR("mop_binary_load: file too small '%s'", path);
        return false;
    }

    size_t file_size = (size_t)st.st_size;
    void *mapping = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (mapping == MAP_FAILED) {
        MOP_ERROR("mop_binary_load: mmap failed for '%s'", path);
        return false;
    }

    const MopBinaryHeader *hdr = (const MopBinaryHeader *)mapping;

    if (hdr->magic != MOP_BINARY_MAGIC) {
        munmap(mapping, file_size);
        MOP_ERROR("mop_binary_load: bad magic in '%s'", path);
        return false;
    }
    if (hdr->version != MOP_BINARY_VERSION) {
        munmap(mapping, file_size);
        MOP_ERROR("mop_binary_load: unsupported version %u in '%s'",
                  hdr->version, path);
        return false;
    }

    size_t vtx_bytes = (size_t)hdr->vertex_count * sizeof(MopVertex);
    size_t idx_bytes = (size_t)hdr->index_count  * sizeof(uint32_t);

    if (hdr->vertex_offset + vtx_bytes > file_size ||
        hdr->index_offset  + idx_bytes > file_size) {
        munmap(mapping, file_size);
        MOP_ERROR("mop_binary_load: data offsets exceed file size in '%s'", path);
        return false;
    }

    out->vertices      = (MopVertex *)((uint8_t *)mapping + hdr->vertex_offset);
    out->vertex_count  = hdr->vertex_count;
    out->indices       = (uint32_t *)((uint8_t *)mapping + hdr->index_offset);
    out->index_count   = hdr->index_count;
    out->bbox_min      = (MopVec3){ hdr->bbox_min[0], hdr->bbox_min[1], hdr->bbox_min[2] };
    out->bbox_max      = (MopVec3){ hdr->bbox_max[0], hdr->bbox_max[1], hdr->bbox_max[2] };
    out->submesh_count = hdr->submesh_count;
    out->is_mmapped    = true;
    out->_mmap_base    = mapping;
    out->_mmap_size    = file_size;
    return true;

#else
    /* --- Fallback: malloc + fread --- */
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        MOP_ERROR("mop_binary_load: failed to open '%s'", path);
        return false;
    }

    MopBinaryHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        MOP_ERROR("mop_binary_load: failed to read header from '%s'", path);
        return false;
    }

    if (hdr.magic != MOP_BINARY_MAGIC) {
        fclose(fp);
        MOP_ERROR("mop_binary_load: bad magic in '%s'", path);
        return false;
    }
    if (hdr.version != MOP_BINARY_VERSION) {
        fclose(fp);
        MOP_ERROR("mop_binary_load: unsupported version %u in '%s'",
                  hdr.version, path);
        return false;
    }

    size_t vtx_bytes = (size_t)hdr.vertex_count * sizeof(MopVertex);
    size_t idx_bytes = (size_t)hdr.index_count  * sizeof(uint32_t);

    MopVertex *verts = malloc(vtx_bytes);
    uint32_t  *idxs  = malloc(idx_bytes);
    if (!verts || !idxs) {
        free(verts); free(idxs);
        fclose(fp);
        return false;
    }

    fseek(fp, (long)hdr.vertex_offset, SEEK_SET);
    if (fread(verts, vtx_bytes, 1, fp) != 1) {
        free(verts); free(idxs);
        fclose(fp);
        return false;
    }

    fseek(fp, (long)hdr.index_offset, SEEK_SET);
    if (fread(idxs, idx_bytes, 1, fp) != 1) {
        free(verts); free(idxs);
        fclose(fp);
        return false;
    }

    fclose(fp);

    out->vertices      = verts;
    out->vertex_count  = hdr.vertex_count;
    out->indices       = idxs;
    out->index_count   = hdr.index_count;
    out->bbox_min      = (MopVec3){ hdr.bbox_min[0], hdr.bbox_min[1], hdr.bbox_min[2] };
    out->bbox_max      = (MopVec3){ hdr.bbox_max[0], hdr.bbox_max[1], hdr.bbox_max[2] };
    out->submesh_count = hdr.submesh_count;
    out->is_mmapped    = false;
    out->_mmap_base    = NULL;
    out->_mmap_size    = 0;
    return true;
#endif
}

/* -------------------------------------------------------------------------
 * Free
 * ------------------------------------------------------------------------- */

void mop_binary_free(MopBinaryMesh *mesh) {
    if (!mesh) return;

#if MOP_HAS_MMAP
    if (mesh->is_mmapped && mesh->_mmap_base) {
        munmap(mesh->_mmap_base, mesh->_mmap_size);
    }
#endif

    if (!mesh->is_mmapped) {
        free(mesh->vertices);
        free(mesh->indices);
    }

    memset(mesh, 0, sizeof(*mesh));
}
