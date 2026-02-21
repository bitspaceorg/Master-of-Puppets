/*
 * Master of Puppets — mop_convert CLI tool
 * Converts a Wavefront .obj file to the .mop binary mesh format.
 *
 * Usage:  mop_convert input.obj output.mop
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/loader.h>
#include <mop/types.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Binary header — must match mop_loader.c exactly
 * ------------------------------------------------------------------------- */

#define MOP_BINARY_MAGIC 0x4D4F5001u
#define MOP_BINARY_VERSION 1u
#define MOP_HEADER_SIZE 128

typedef struct MopBinaryHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t flags;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t submesh_count;
  uint32_t vertex_offset;
  uint32_t index_offset;
  float bbox_min[3];
  float bbox_max[3];
  uint8_t _reserved[128 - 56];
} MopBinaryHeader;

int main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s input.obj output.mop\n", argv[0]);
    return 1;
  }

  const char *input = argv[1];
  const char *output = argv[2];

  /* Load OBJ */
  MopObjMesh obj;
  if (!mop_obj_load(input, &obj)) {
    fprintf(stderr, "Failed to load OBJ: %s\n", input);
    return 1;
  }

  printf("Loaded %s: %u vertices, %u indices (%u triangles)\n", input,
         obj.vertex_count, obj.index_count, obj.index_count / 3);

  /* Build header */
  MopBinaryHeader hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = MOP_BINARY_MAGIC;
  hdr.version = MOP_BINARY_VERSION;
  hdr.flags = 0;
  hdr.vertex_count = obj.vertex_count;
  hdr.index_count = obj.index_count;
  hdr.submesh_count = 1;
  hdr.vertex_offset = MOP_HEADER_SIZE;
  hdr.index_offset = (uint32_t)(MOP_HEADER_SIZE +
                                (size_t)obj.vertex_count * sizeof(MopVertex));
  hdr.bbox_min[0] = obj.bbox_min.x;
  hdr.bbox_min[1] = obj.bbox_min.y;
  hdr.bbox_min[2] = obj.bbox_min.z;
  hdr.bbox_max[0] = obj.bbox_max.x;
  hdr.bbox_max[1] = obj.bbox_max.y;
  hdr.bbox_max[2] = obj.bbox_max.z;

  /* Write binary file */
  FILE *fp = fopen(output, "wb");
  if (!fp) {
    fprintf(stderr, "Failed to open output file: %s\n", output);
    mop_obj_free(&obj);
    return 1;
  }

  fwrite(&hdr, sizeof(hdr), 1, fp);
  fwrite(obj.vertices, sizeof(MopVertex), obj.vertex_count, fp);
  fwrite(obj.indices, sizeof(uint32_t), obj.index_count, fp);

  fclose(fp);
  mop_obj_free(&obj);

  printf(
      "Written %s (%u bytes header + %u bytes vertices + %u bytes indices)\n",
      output, MOP_HEADER_SIZE, (uint32_t)(obj.vertex_count * sizeof(MopVertex)),
      (uint32_t)(obj.index_count * sizeof(uint32_t)));

  return 0;
}
