/*
 * Master of Puppets — Font runtime loader
 * core/font.c — Read .mfa atlas blobs and serve glyph queries
 *
 * Mirrors the mmap pattern used by mop_loader.c.  The runtime never
 * rasterizes glyphs — it only walks the pre-baked metrics table.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/font_internal.h"
#include <mop/core/font.h>
#include <mop/util/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#define MOP_FONT_HAS_MMAP 1
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* -------------------------------------------------------------------------
 * Internal struct — opaque to public consumers
 * ------------------------------------------------------------------------- */

struct MopFont {
  /* Mapping bookkeeping */
  void *mapping;     /* base of mmap, or NULL when memory-loaded         */
  size_t mapping_sz; /* size of mmap (only valid when mapping != NULL)   */
  void *owned_copy;  /* malloc'd copy when load_memory wraps a blob;     */
                     /* mutually exclusive with mapping                  */

  /* Resolved pointers into the blob (whichever owns it) */
  const MopFontHeader *header;
  const MopFontGlyph *glyphs;
  const MopFontKern *kernings;
  const uint8_t *atlas_pixels;

  /* Cached, post-validation summary */
  MopFontMetrics metrics;
  MopFontAtlasType atlas_type;
};

/* -------------------------------------------------------------------------
 * Header validation — blob is at least header-sized.
 * Returns the header pointer on success, NULL on rejection.
 * ------------------------------------------------------------------------- */

static const MopFontHeader *validate_blob(const void *blob, size_t size,
                                          const char *origin) {
  if (size < MOP_FONT_HEADER_SIZE) {
    MOP_ERROR("mop_font: blob too small (%zu bytes) — %s", size, origin);
    return NULL;
  }

  const MopFontHeader *h = (const MopFontHeader *)blob;

  if (h->magic != MOP_FONT_MAGIC) {
    MOP_ERROR("mop_font: bad magic 0x%08X — %s", h->magic, origin);
    return NULL;
  }
  if (h->version != MOP_FONT_VERSION) {
    MOP_ERROR("mop_font: unsupported version %u — %s", h->version, origin);
    return NULL;
  }

  /* Sanity-check geometry. */
  if (h->atlas_width == 0 || h->atlas_height == 0 ||
      (h->atlas_channels != 1 && h->atlas_channels != 3)) {
    MOP_ERROR("mop_font: invalid atlas geometry %ux%u/%uch — %s",
              h->atlas_width, h->atlas_height, h->atlas_channels, origin);
    return NULL;
  }

  /* Validate that all referenced regions live inside the blob. */
  uint64_t glyph_bytes = (uint64_t)h->glyph_count * sizeof(MopFontGlyph);
  uint64_t kern_bytes = (uint64_t)h->kerning_count * sizeof(MopFontKern);
  uint64_t atlas_bytes =
      (uint64_t)h->atlas_width * h->atlas_height * h->atlas_channels;

  if (h->glyph_table_offset + glyph_bytes > size ||
      h->kerning_table_offset + kern_bytes > size ||
      h->atlas_offset + atlas_bytes > size) {
    MOP_ERROR("mop_font: table offsets exceed blob size — %s", origin);
    return NULL;
  }

  return h;
}

/* -------------------------------------------------------------------------
 * Construction — common path after the blob is validated.
 * `mapping` / `owned_copy` are mutually exclusive ownership tags;
 * pass NULL for the one that doesn't apply.
 * ------------------------------------------------------------------------- */

static MopFont *make_font(const void *blob, size_t size, void *mapping,
                          size_t mapping_sz, void *owned_copy) {
  const MopFontHeader *h =
      validate_blob(blob, size, mapping ? "<mmap>" : "<memory>");
  if (!h) {
    if (mapping)
      munmap(mapping, mapping_sz);
    free(owned_copy);
    return NULL;
  }

  MopFont *f = (MopFont *)calloc(1, sizeof(*f));
  if (!f) {
    if (mapping)
      munmap(mapping, mapping_sz);
    free(owned_copy);
    return NULL;
  }

  f->mapping = mapping;
  f->mapping_sz = mapping_sz;
  f->owned_copy = owned_copy;
  f->header = h;
  f->glyphs =
      (const MopFontGlyph *)((const uint8_t *)blob + h->glyph_table_offset);
  f->kernings =
      (const MopFontKern *)((const uint8_t *)blob + h->kerning_table_offset);
  f->atlas_pixels = (const uint8_t *)blob + h->atlas_offset;
  f->atlas_type = (MopFontAtlasType)h->atlas_type;

  f->metrics.ascent = h->ascent;
  f->metrics.descent = h->descent;
  f->metrics.line_gap = h->line_gap;
  f->metrics.line_height = h->ascent - h->descent + h->line_gap;
  f->metrics.em_size = h->em_size;
  f->metrics.px_range = h->px_range;
  f->metrics.glyph_count = h->glyph_count;
  f->metrics.kerning_count = h->kerning_count;

  return f;
}

/* -------------------------------------------------------------------------
 * Load from disk (mmap)
 * ------------------------------------------------------------------------- */

MopFont *mop_font_load(const char *path) {
  if (!path) {
    MOP_ERROR("mop_font_load: NULL path");
    return NULL;
  }

#if MOP_FONT_HAS_MMAP
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    MOP_ERROR("mop_font_load: failed to open '%s'", path);
    return NULL;
  }

  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size <= 0) {
    close(fd);
    MOP_ERROR("mop_font_load: stat failed for '%s'", path);
    return NULL;
  }

  size_t file_size = (size_t)st.st_size;
  void *mapping = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (mapping == MAP_FAILED) {
    MOP_ERROR("mop_font_load: mmap failed for '%s'", path);
    return NULL;
  }

  return make_font(mapping, file_size, mapping, file_size, NULL);
#else
  /* Non-POSIX fallback: read the whole file into a malloc'd buffer. */
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    MOP_ERROR("mop_font_load: failed to open '%s'", path);
    return NULL;
  }
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz <= 0) {
    fclose(fp);
    MOP_ERROR("mop_font_load: empty file '%s'", path);
    return NULL;
  }
  void *buf = malloc((size_t)sz);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
    fclose(fp);
    free(buf);
    MOP_ERROR("mop_font_load: short read on '%s'", path);
    return NULL;
  }
  fclose(fp);
  return make_font(buf, (size_t)sz, NULL, 0, buf);
#endif
}

/* -------------------------------------------------------------------------
 * Load from a caller-owned blob.  The caller guarantees the blob
 * outlives the returned MopFont; we do NOT copy.
 * ------------------------------------------------------------------------- */

MopFont *mop_font_load_memory(const void *data, size_t size) {
  if (!data) {
    MOP_ERROR("mop_font_load_memory: NULL data");
    return NULL;
  }
  return make_font(data, size, NULL, 0, NULL);
}

void mop_font_free(MopFont *font) {
  if (!font)
    return;
  if (font->mapping)
    munmap(font->mapping, font->mapping_sz);
  free(font->owned_copy);
  free(font);
}

/* -------------------------------------------------------------------------
 * Inspection
 * ------------------------------------------------------------------------- */

MopFontMetrics mop_font_metrics(const MopFont *font) {
  if (!font) {
    MopFontMetrics zero;
    memset(&zero, 0, sizeof(zero));
    return zero;
  }
  return font->metrics;
}

MopFontAtlasType mop_font_atlas_type(const MopFont *font) {
  return font ? font->atlas_type : MOP_FONT_ATLAS_SDF;
}

/* -------------------------------------------------------------------------
 * Glyph lookup — sorted-array binary search on codepoint.
 * Returns NULL when the codepoint is absent from the atlas.
 *
 * Exposed via font_internal.h for engine subsystems (text rasterizer).
 * ------------------------------------------------------------------------- */

const MopFontGlyph *mop_font_lookup_glyph(const MopFont *font, uint32_t cp) {
  if (!font || font->header->glyph_count == 0)
    return NULL;
  uint32_t lo = 0, hi = font->header->glyph_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    uint32_t mcp = font->glyphs[mid].codepoint;
    if (mcp == cp)
      return &font->glyphs[mid];
    if (mcp < cp)
      lo = mid + 1;
    else
      hi = mid;
  }
  return NULL;
}

/* Glyph index from pointer — used to resolve kern-pair lookups,
 * which key on indices rather than codepoints. */
static uint32_t glyph_index_internal(const MopFont *font,
                                     const MopFontGlyph *g) {
  return (uint32_t)(g - font->glyphs);
}

/* Kerning lookup — sorted by (left_idx, right_idx).  Returns 0.0f
 * for absent pairs or out-of-bounds indices. */
static float kern_by_index(const MopFont *font, uint32_t left, uint32_t right) {
  if (!font || font->header->kerning_count == 0)
    return 0.0f;
  uint64_t key = ((uint64_t)left << 16) | (uint64_t)(right & 0xFFFF);
  uint32_t lo = 0, hi = font->header->kerning_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const MopFontKern *k = &font->kernings[mid];
    uint64_t mkey = ((uint64_t)k->left << 16) | (uint64_t)k->right;
    if (mkey == key)
      return k->offset;
    if (mkey < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return 0.0f;
}

float mop_font_kerning(const MopFont *font, const MopFontGlyph *left,
                       const MopFontGlyph *right) {
  if (!font || !left || !right)
    return 0.0f;
  return kern_by_index(font, glyph_index_internal(font, left),
                       glyph_index_internal(font, right));
}

const uint8_t *mop_font_atlas_pixels(const MopFont *font, int *out_w,
                                     int *out_h, int *out_channels) {
  if (!font)
    return NULL;
  if (out_w)
    *out_w = (int)font->header->atlas_width;
  if (out_h)
    *out_h = (int)font->header->atlas_height;
  if (out_channels)
    *out_channels = (int)font->header->atlas_channels;
  return font->atlas_pixels;
}

float mop_font_px_range(const MopFont *font) {
  return font ? font->header->px_range : 0.0f;
}

float mop_font_em_size(const MopFont *font) {
  return font ? font->header->em_size : 0.0f;
}

/* -------------------------------------------------------------------------
 * UTF-8 → codepoint, advances `*pp` past the consumed bytes.
 * Returns 0xFFFD on malformed input (keeps the walker moving so a
 * bad byte never wedges the layout pass).
 * ------------------------------------------------------------------------- */

static uint32_t utf8_next(const char **pp) {
  const unsigned char *p = (const unsigned char *)*pp;
  uint32_t cp;
  if (p[0] < 0x80) {
    cp = p[0];
    *pp += 1;
  } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
    cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    *pp += 2;
  } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
             (p[2] & 0xC0) == 0x80) {
    cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
         (p[2] & 0x3F);
    *pp += 3;
  } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
             (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
    cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
         ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    *pp += 4;
  } else {
    cp = 0xFFFDu;
    *pp += 1;
  }
  return cp;
}

float mop_text_measure(const MopFont *font, const char *utf8, float px_size) {
  if (!font || !utf8 || px_size <= 0.0f)
    return 0.0f;

  float em = 0.0f;
  const MopFontGlyph *prev = NULL;

  for (const char *p = utf8; *p;) {
    uint32_t cp = utf8_next(&p);
    if (cp == '\n') {
      prev = NULL;
      continue;
    }
    const MopFontGlyph *g = mop_font_lookup_glyph(font, cp);
    if (!g) {
      /* Missing glyph still consumes space so the layout doesn't
       * collapse — fall back to the average advance approximation
       * (em_size / 2) so the cursor advances predictably. */
      em += 0.5f;
      prev = NULL;
      continue;
    }
    if (prev)
      em += mop_font_kerning(font, prev, g);
    em += g->advance;
    prev = g;
  }
  return em * px_size;
}

/* -------------------------------------------------------------------------
 * Built-in HUD font — populated by the embedded blob generated at
 * build time.  When the build excludes fonts (e.g., during early
 * bring-up), the symbols below are weak/zero and we return NULL.
 * ------------------------------------------------------------------------- */

#if defined(MOP_HAS_EMBEDDED_HUD_FONT)
extern const uint8_t mop_embedded_hud_font[];
extern const size_t mop_embedded_hud_font_size;

static MopFont *s_hud_font = NULL;

const MopFont *mop_font_hud(void) {
  if (!s_hud_font)
    s_hud_font =
        mop_font_load_memory(mop_embedded_hud_font, mop_embedded_hud_font_size);
  return s_hud_font;
}
#else
const MopFont *mop_font_hud(void) { return NULL; }
#endif
