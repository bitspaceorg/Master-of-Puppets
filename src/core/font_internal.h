/*
 * Master of Puppets — .mfa Binary Font Atlas — internal layout
 *
 * This header defines the on-disk format shared between
 * tools/mop_font_bake (writer) and src/core/font.c (reader).
 * Public consumers should include <mop/core/font.h> instead.
 *
 * File layout:
 *   [0 .. 127]                       MopFontHeader (128 bytes)
 *   [glyph_table_offset ..]          MopFontGlyph * glyph_count
 *   [kerning_table_offset ..]        MopFontKern  * kerning_count
 *   [atlas_offset ..]                raw pixels (1 or 3 bytes per texel)
 *
 * All offsets are absolute byte offsets from the start of the file.
 * All multi-byte integers are little-endian.
 *
 * The glyph table is sorted by codepoint, allowing O(log N) lookup.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_FONT_INTERNAL_H
#define MOP_CORE_FONT_INTERNAL_H

#include <stdint.h>

/* ASCII 'M' 'F' 'A' 0x01 — distinct from MOP_BINARY_MAGIC. */
#define MOP_FONT_MAGIC 0x4D464101u
#define MOP_FONT_VERSION 1u
#define MOP_FONT_HEADER_SIZE 128

/* Atlas type values match the public MopFontAtlasType enum. */
#define MOP_FONT_TYPE_SDF 0u
#define MOP_FONT_TYPE_MSDF 1u
#define MOP_FONT_TYPE_BITMAP 2u

typedef struct MopFontHeader {
  uint32_t magic;          /* MOP_FONT_MAGIC                              */
  uint32_t version;        /* MOP_FONT_VERSION                            */
  uint32_t atlas_type;     /* SDF / MSDF / BITMAP                         */
  uint32_t atlas_channels; /* 1 (SDF, BITMAP) or 3 (MSDF)                 */
  uint32_t atlas_width;    /* pixels                                      */
  uint32_t atlas_height;   /* pixels                                      */
  float px_range;          /* SDF/MSDF range in atlas pixels              */
  float em_size;           /* source em size used at bake                 */
  float ascent;            /* em units                                    */
  float descent;           /* em units (negative)                         */
  float line_gap;          /* em units                                    */
  uint32_t glyph_count;
  uint32_t kerning_count;
  uint64_t glyph_table_offset;
  uint64_t kerning_table_offset;
  uint64_t atlas_offset;
  uint8_t _reserved[128 - 80]; /* pad to MOP_FONT_HEADER_SIZE */
} MopFontHeader;

/* Static size assertion — file format must stay locked at 128 bytes. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(MopFontHeader) == MOP_FONT_HEADER_SIZE,
               "MopFontHeader must be exactly 128 bytes");
#endif

/* Per-glyph entry — 32 bytes.
 *
 * atlas_uv_*  : atlas pixel coordinates of the glyph's bounding box.
 * plane_*     : glyph quad relative to the pen position, em units.
 *               (plane_min.x, plane_min.y) is the bottom-left corner;
 *               y increases upward in baseline-relative space.
 * advance     : pen advance in em units after drawing this glyph.
 */
typedef struct MopFontGlyph {
  uint32_t codepoint;
  uint16_t atlas_uv_min_x; /* atlas pixels */
  uint16_t atlas_uv_min_y;
  uint16_t atlas_uv_max_x;
  uint16_t atlas_uv_max_y;
  float plane_min_x; /* em units */
  float plane_min_y;
  float plane_max_x;
  float plane_max_y;
  float advance; /* em units */
} MopFontGlyph;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(MopFontGlyph) == 32,
               "MopFontGlyph must be exactly 32 bytes");
#endif

/* Kerning pair — 8 bytes.  Sorted by (left, right) for binary search. */
typedef struct MopFontKern {
  uint16_t left;  /* glyph index, NOT codepoint                  */
  uint16_t right; /* glyph index                                 */
  float offset;   /* em units, added to advance after `left`     */
} MopFontKern;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(MopFontKern) == 8, "MopFontKern must be exactly 8 bytes");
#endif

/* -------------------------------------------------------------------------
 * Runtime accessors — exposed across the engine to subsystems that need
 * direct access to the atlas and metrics tables (e.g., the text
 * rasterizer in src/core/text.c).  These are NOT part of the public API.
 * ------------------------------------------------------------------------- */

#include <mop/core/font.h>

/* Atlas pixel buffer + dimensions.  Returns NULL when font is NULL. */
const uint8_t *mop_font_atlas_pixels(const MopFont *font, int *out_w,
                                     int *out_h, int *out_channels);

/* px_range from the bake (in atlas pixels) — needed for the SDF→alpha
 * conversion in the rasterizer. */
float mop_font_px_range(const MopFont *font);

/* em_size from the bake (in source pixels) — paired with px_range. */
float mop_font_em_size(const MopFont *font);

/* Glyph lookup — returns NULL when the codepoint isn't in the atlas. */
const MopFontGlyph *mop_font_lookup_glyph(const MopFont *font, uint32_t cp);

/* Kerning offset, em units, between two glyphs.  Looked up via
 * pointer arithmetic on the glyph table — pass pointers returned by
 * mop_font_lookup_glyph. */
float mop_font_kerning(const MopFont *font, const MopFontGlyph *left,
                       const MopFontGlyph *right);

#endif /* MOP_CORE_FONT_INTERNAL_H */
