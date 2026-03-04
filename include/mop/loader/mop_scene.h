/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * mop_scene.h — .mop v2 scene file format
 *
 * Multi-mesh scene format with camera, lights, materials, and transforms.
 * Supports quantized vertices (20 bytes vs 48 bytes) and mmap loading.
 *
 * File layout (all sections 8-byte aligned for mmap):
 *   [0..63]      Scene header (magic, version, section_count, flags)
 *   [64..]       Section table of contents (TOC)
 *   [aligned..]  Section data
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_LOADER_MOP_SCENE_H
#define MOP_LOADER_MOP_SCENE_H

#include <mop/types.h>

/* -------------------------------------------------------------------------
 * Magic and version
 * ------------------------------------------------------------------------- */

#define MOP_SCENE_MAGIC 0x4D4F5002u /* 'M' 'O' 'P' 0x02 */
#define MOP_SCENE_VERSION 2u

/* -------------------------------------------------------------------------
 * Section types in the table of contents
 * ------------------------------------------------------------------------- */

typedef enum MopSectionType {
  MOP_SECTION_MESH = 1,      /* vertex + index data */
  MOP_SECTION_CAMERA = 2,    /* camera parameters */
  MOP_SECTION_LIGHT = 3,     /* light definition */
  MOP_SECTION_MATERIAL = 4,  /* material properties */
  MOP_SECTION_TRANSFORM = 5, /* per-mesh transform */
} MopSectionType;

/* -------------------------------------------------------------------------
 * Vertex encoding flags
 * ------------------------------------------------------------------------- */

#define MOP_VTX_RAW 0x00       /* 48-byte MopVertex as-is */
#define MOP_VTX_QUANTIZED 0x01 /* 20-byte quantized vertex */

/* -------------------------------------------------------------------------
 * Quantized vertex: 20 bytes vs 48 bytes (2.4x smaller)
 *
 *   position: 3x uint16 relative to mesh bbox (0.01mm precision at 650m)
 *   normal:   2x int16 octahedral encoding (14-bit precision)
 *   color:    4x uint8 RGBA
 *   uv:       2x uint16 (0..65535 -> 0.0..1.0)
 * ------------------------------------------------------------------------- */

typedef struct MopQuantizedVertex {
  uint16_t pos[3];    /* quantized to mesh bbox */
  int16_t nrm[2];     /* octahedral normal encoding */
  uint8_t color[4];   /* RGBA8 */
  uint16_t uv[2];     /* quantized UV */
} MopQuantizedVertex; /* exactly 20 bytes */

/* -------------------------------------------------------------------------
 * Opaque scene file handle
 * ------------------------------------------------------------------------- */

typedef struct MopSceneFile MopSceneFile;

/* Forward declarations */
typedef struct MopViewport MopViewport;
typedef struct MopLoadedMesh MopLoadedMesh;

/* -------------------------------------------------------------------------
 * Load / save
 * ------------------------------------------------------------------------- */

/* Load a .mop v2 scene file.  Returns NULL on failure.
 * Uses mmap on POSIX for zero-copy demand-paged loading. */
MopSceneFile *mop_scene_load(const char *path);

/* Free a loaded scene file. */
void mop_scene_free(MopSceneFile *scene);

/* Write entire viewport to .mop v2 scene file.
 * Returns 0 on success, -1 on failure. */
int mop_scene_save(const MopViewport *vp, const char *path, uint32_t flags);
#define MOP_SAVE_QUANTIZE 0x01 /* use quantized vertices (default: raw) */

/* -------------------------------------------------------------------------
 * Query loaded scene
 * ------------------------------------------------------------------------- */

/* Number of mesh sections in the scene. */
uint32_t mop_scene_mesh_count(const MopSceneFile *s);

/* Get mesh data at index.  Fills out a MopLoadedMesh.
 * Returns true on success. */
bool mop_scene_get_mesh(const MopSceneFile *s, uint32_t idx,
                        MopLoadedMesh *out);

/* Get camera parameters.  Returns true if a camera section exists. */
bool mop_scene_get_camera(const MopSceneFile *s, MopVec3 *eye, MopVec3 *target,
                          MopVec3 *up, float *fov, float *near_p, float *far_p);

/* Number of light sections in the scene. */
uint32_t mop_scene_light_count(const MopSceneFile *s);

#endif /* MOP_LOADER_MOP_SCENE_H */
