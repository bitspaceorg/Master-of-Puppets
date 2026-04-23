/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * texture_pipeline.c — Texture creation, loading, caching, and mip generation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"

#include <mop/core/texture_pipeline.h>
#include <mop/util/log.h>
#include <stdlib.h>
#include <string.h>

/* stb_image for file loading (implementation in src/util/stb_impl.c) */
#include "stb_image.h"

/* -------------------------------------------------------------------------
 * FNV-1a 64-bit hash
 * ------------------------------------------------------------------------- */

static uint64_t fnv1a_hash(const uint8_t *data, size_t len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

/* -------------------------------------------------------------------------
 * Box-filter mipmap generation (RGBA8)
 *
 * Generates the full mip chain from mip 0 data.  Returns a newly allocated
 * buffer containing all mip levels packed sequentially, or NULL on failure.
 * out_total_size receives the total size of the returned buffer.
 * out_levels receives the number of mip levels (including mip 0).
 * ------------------------------------------------------------------------- */

static uint8_t *generate_mips_rgba8(const uint8_t *mip0, int w, int h,
                                    size_t *out_total_size, int *out_levels) {
  /* Public callers already guard this, but keep the check here too — gcc's
   * -Wstringop-overflow can't prove w/h > 0 when this helper is inlined,
   * and `malloc(0)` + memcpy into the result would be UB. */
  if (w <= 0 || h <= 0)
    return NULL;

  /* Count mip levels */
  int levels = 1;
  {
    int tw = w, th = h;
    while (tw > 1 || th > 1) {
      tw = tw > 1 ? tw / 2 : 1;
      th = th > 1 ? th / 2 : 1;
      levels++;
    }
  }

  /* Compute total size */
  size_t total = 0;
  {
    int tw = w, th = h;
    for (int i = 0; i < levels; i++) {
      total += (size_t)tw * (size_t)th * 4;
      tw = tw > 1 ? tw / 2 : 1;
      th = th > 1 ? th / 2 : 1;
    }
  }

  uint8_t *buf = malloc(total);
  if (!buf)
    return NULL;

  /* Copy mip 0 */
  size_t mip0_size = (size_t)w * (size_t)h * 4;
  memcpy(buf, mip0, mip0_size);

  /* Generate subsequent mips via 2x2 box filter */
  uint8_t *prev = buf;
  uint8_t *cur = buf + mip0_size;
  int pw = w, ph = h;

  for (int level = 1; level < levels; level++) {
    int cw = pw > 1 ? pw / 2 : 1;
    int ch = ph > 1 ? ph / 2 : 1;

    for (int y = 0; y < ch; y++) {
      for (int x = 0; x < cw; x++) {
        int sx = x * 2;
        int sy = y * 2;
        /* Sample 2x2 block from previous mip (clamp at edges) */
        int sx1 = sx + 1 < pw ? sx + 1 : sx;
        int sy1 = sy + 1 < ph ? sy + 1 : sy;

        const uint8_t *p00 = prev + ((size_t)sy * pw + sx) * 4;
        const uint8_t *p10 = prev + ((size_t)sy * pw + sx1) * 4;
        const uint8_t *p01 = prev + ((size_t)sy1 * pw + sx) * 4;
        const uint8_t *p11 = prev + ((size_t)sy1 * pw + sx1) * 4;

        uint8_t *dst = cur + ((size_t)y * cw + x) * 4;
        for (int c = 0; c < 4; c++) {
          dst[c] = (uint8_t)((p00[c] + p10[c] + p01[c] + p11[c] + 2) / 4);
        }
      }
    }

    prev = cur;
    cur += (size_t)cw * (size_t)ch * 4;
    pw = cw;
    ph = ch;
  }

  *out_total_size = total;
  *out_levels = levels;
  return buf;
}

/* -------------------------------------------------------------------------
 * Texture cache lookup
 * ------------------------------------------------------------------------- */

static MopTexture *cache_lookup(MopViewport *vp, const char *path) {
  for (uint32_t i = 0; i < vp->tex_cache_count; i++) {
    if (strcmp(vp->tex_cache[i].path, path) == 0) {
      vp->tex_cache_hits++;
      MopTexture *tex = vp->tex_cache[i].texture;
      tex->last_used_frame = vp->frame_counter;
      return tex;
    }
  }
  return NULL;
}

static bool cache_insert(MopViewport *vp, const char *path, MopTexture *tex) {
  if (vp->tex_cache_count >= vp->tex_cache_capacity) {
    uint32_t new_cap = vp->tex_cache_capacity ? vp->tex_cache_capacity * 2 : 16;
    struct MopTexCacheEntry *new_cache =
        realloc(vp->tex_cache, new_cap * sizeof(struct MopTexCacheEntry));
    if (!new_cache)
      return false;
    memset(new_cache + vp->tex_cache_capacity, 0,
           (new_cap - vp->tex_cache_capacity) *
               sizeof(struct MopTexCacheEntry));
    vp->tex_cache = new_cache;
    vp->tex_cache_capacity = new_cap;
  }

  struct MopTexCacheEntry *entry = &vp->tex_cache[vp->tex_cache_count++];
  snprintf(entry->path, sizeof(entry->path), "%s", path);
  entry->texture = tex;
  return true;
}

/* -------------------------------------------------------------------------
 * mop_tex_create — create texture from descriptor
 * ------------------------------------------------------------------------- */

MopTexture *mop_tex_create(MopViewport *viewport, const MopTextureDesc *desc) {
  if (!viewport || !desc)
    return NULL;
  if (desc->width <= 0 || desc->height <= 0)
    return NULL;

  const MopRhiBackend *rhi = viewport->rhi;
  MopRhiDevice *dev = viewport->device;
  if (!rhi || !dev)
    return NULL;
  MOP_VP_LOCK(viewport);

  if (!desc->data) {
    MOP_WARN("mop_tex_create: NULL data for texture");
    MOP_VP_UNLOCK(viewport);
    return NULL;
  }

  MopRhiTexture *rhi_tex = NULL;

  if (desc->format != MOP_TEX_FORMAT_RGBA8) {
    /* Try extended texture creation for compressed formats */
    if (!rhi->texture_create_ex) {
      MOP_WARN("mop_tex_create: format %d not supported by backend '%s'",
               desc->format, rhi->name);
      MOP_VP_UNLOCK(viewport);
      return NULL;
    }
    size_t ds = desc->data_size ? desc->data_size : 0;
    rhi_tex = rhi->texture_create_ex(dev, desc->width, desc->height,
                                     (int)desc->format, desc->mip_levels,
                                     desc->data, ds);
    if (!rhi_tex) {
      MOP_VP_UNLOCK(viewport);
      return NULL;
    }

    MopTexture *tex = calloc(1, sizeof(MopTexture));
    if (!tex) {
      rhi->texture_destroy(dev, rhi_tex);
      MOP_VP_UNLOCK(viewport);
      return NULL;
    }

    tex->rhi_texture = rhi_tex;
    tex->width = desc->width;
    tex->height = desc->height;
    tex->srgb = desc->srgb;
    tex->stream_state = MOP_TEX_STREAM_COMPLETE;
    tex->last_used_frame = viewport->frame_counter;

    /* Compute content hash */
    size_t hash_size = desc->data_size ? desc->data_size : 0;
    if (hash_size > 0)
      tex->content_hash = fnv1a_hash(desc->data, hash_size);

    /* Compressed textures: use provided mip_levels or 1 (no auto-gen) */
    tex->mip_levels = desc->mip_levels > 0 ? desc->mip_levels : 1;

    MOP_VP_UNLOCK(viewport);
    return tex;
  }

  /* RGBA8 path */
  rhi_tex = rhi->texture_create(dev, desc->width, desc->height, desc->data);
  if (!rhi_tex) {
    MOP_VP_UNLOCK(viewport);
    return NULL;
  }

  MopTexture *tex = calloc(1, sizeof(MopTexture));
  if (!tex) {
    rhi->texture_destroy(dev, rhi_tex);
    MOP_VP_UNLOCK(viewport);
    return NULL;
  }

  tex->rhi_texture = rhi_tex;
  tex->width = desc->width;
  tex->height = desc->height;
  tex->srgb = desc->srgb;
  tex->stream_state = MOP_TEX_STREAM_COMPLETE;
  tex->last_used_frame = viewport->frame_counter;

  /* Compute content hash */
  size_t data_size = desc->data_size
                         ? desc->data_size
                         : (size_t)desc->width * (size_t)desc->height * 4;
  tex->content_hash = fnv1a_hash(desc->data, data_size);

  /* Compute mip levels */
  if (desc->mip_levels > 0) {
    tex->mip_levels = desc->mip_levels;
  } else {
    int max_dim = desc->width > desc->height ? desc->width : desc->height;
    tex->mip_levels = 1;
    while (max_dim > 1) {
      max_dim /= 2;
      tex->mip_levels++;
    }
  }

  /* Mip generation is deferred until the RHI gains per-mip upload.
   * generate_mips_rgba8() is available for when backends support it.
   * For now, the GPU driver or backend handles mip generation if needed,
   * and we report mip_levels=1 (mip 0 only). */
  if (!desc->generate_mips)
    tex->mip_levels = 1;

  MOP_VP_UNLOCK(viewport);
  return tex;
}

/* -------------------------------------------------------------------------
 * mop_tex_load_async — load texture from file (synchronous fallback)
 *
 * True async loading would use a background thread + ring buffer.
 * For now, this loads synchronously via stb_image but provides the
 * full cache + dedup path.
 * ------------------------------------------------------------------------- */

MopTexture *mop_tex_load_async(MopViewport *viewport, const char *path) {
  if (!viewport || !path || !path[0])
    return NULL;

  MOP_VP_LOCK(viewport);
  /* Check cache first (path-based) */
  MopTexture *cached = cache_lookup(viewport, path);
  if (cached) {
    MOP_VP_UNLOCK(viewport);
    return cached;
  }

  /* Load via stb_image */
  int w, h, channels;
  uint8_t *pixels = stbi_load(path, &w, &h, &channels, 4 /* force RGBA */);
  if (!pixels) {
    MOP_WARN("mop_tex_load_async: failed to load '%s': %s", path,
             stbi_failure_reason());
    MOP_VP_UNLOCK(viewport);
    return NULL;
  }

  /* Check for content-hash dedup: same pixel data loaded from a different path
   */
  size_t pixel_size = (size_t)w * (size_t)h * 4;
  uint64_t hash = fnv1a_hash(pixels, pixel_size);
  for (uint32_t i = 0; i < viewport->tex_cache_count; i++) {
    MopTexture *t = viewport->tex_cache[i].texture;
    if (t && t->content_hash == hash && t->width == w && t->height == h) {
      /* Content match — reuse existing texture, add path alias to cache */
      stbi_image_free(pixels);
      viewport->tex_cache_hits++;
      t->last_used_frame = viewport->frame_counter;
      cache_insert(viewport, path, t);
      MOP_VP_UNLOCK(viewport);
      return t;
    }
  }

  /* Create texture descriptor */
  MopTextureDesc desc = {
      .width = w,
      .height = h,
      .format = MOP_TEX_FORMAT_RGBA8,
      .data = pixels,
      .data_size = (uint32_t)pixel_size,
      .generate_mips = true,
      .srgb = true, /* assume sRGB for file-loaded textures */
  };

  MopTexture *tex = mop_tex_create(viewport, &desc);
  stbi_image_free(pixels);

  if (!tex) {
    MOP_VP_UNLOCK(viewport);
    return NULL;
  }

  /* Store path and insert into cache */
  snprintf(tex->path, sizeof(tex->path), "%s", path);
  if (!cache_insert(viewport, path, tex)) {
    MOP_WARN("mop_tex_load_async: cache insert failed for '%s' (OOM)", path);
    /* Texture is still usable, just won't be cached for dedup */
  }

  MOP_VP_UNLOCK(viewport);
  return tex;
}

/* -------------------------------------------------------------------------
 * mop_tex_get_stream_state
 * ------------------------------------------------------------------------- */

MopTexStreamState mop_tex_get_stream_state(const MopTexture *texture) {
  if (!texture)
    return MOP_TEX_STREAM_ERROR;
  return texture->stream_state;
}

/* -------------------------------------------------------------------------
 * mop_tex_get_hash
 * ------------------------------------------------------------------------- */

uint64_t mop_tex_get_hash(const MopTexture *texture) {
  if (!texture)
    return 0;
  return texture->content_hash;
}

/* -------------------------------------------------------------------------
 * mop_tex_cache_stats
 * ------------------------------------------------------------------------- */

MopTexCacheStats mop_tex_cache_stats(const MopViewport *viewport) {
  MopTexCacheStats stats;
  memset(&stats, 0, sizeof(stats));
  if (!viewport)
    return stats;

  stats.total_textures = viewport->tex_cache_count;
  stats.cache_hits = viewport->tex_cache_hits;

  /* Count unique textures by hash */
  uint32_t unique = 0;
  for (uint32_t i = 0; i < viewport->tex_cache_count; i++) {
    const MopTexture *t = viewport->tex_cache[i].texture;
    if (!t)
      continue;
    bool dup = false;
    for (uint32_t j = 0; j < i; j++) {
      const MopTexture *t2 = viewport->tex_cache[j].texture;
      if (t2 && t->content_hash && t->content_hash == t2->content_hash) {
        dup = true;
        break;
      }
    }
    if (!dup)
      unique++;
  }
  stats.unique_textures = unique;

  /* Estimate GPU memory (RGBA8 = 4 bytes/pixel, mip chain ~1.33x) */
  for (uint32_t i = 0; i < viewport->tex_cache_count; i++) {
    const MopTexture *t = viewport->tex_cache[i].texture;
    if (t) {
      uint64_t base = (uint64_t)t->width * (uint64_t)t->height * 4;
      stats.memory_used += base + base / 3; /* approximate mip chain */
    }
  }

  return stats;
}

/* -------------------------------------------------------------------------
 * mop_tex_cache_flush — evict textures not used recently
 * ------------------------------------------------------------------------- */

void mop_tex_cache_flush(MopViewport *viewport, uint32_t max_age_frames) {
  if (!viewport)
    return;
  MOP_VP_LOCK(viewport);

  uint32_t current_frame = viewport->frame_counter;
  uint32_t write = 0;

  for (uint32_t i = 0; i < viewport->tex_cache_count; i++) {
    MopTexture *t = viewport->tex_cache[i].texture;
    if (t && (current_frame - t->last_used_frame) > max_age_frames) {
      /* Evict — destroy the texture via RHI */
      if (viewport->rhi && viewport->device) {
        viewport->rhi->texture_destroy(viewport->device, t->rhi_texture);
      }
      free(t);
    } else {
      /* Keep */
      if (write != i)
        viewport->tex_cache[write] = viewport->tex_cache[i];
      write++;
    }
  }

  viewport->tex_cache_count = write;
  MOP_VP_UNLOCK(viewport);
}

/* -------------------------------------------------------------------------
 * mop_tex_read_rgba8 — read texture pixels into a host-provided buffer
 * ------------------------------------------------------------------------- */

bool mop_tex_read_rgba8(MopViewport *viewport, MopTexture *texture,
                        uint8_t *out_buf, size_t buf_size) {
  if (!viewport || !viewport->rhi || !viewport->device || !texture ||
      !texture->rhi_texture || !out_buf)
    return false;
  if (!viewport->rhi->texture_read_rgba8)
    return false;
  MOP_VP_LOCK(viewport);
  bool ok = viewport->rhi->texture_read_rgba8(
      viewport->device, texture->rhi_texture, out_buf, buf_size);
  MOP_VP_UNLOCK(viewport);
  return ok;
}

/* -------------------------------------------------------------------------
 * mop_tex_cache_destroy_all — called from viewport destroy
 * ------------------------------------------------------------------------- */

void mop_tex_cache_destroy_all(MopViewport *vp) {
  if (!vp)
    return;
  /* Note: we do NOT destroy the RHI textures here because they may be
   * shared with meshes that are still being cleaned up.  The viewport
   * destroy path handles RHI texture destruction separately. */
  free(vp->tex_cache);
  vp->tex_cache = NULL;
  vp->tex_cache_count = 0;
  vp->tex_cache_capacity = 0;
  vp->tex_cache_hits = 0;
}

/* -------------------------------------------------------------------------
 * mop_tex_generate_mips — standalone mip generation for external use
 * ------------------------------------------------------------------------- */

uint8_t *mop_tex_generate_mips(const uint8_t *rgba_data, int width, int height,
                               size_t *out_total_size, int *out_levels) {
  if (!rgba_data || width <= 0 || height <= 0 || !out_total_size || !out_levels)
    return NULL;
  return generate_mips_rgba8(rgba_data, width, height, out_total_size,
                             out_levels);
}
