/*
 * Master of Puppets — Multithreaded Tile-Based Rasterizer
 * rasterizer_mt.h — Thread pool and tile-based rasterization interface
 *
 * Splits the framebuffer into 32x32 tiles.  Triangles are binned to
 * overlapping tiles (single-threaded), then tiles are rasterized in
 * parallel with pthreads.  Each tile writes to a disjoint framebuffer
 * region, so no locks are needed during rasterization.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_SW_RASTERIZER_MT_H
#define MOP_SW_RASTERIZER_MT_H

#include "rasterizer.h"
#include <mop/light.h>
#include <mop/types.h>

#define MOP_TILE_SIZE 32

/* -------------------------------------------------------------------------
 * Prepared triangle for tile-based rasterization
 *
 * Contains everything needed to rasterize a single triangle, so the
 * tiled path can feed these directly to the existing rasterizer.
 * ------------------------------------------------------------------------- */

typedef struct MopSwPreparedTri {
  MopSwClipVertex vertices[3];
  uint32_t object_id;
  bool wireframe;
  bool depth_test;
  bool cull_back;
  MopVec3 light_dir;
  float ambient;
  float opacity;
  bool smooth_shading;
  MopBlendMode blend_mode;

  /* Multi-light: NULL = use legacy light_dir + ambient */
  const MopLight *lights;
  uint32_t light_count;
  MopVec3 cam_eye;
} MopSwPreparedTri;

/* -------------------------------------------------------------------------
 * Thread pool (create once, reuse across frames)
 * ------------------------------------------------------------------------- */

typedef struct MopSwThreadPool MopSwThreadPool;

MopSwThreadPool *mop_sw_threadpool_create(int num_threads);
void mop_sw_threadpool_destroy(MopSwThreadPool *pool);

/* -------------------------------------------------------------------------
 * Tile-based rasterization
 *
 * 1. Bin phase: assign triangles to tiles (single-threaded)
 * 2. Rasterize phase: process tiles in parallel (multi-threaded)
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_tiled(MopSwThreadPool *pool,
                            const MopSwPreparedTri *triangles,
                            uint32_t triangle_count, MopSwFramebuffer *fb);

#endif /* MOP_SW_RASTERIZER_MT_H */
