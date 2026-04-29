/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * core/font.h — Font asset loading + glyph metrics
 *
 * MOP fonts are pre-baked at asset-build time into the .mfa binary
 * format (MOP Font Atlas).  At runtime the font is just an atlas
 * texture + a metrics table — there is no runtime TTF parsing or
 * glyph rasterization, so a font that loads will never produce a
 * blurry, missing, or partially-rendered glyph at draw time.
 *
 * The bake tool (tools/mop_font_bake) consumes a TTF and emits a
 * .mfa.  See docs/reference/core/font.mdx for format details.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_FONT_H
#define MOP_CORE_FONT_H

#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Atlas type — picked at bake time.  The runtime sampler is selected
 * automatically; user code does not branch on this.
 * ------------------------------------------------------------------------- */

typedef enum MopFontAtlasType {
  MOP_FONT_ATLAS_SDF = 0,    /* single-channel signed distance field */
  MOP_FONT_ATLAS_MSDF = 1,   /* three-channel multi-channel SDF */
  MOP_FONT_ATLAS_BITMAP = 2, /* raw alpha (no scale-invariance) */
} MopFontAtlasType;

/* -------------------------------------------------------------------------
 * Font metrics — em-relative.  Multiply by desired pixel size to get
 * pixel-space metrics for a given draw call.
 * ------------------------------------------------------------------------- */

typedef struct MopFontMetrics {
  float ascent;      /* baseline → top, em units                       */
  float descent;     /* baseline → bottom, em units (negative)         */
  float line_gap;    /* gap between lines, em units                    */
  float line_height; /* ascent − descent + line_gap, em units          */
  float em_size;     /* source em size used at bake (informational)    */
  float px_range;    /* SDF/MSDF range in atlas pixels (shader uniform)*/
  uint32_t glyph_count;
  uint32_t kerning_count;
} MopFontMetrics;

/* -------------------------------------------------------------------------
 * Opaque handle
 * ------------------------------------------------------------------------- */

typedef struct MopFont MopFont;

/* -------------------------------------------------------------------------
 * Loading
 *
 * mop_font_load reads a .mfa file from disk; the implementation may
 * mmap the file, in which case the returned MopFont retains an
 * internal reference to the mapping until mop_font_free.
 *
 * mop_font_load_memory consumes a caller-owned blob (typically the
 * embedded HUD font baked into the binary at build time).  The blob
 * MUST remain valid for the lifetime of the returned MopFont; the
 * loader does NOT copy.
 *
 * Both return NULL on any kind of failure (open, magic, version,
 * truncation).  Errors are logged via MOP_ERROR.
 * ------------------------------------------------------------------------- */

MopFont *mop_font_load(const char *path);
MopFont *mop_font_load_memory(const void *data, size_t size);
void mop_font_free(MopFont *font);

/* -------------------------------------------------------------------------
 * Inspection
 *
 * All measurements are em-relative.  px_size is the desired pixel
 * height of the line; the caller is responsible for the multiply.
 * ------------------------------------------------------------------------- */

MopFontMetrics mop_font_metrics(const MopFont *font);
MopFontAtlasType mop_font_atlas_type(const MopFont *font);

/* Width of a UTF-8 string at px_size, in pixels.  Includes kerning.
 * Returns 0.0f for an empty string or NULL inputs.
 *
 * Single-line strings only — newlines reset kerning but do not break the
 * advance accumulator, so for multi-line text use mop_text_measure_extent
 * which returns both width (longest line) and height (line count * line
 * height + ascent). */
float mop_text_measure(const MopFont *font, const char *utf8, float px_size);

/* Measure both width (pixels of the longest line) and height (pixels from
 * top of first line to bottom of last line) for a UTF-8 string at px_size.
 *
 * Width sums per-glyph advances + kerning, in line-local terms; '\n' resets
 * the line accumulator and bumps the line count. Height is line_height in
 * pixels times (lines - 1) plus ascent - descent for the first/last line.
 *
 * Either output pointer may be NULL — pass &w only if you don't care about
 * height. Returns 0 width / 0 height for empty or NULL inputs.
 *
 * Use case: right-aligning HUD text without hardcoded glyph widths:
 *
 *   float w;
 *   mop_text_measure_extent(font, "FPS 144", 14.0f, &w, NULL);
 *   mop_text_draw_2d(vp, font, "FPS 144", screen_w - w - 8, 8, style);
 */
void mop_text_measure_extent(const MopFont *font, const char *utf8,
                             float px_size, float *out_width,
                             float *out_height);

/* -------------------------------------------------------------------------
 * Default font — JetBrains Mono
 *
 * MOP's design language ships JetBrains Mono (OFL) as the canonical
 * font for HUD chrome, navigator labels, gizmo axis tags, and
 * selection callouts.  The engine bakes it into an SDF atlas at
 * build time (`make fonts`) and embeds it as a `const uint8_t[]`
 * inside libmop, so:
 *
 *   • mop_font_hud() returns a usable MopFont* with zero disk I/O.
 *   • Every text draw call (mop_text_draw_2d / mop_text_draw_label)
 *     that passes `font = NULL` resolves to this same pointer.
 *
 * That makes JetBrains Mono the *de facto* default font of MOP —
 * nothing further is required of host code.  The pointer is owned
 * by the engine; do not free.
 *
 * Returns NULL only if the build was produced without `make fonts`
 * having generated the embedded atlas (defensive — the standard
 * build always includes it).
 * ------------------------------------------------------------------------- */

const MopFont *mop_font_hud(void);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_FONT_H */
