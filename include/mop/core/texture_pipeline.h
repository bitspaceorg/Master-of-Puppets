/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * texture_pipeline.h — Texture streaming and compression (Phase 8B)
 *
 * The texture pipeline provides:
 *   - Progressive mip-by-mip streaming (load low mips first, refine)
 *   - BC-compressed texture format support (BC1/3/5/7)
 *   - Content-hash deduplication (avoid loading the same image twice)
 *
 * Textures are loaded asynchronously.  A 1x1 fallback is shown until
 * the first mip arrives.  Higher mips stream in over subsequent frames.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_TEXTURE_PIPELINE_H
#define MOP_CORE_TEXTURE_PIPELINE_H

#include <mop/types.h>
#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
typedef struct MopViewport MopViewport;
typedef struct MopTexture MopTexture;

/* -------------------------------------------------------------------------
 * Compressed texture formats
 * ------------------------------------------------------------------------- */

typedef enum MopTexFormat {
  MOP_TEX_FORMAT_RGBA8 = 0, /* uncompressed RGBA, 4 bytes/pixel */
  MOP_TEX_FORMAT_BC1,       /* RGB, 4:1 compression (0.5 byte/pixel) */
  MOP_TEX_FORMAT_BC3,       /* RGBA, 4:1 compression (1 byte/pixel) */
  MOP_TEX_FORMAT_BC5,       /* RG (normal maps), 2:1 compression */
  MOP_TEX_FORMAT_BC7,       /* RGBA high quality, 4:1 compression */
  MOP_TEX_FORMAT_COUNT
} MopTexFormat;

/* -------------------------------------------------------------------------
 * Texture descriptor (extended)
 * ------------------------------------------------------------------------- */

typedef struct MopTextureDesc {
  int width;
  int height;
  int mip_levels; /* 0 = auto-compute from dimensions */
  MopTexFormat format;
  const uint8_t *data; /* pixel data for mip 0 (NULL for streaming) */
  uint32_t data_size;  /* size in bytes */
  bool generate_mips;  /* auto-generate mip chain from mip 0 data */
  bool srgb;           /* interpret as sRGB (true for albedo/emissive) */
} MopTextureDesc;

/* -------------------------------------------------------------------------
 * Streaming state
 * ------------------------------------------------------------------------- */

typedef enum MopTexStreamState {
  MOP_TEX_STREAM_PENDING = 0, /* not yet loaded */
  MOP_TEX_STREAM_PARTIAL,     /* some mips loaded */
  MOP_TEX_STREAM_COMPLETE,    /* all mips loaded */
  MOP_TEX_STREAM_ERROR,       /* failed to load */
} MopTexStreamState;

/* -------------------------------------------------------------------------
 * Texture cache statistics
 * ------------------------------------------------------------------------- */

typedef struct MopTexCacheStats {
  uint32_t total_textures;  /* textures in cache */
  uint32_t unique_textures; /* deduplicated count */
  uint64_t memory_used;     /* GPU memory for textures (bytes) */
  uint32_t pending_loads;   /* textures still streaming */
  uint32_t cache_hits;      /* loads avoided via dedup */
} MopTexCacheStats;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Create a texture from an extended descriptor.
 * If desc->data is NULL and a path was provided via mop_tex_load_async,
 * the texture starts with a 1x1 fallback and streams in over time. */
MopTexture *mop_tex_create(MopViewport *viewport, const MopTextureDesc *desc);

/* Begin async loading of a texture from a file path.
 * Returns a texture handle immediately (shows fallback until loaded).
 * Supported formats: PNG, JPEG, HDR, EXR, KTX2 (if BC data). */
MopTexture *mop_tex_load_async(MopViewport *viewport, const char *path);

/* Query the streaming state of a texture. */
MopTexStreamState mop_tex_get_stream_state(const MopTexture *texture);

/* Get the content hash of a texture (for deduplication).
 * Returns 0 if not yet computed. */
uint64_t mop_tex_get_hash(const MopTexture *texture);

/* Get texture cache statistics for a viewport. */
MopTexCacheStats mop_tex_cache_stats(const MopViewport *viewport);

/* Flush textures that haven't been referenced in `max_age_frames` frames. */
void mop_tex_cache_flush(MopViewport *viewport, uint32_t max_age_frames);

/* Copy a texture's RGBA8 pixels into a caller-provided buffer.  `buf_size`
 * must be >= width * height * 4.  Returns true on success, false on
 * NULL args, size mismatch, or a backend without readback support.
 *
 * Cheap on CPU backend. GPU backends need to be wired with a staging
 * buffer and currently return false — hosts using GPU backends should
 * keep pixels on the GPU via `mop_viewport_present_to_texture` and
 * consume the texture directly in their render pipeline. */
bool mop_tex_read_rgba8(MopViewport *viewport, MopTexture *texture,
                        uint8_t *out_buf, size_t buf_size);

#endif /* MOP_CORE_TEXTURE_PIPELINE_H */
