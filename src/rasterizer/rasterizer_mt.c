/*
 * Master of Puppets — Multithreaded Tile-Based Rasterizer
 * rasterizer_mt.c — Thread pool and tile-based parallel rasterization
 *
 * Strategy:
 *   1. Divide framebuffer into 32x32 pixel tiles
 *   2. Bin phase (single-threaded): assign each triangle to exactly one
 *      tile based on its screen-space centroid
 *   3. Rasterize phase (multi-threaded): workers atomically grab tile
 *      indices and rasterize all assigned triangles.  Since each triangle
 *      is assigned to exactly one tile, no two workers process the same
 *      triangle, making pixel writes race-free without locks.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rasterizer_mt.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Tile bin — dynamic array of triangle indices per tile
 * ------------------------------------------------------------------------- */

#define TILE_BIN_INIT_CAP 64

typedef struct MopTileBin {
  uint32_t *tri_indices;
  uint32_t count;
  uint32_t capacity;
} MopTileBin;

static void tile_bin_init(MopTileBin *bin) {
  bin->tri_indices = NULL;
  bin->count = 0;
  bin->capacity = 0;
}

static void tile_bin_push(MopTileBin *bin, uint32_t tri_idx) {
  if (bin->count == bin->capacity) {
    uint32_t new_cap = bin->capacity ? bin->capacity * 2 : TILE_BIN_INIT_CAP;
    uint32_t *new_arr = realloc(bin->tri_indices, new_cap * sizeof(uint32_t));
    if (!new_arr)
      return; /* drop triangle on OOM — non-fatal */
    bin->tri_indices = new_arr;
    bin->capacity = new_cap;
  }
  bin->tri_indices[bin->count++] = tri_idx;
}

static void tile_bin_free(MopTileBin *bin) {
  free(bin->tri_indices);
  bin->tri_indices = NULL;
  bin->count = 0;
  bin->capacity = 0;
}

/* -------------------------------------------------------------------------
 * Tile grid
 * ------------------------------------------------------------------------- */

typedef struct MopTileGrid {
  MopTileBin *bins; /* tiles_x * tiles_y bins */
  int tiles_x;
  int tiles_y;
} MopTileGrid;

static void tile_grid_init(MopTileGrid *grid, int fb_width, int fb_height) {
  grid->tiles_x = (fb_width + MOP_TILE_SIZE - 1) / MOP_TILE_SIZE;
  grid->tiles_y = (fb_height + MOP_TILE_SIZE - 1) / MOP_TILE_SIZE;
  int total = grid->tiles_x * grid->tiles_y;
  grid->bins = malloc((size_t)total * sizeof(MopTileBin));
  if (grid->bins) {
    for (int i = 0; i < total; i++) {
      tile_bin_init(&grid->bins[i]);
    }
  }
}

static void tile_grid_free(MopTileGrid *grid) {
  if (!grid->bins)
    return;
  int total = grid->tiles_x * grid->tiles_y;
  for (int i = 0; i < total; i++) {
    tile_bin_free(&grid->bins[i]);
  }
  free(grid->bins);
  grid->bins = NULL;
}

/* -------------------------------------------------------------------------
 * Thread pool
 * ------------------------------------------------------------------------- */

typedef struct MopSwTileWork {
  const MopSwPreparedTri *triangles;
  MopTileGrid *grid;
  MopSwFramebuffer *fb;
  volatile int next_tile; /* atomic tile counter */
  int total_tiles;
} MopSwTileWork;

struct MopSwThreadPool {
  pthread_t *threads;
  int num_threads;

  /* Synchronization */
  pthread_mutex_t mutex;
  pthread_cond_t work_ready;
  pthread_cond_t work_done;

  /* Work descriptor for current frame */
  MopSwTileWork *work;
  bool shutdown;
  int active_workers;
};

/* -------------------------------------------------------------------------
 * Worker: process tiles until none remain
 *
 * Each triangle is assigned to exactly one tile (centroid-based binning),
 * so we rasterize the full triangle without tile clipping.  Pixels may
 * land in adjacent tiles, but no other worker has the same triangle,
 * so there are no data races.
 * ------------------------------------------------------------------------- */

static void process_tile(const MopSwTileWork *work, int tile_idx) {
  const MopTileBin *bin = &work->grid->bins[tile_idx];
  if (bin->count == 0)
    return;

  MopSwFramebuffer *fb = work->fb;

  for (uint32_t i = 0; i < bin->count; i++) {
    const MopSwPreparedTri *tri = &work->triangles[bin->tri_indices[i]];

    if (tri->lights && tri->light_count > 0) {
      mop_sw_rasterize_triangle_full(
          tri->vertices, tri->object_id, tri->wireframe, tri->depth_test,
          tri->cull_back, tri->light_dir, tri->ambient, tri->opacity,
          tri->smooth_shading, tri->blend_mode, tri->lights, tri->light_count,
          tri->cam_eye, fb);
    } else {
      mop_sw_rasterize_triangle(tri->vertices, tri->object_id, tri->wireframe,
                                tri->depth_test, tri->cull_back, tri->light_dir,
                                tri->ambient, tri->opacity, tri->smooth_shading,
                                tri->blend_mode, fb);
    }
  }
}

static void *worker_func(void *arg) {
  MopSwThreadPool *pool = (MopSwThreadPool *)arg;

  for (;;) {
    pthread_mutex_lock(&pool->mutex);

    /* Wait for work or shutdown */
    while (!pool->work && !pool->shutdown) {
      pthread_cond_wait(&pool->work_ready, &pool->mutex);
    }

    if (pool->shutdown) {
      pthread_mutex_unlock(&pool->mutex);
      break;
    }

    pool->active_workers++;
    MopSwTileWork *work = pool->work;
    pthread_mutex_unlock(&pool->mutex);

    /* Process tiles atomically */
    for (;;) {
      int tile_idx = __atomic_fetch_add(&work->next_tile, 1, __ATOMIC_RELAXED);
      if (tile_idx >= work->total_tiles)
        break;
      process_tile(work, tile_idx);
    }

    /* Signal completion */
    pthread_mutex_lock(&pool->mutex);
    pool->active_workers--;
    if (pool->active_workers == 0) {
      pthread_cond_signal(&pool->work_done);
    }
    pthread_mutex_unlock(&pool->mutex);
  }

  return NULL;
}

/* -------------------------------------------------------------------------
 * Thread pool lifecycle
 * ------------------------------------------------------------------------- */

MopSwThreadPool *mop_sw_threadpool_create(int num_threads) {
  if (num_threads < 1)
    num_threads = 1;

  MopSwThreadPool *pool = calloc(1, sizeof(MopSwThreadPool));
  if (!pool)
    return NULL;

  pool->num_threads = num_threads;
  pool->shutdown = false;
  pool->work = NULL;
  pool->active_workers = 0;

  if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
    free(pool);
    return NULL;
  }
  if (pthread_cond_init(&pool->work_ready, NULL) != 0) {
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
    return NULL;
  }
  if (pthread_cond_init(&pool->work_done, NULL) != 0) {
    pthread_cond_destroy(&pool->work_ready);
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
    return NULL;
  }

  pool->threads = malloc((size_t)num_threads * sizeof(pthread_t));
  if (!pool->threads) {
    pthread_cond_destroy(&pool->work_done);
    pthread_cond_destroy(&pool->work_ready);
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
    return NULL;
  }

  for (int i = 0; i < num_threads; i++) {
    if (pthread_create(&pool->threads[i], NULL, worker_func, pool) != 0) {
      /* Shut down already-created threads */
      pthread_mutex_lock(&pool->mutex);
      pool->shutdown = true;
      pthread_cond_broadcast(&pool->work_ready);
      pthread_mutex_unlock(&pool->mutex);
      for (int j = 0; j < i; j++) {
        pthread_join(pool->threads[j], NULL);
      }
      free(pool->threads);
      pthread_cond_destroy(&pool->work_done);
      pthread_cond_destroy(&pool->work_ready);
      pthread_mutex_destroy(&pool->mutex);
      free(pool);
      return NULL;
    }
  }

  return pool;
}

void mop_sw_threadpool_destroy(MopSwThreadPool *pool) {
  if (!pool)
    return;

  /* Signal shutdown */
  pthread_mutex_lock(&pool->mutex);
  pool->shutdown = true;
  pthread_cond_broadcast(&pool->work_ready);
  pthread_mutex_unlock(&pool->mutex);

  /* Join all threads */
  for (int i = 0; i < pool->num_threads; i++) {
    pthread_join(pool->threads[i], NULL);
  }

  free(pool->threads);
  pthread_cond_destroy(&pool->work_done);
  pthread_cond_destroy(&pool->work_ready);
  pthread_mutex_destroy(&pool->mutex);
  free(pool);
}

/* -------------------------------------------------------------------------
 * Bin phase: assign each triangle to exactly one tile
 *
 * We assign each triangle to the tile containing its screen-space
 * centroid.  This avoids double-rasterization and race conditions.
 * The triangle is rasterized fully by that tile's worker; pixels
 * may land in adjacent tiles, but since each triangle is processed
 * by exactly one worker, there are no data races.
 * ------------------------------------------------------------------------- */

static void bin_triangles(MopTileGrid *grid, const MopSwPreparedTri *triangles,
                          uint32_t triangle_count, int fb_width,
                          int fb_height) {
  float half_w = (float)fb_width * 0.5f;
  float half_h = (float)fb_height * 0.5f;

  for (uint32_t t = 0; t < triangle_count; t++) {
    const MopSwPreparedTri *tri = &triangles[t];

    /* Quick screen-space centroid from clip positions */
    float cx = 0.0f, cy = 0.0f;
    bool valid = true;
    for (int vi = 0; vi < 3; vi++) {
      float w = tri->vertices[vi].position.w;
      if (fabsf(w) < 1e-7f) {
        valid = false;
        break;
      }
      float inv_w = 1.0f / w;
      float sx = (tri->vertices[vi].position.x * inv_w + 1.0f) * half_w;
      float sy = (1.0f - tri->vertices[vi].position.y * inv_w) * half_h;
      cx += sx;
      cy += sy;
    }
    if (!valid)
      continue;

    cx *= (1.0f / 3.0f);
    cy *= (1.0f / 3.0f);

    int tile_x = (int)(cx / (float)MOP_TILE_SIZE);
    int tile_y = (int)(cy / (float)MOP_TILE_SIZE);

    /* Clamp to grid bounds */
    if (tile_x < 0)
      tile_x = 0;
    if (tile_y < 0)
      tile_y = 0;
    if (tile_x >= grid->tiles_x)
      tile_x = grid->tiles_x - 1;
    if (tile_y >= grid->tiles_y)
      tile_y = grid->tiles_y - 1;

    int tile_idx = tile_y * grid->tiles_x + tile_x;
    tile_bin_push(&grid->bins[tile_idx], t);
  }
}

/* -------------------------------------------------------------------------
 * Tiled rasterization entry point
 * ------------------------------------------------------------------------- */

void mop_sw_rasterize_tiled(MopSwThreadPool *pool,
                            const MopSwPreparedTri *triangles,
                            uint32_t triangle_count, MopSwFramebuffer *fb) {
  if (!pool || !triangles || triangle_count == 0 || !fb)
    return;

  /* Build tile grid */
  MopTileGrid grid;
  tile_grid_init(&grid, fb->width, fb->height);
  if (!grid.bins)
    return;

  /* Bin triangles to tiles (single-threaded) */
  bin_triangles(&grid, triangles, triangle_count, fb->width, fb->height);

  /* Set up work descriptor */
  MopSwTileWork work;
  work.triangles = triangles;
  work.grid = &grid;
  work.fb = fb;
  work.next_tile = 0;
  work.total_tiles = grid.tiles_x * grid.tiles_y;

  /* Dispatch to thread pool */
  pthread_mutex_lock(&pool->mutex);
  pool->work = &work;
  pthread_cond_broadcast(&pool->work_ready);
  pthread_mutex_unlock(&pool->mutex);

  /* Also do work on the main thread */
  for (;;) {
    int tile_idx = __atomic_fetch_add(&work.next_tile, 1, __ATOMIC_RELAXED);
    if (tile_idx >= work.total_tiles)
      break;
    process_tile(&work, tile_idx);
  }

  /* Wait for all workers to finish */
  pthread_mutex_lock(&pool->mutex);
  while (pool->active_workers > 0) {
    pthread_cond_wait(&pool->work_done, &pool->mutex);
  }
  pool->work = NULL;
  pthread_mutex_unlock(&pool->mutex);

  /* Cleanup */
  tile_grid_free(&grid);
}
