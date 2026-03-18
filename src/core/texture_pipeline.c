/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * texture_pipeline.c — Texture streaming and compression stubs (Phase 8B)
 *
 * Provides API entry points for the texture pipeline.  Full streaming
 * and BC compression support requires backend integration (Phase 8B).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/core/texture_pipeline.h>
#include <mop/util/log.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * API stubs — will be fleshed out when integrated with Vulkan backend
 * ------------------------------------------------------------------------- */

MopTexture *mop_tex_create(MopViewport *viewport, const MopTextureDesc *desc) {
  if (!viewport || !desc)
    return NULL;
  /* Delegate to existing mop_viewport_create_texture for RGBA8 */
  if (desc->format == MOP_TEX_FORMAT_RGBA8 && desc->data) {
    /* Forward declaration avoids circular include */
    extern MopTexture *mop_viewport_create_texture(MopViewport *, int, int,
                                                   const uint8_t *);
    return mop_viewport_create_texture(viewport, desc->width, desc->height,
                                       desc->data);
  }
  MOP_WARN("mop_tex_create: format %d not yet supported", desc->format);
  return NULL;
}

MopTexture *mop_tex_load_async(MopViewport *viewport, const char *path) {
  if (!viewport || !path)
    return NULL;
  MOP_WARN("mop_tex_load_async: async loading not yet implemented ('%s')",
           path);
  return NULL;
}

MopTexStreamState mop_tex_get_stream_state(const MopTexture *texture) {
  if (!texture)
    return MOP_TEX_STREAM_ERROR;
  /* All textures created via mop_tex_create are immediately complete */
  return MOP_TEX_STREAM_COMPLETE;
}

uint64_t mop_tex_get_hash(const MopTexture *texture) {
  (void)texture;
  return 0; /* Content hashing not yet implemented */
}

MopTexCacheStats mop_tex_cache_stats(const MopViewport *viewport) {
  MopTexCacheStats stats;
  memset(&stats, 0, sizeof(stats));
  (void)viewport;
  return stats;
}

void mop_tex_cache_flush(MopViewport *viewport, uint32_t max_age_frames) {
  (void)viewport;
  (void)max_age_frames;
}
