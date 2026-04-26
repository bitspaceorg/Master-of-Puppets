/*
 * Master of Puppets — Font bake tool
 * mop_font_bake.c — TTF → .mfa (SDF atlas + metrics)
 *
 * Pure C, vendor-only deps (stb_truetype).  Reads a TrueType font,
 * generates a single-channel SDF atlas covering a configurable
 * glyph set, and writes the .mfa binary that mop_font_load consumes.
 *
 * Usage:
 *   mop_font_bake INPUT.ttf --out OUT.mfa
 *                  [--glyphs ascii|ascii+latin1]
 *                  [--size 64]      source glyph height in pixels
 *                  [--padding 4]    SDF range in pixels
 *                  [--name SYMBOL]  also emit OUT.mfa.h with
 *                                   const uint8_t mop_embedded_<SYMBOL>[]
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "../src/core/font_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * CLI parsing
 * ------------------------------------------------------------------------- */

typedef struct CliArgs {
  const char *input;
  const char *output;
  const char *glyph_set;  /* "ascii", "ascii+latin1" */
  const char *embed_name; /* if non-NULL, emit a C header next to OUT */
  int source_px;
  int padding;
} CliArgs;

static void usage(const char *prog) {
  fprintf(stderr,
          "usage: %s INPUT.ttf --out OUT.mfa\n"
          "       [--glyphs ascii|ascii+latin1] [--size 64] [--padding 4]\n"
          "       [--name SYMBOL]\n",
          prog);
}

static int parse_args(int argc, char **argv, CliArgs *out) {
  memset(out, 0, sizeof(*out));
  out->glyph_set = "ascii";
  out->source_px = 64;
  out->padding = 4;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] != '-' && !out->input) {
      out->input = a;
    } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
      out->output = argv[++i];
    } else if (strcmp(a, "--glyphs") == 0 && i + 1 < argc) {
      out->glyph_set = argv[++i];
    } else if (strcmp(a, "--size") == 0 && i + 1 < argc) {
      out->source_px = atoi(argv[++i]);
    } else if (strcmp(a, "--padding") == 0 && i + 1 < argc) {
      out->padding = atoi(argv[++i]);
    } else if (strcmp(a, "--name") == 0 && i + 1 < argc) {
      out->embed_name = argv[++i];
    } else {
      fprintf(stderr, "mop_font_bake: unknown arg '%s'\n", a);
      return 0;
    }
  }
  if (!out->input || !out->output) {
    return 0;
  }
  if (out->source_px < 8 || out->source_px > 256) {
    fprintf(stderr, "mop_font_bake: --size must be in [8, 256]\n");
    return 0;
  }
  if (out->padding < 1 || out->padding > 32) {
    fprintf(stderr, "mop_font_bake: --padding must be in [1, 32]\n");
    return 0;
  }
  return 1;
}

/* -------------------------------------------------------------------------
 * Glyph set expansion — turns "ascii", "ascii+latin1" into a sorted
 * array of unique codepoints.  The set always includes U+FFFD so the
 * loader's "missing glyph" fallback is satisfied within the file.
 * ------------------------------------------------------------------------- */

static int append_range(uint32_t *cps, int count, uint32_t lo, uint32_t hi) {
  for (uint32_t cp = lo; cp <= hi; cp++)
    cps[count++] = cp;
  return count;
}

static int compare_u32(const void *a, const void *b) {
  uint32_t ua = *(const uint32_t *)a, ub = *(const uint32_t *)b;
  return (ua > ub) - (ua < ub);
}

static int build_glyph_set(const char *spec, uint32_t **out_cps) {
  /* Worst case: ASCII (95) + latin1 (96) + .notdef = ~200 — allocate
   * 1024 to leave headroom for future ranges. */
  uint32_t *cps = (uint32_t *)malloc(sizeof(uint32_t) * 1024);
  int count = 0;

  /* .notdef placeholder — always present so missing-glyph fallback
   * has a home in every atlas. */
  cps[count++] = 0xFFFDu;

  /* Printable ASCII is the floor for every set we ship. */
  count = append_range(cps, count, 0x20, 0x7E);

  if (strcmp(spec, "ascii+latin1") == 0) {
    count = append_range(cps, count, 0xA0, 0xFF);
  } else if (strcmp(spec, "ascii") != 0) {
    fprintf(stderr, "mop_font_bake: unknown glyph set '%s'\n", spec);
    free(cps);
    return -1;
  }

  qsort(cps, (size_t)count, sizeof(uint32_t), compare_u32);

  /* Dedup in place — append_range can produce duplicates if ranges
   * overlap; today they don't, but keep the path honest. */
  int unique = 1;
  for (int i = 1; i < count; i++) {
    if (cps[i] != cps[unique - 1])
      cps[unique++] = cps[i];
  }

  *out_cps = cps;
  return unique;
}

/* -------------------------------------------------------------------------
 * Per-glyph SDF result — kept around through pack and atlas blit.
 * ------------------------------------------------------------------------- */

typedef struct GlyphBake {
  uint32_t codepoint;
  int glyph_index_in_font; /* stbtt index, for kerning lookup */
  unsigned char *bitmap;   /* malloc'd by stbtt; we free            */
  int w, h;                /* bitmap dimensions in atlas pixels      */
  int xoff, yoff;          /* bitmap offset from pen, pixels (Y-down) */
  int advance_unscaled;    /* font-unit advance                      */
  /* Pack output */
  int atlas_x, atlas_y;
} GlyphBake;

/* -------------------------------------------------------------------------
 * Shelf packer — sort by height descending, lay out in rows.
 * Returns 1 on success, 0 if the requested atlas size can't hold them.
 * ------------------------------------------------------------------------- */

static int compare_glyph_height_desc(const void *a, const void *b) {
  const GlyphBake *ga = (const GlyphBake *)a;
  const GlyphBake *gb = (const GlyphBake *)b;
  return gb->h - ga->h;
}

static int try_pack(GlyphBake *glyphs, int n, int atlas_w, int atlas_h,
                    int gap) {
  qsort(glyphs, (size_t)n, sizeof(GlyphBake), compare_glyph_height_desc);

  int x = gap, y = gap, row_h = 0;
  for (int i = 0; i < n; i++) {
    GlyphBake *g = &glyphs[i];
    if (g->w == 0 || g->h == 0) {
      g->atlas_x = 0;
      g->atlas_y = 0;
      continue;
    }
    if (g->w + 2 * gap > atlas_w)
      return 0; /* glyph wider than atlas */

    if (x + g->w + gap > atlas_w) {
      /* New row */
      y += row_h + gap;
      x = gap;
      row_h = 0;
    }
    if (y + g->h + gap > atlas_h)
      return 0; /* doesn't fit */

    g->atlas_x = x;
    g->atlas_y = y;
    x += g->w + gap;
    if (g->h > row_h)
      row_h = g->h;
  }
  return 1;
}

/* -------------------------------------------------------------------------
 * Find the smallest power-of-two atlas that fits all glyphs.
 * Tries 256, 512, 1024, 2048, 4096.  Caller frees `atlas`.
 * Returns the atlas width on success, 0 on failure.
 * ------------------------------------------------------------------------- */

static int pack_and_compose(GlyphBake *glyphs, int n, int padding,
                            uint8_t **out_atlas, int *out_w, int *out_h) {
  static const int sizes[] = {256, 512, 1024, 2048, 4096};
  int gap = padding; /* SDFs already include their own padding; we
                      * add a one-pixel gap to keep neighbors from
                      * bleeding through bilinear filtering. */
  int packed_w = 0, packed_h = 0;

  for (int s = 0; s < (int)(sizeof(sizes) / sizeof(sizes[0])); s++) {
    int w = sizes[s];
    int h = sizes[s];
    if (try_pack(glyphs, n, w, h, gap)) {
      packed_w = w;
      packed_h = h;
      break;
    }
  }
  if (!packed_w) {
    fprintf(stderr, "mop_font_bake: glyphs do not fit in 4096²\n");
    return 0;
  }

  /* Allocate atlas, zero-fill (background = "far outside" in SDF). */
  uint8_t *atlas = (uint8_t *)calloc((size_t)(packed_w * packed_h), 1);
  if (!atlas) {
    fprintf(stderr, "mop_font_bake: out of memory for atlas\n");
    return 0;
  }

  /* Blit each glyph SDF into its packed slot. */
  for (int i = 0; i < n; i++) {
    const GlyphBake *g = &glyphs[i];
    if (!g->bitmap || g->w == 0 || g->h == 0)
      continue;
    for (int row = 0; row < g->h; row++) {
      const uint8_t *src = g->bitmap + row * g->w;
      uint8_t *dst = atlas + (g->atlas_y + row) * packed_w + g->atlas_x;
      memcpy(dst, src, (size_t)g->w);
    }
  }

  *out_atlas = atlas;
  *out_w = packed_w;
  *out_h = packed_h;
  return packed_w;
}

/* -------------------------------------------------------------------------
 * Kerning table — query stbtt for every pair, keep non-zero ones.
 * Sorted by (left_idx, right_idx) for runtime binary search.
 * ------------------------------------------------------------------------- */

typedef struct KernEntry {
  uint16_t left;
  uint16_t right;
  float offset;
} KernEntry;

static int compare_kern(const void *a, const void *b) {
  const KernEntry *ka = (const KernEntry *)a;
  const KernEntry *kb = (const KernEntry *)b;
  uint64_t la = ((uint64_t)ka->left << 16) | ka->right;
  uint64_t lb = ((uint64_t)kb->left << 16) | kb->right;
  return (la > lb) - (la < lb);
}

static KernEntry *build_kern_table(const stbtt_fontinfo *info,
                                   const GlyphBake *glyphs, int n,
                                   float em_scale, int *out_count) {
  /* Worst case: n*n pairs.  Cap at 32k entries — JBM Regular yields
   * far fewer in practice. */
  size_t cap = (size_t)n * (size_t)n;
  if (cap > 32768)
    cap = 32768;
  KernEntry *kerns = (KernEntry *)malloc(sizeof(KernEntry) * cap);
  int count = 0;

  for (int li = 0; li < n; li++) {
    for (int ri = 0; ri < n; ri++) {
      int kern_unscaled = stbtt_GetGlyphKernAdvance(
          (stbtt_fontinfo *)info, glyphs[li].glyph_index_in_font,
          glyphs[ri].glyph_index_in_font);
      if (kern_unscaled == 0)
        continue;
      if ((size_t)count >= cap)
        break;
      kerns[count].left = (uint16_t)li;
      kerns[count].right = (uint16_t)ri;
      kerns[count].offset = (float)kern_unscaled * em_scale;
      count++;
    }
  }

  qsort(kerns, (size_t)count, sizeof(KernEntry), compare_kern);
  *out_count = count;
  return kerns;
}

/* -------------------------------------------------------------------------
 * Read a whole file into a malloc'd buffer.  Returns NULL on failure.
 * ------------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *out_size) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return NULL;
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz <= 0) {
    fclose(fp);
    return NULL;
  }
  uint8_t *buf = (uint8_t *)malloc((size_t)sz);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
    free(buf);
    fclose(fp);
    return NULL;
  }
  fclose(fp);
  *out_size = (size_t)sz;
  return buf;
}

/* -------------------------------------------------------------------------
 * Emit a C source file that defines the .mfa as a const uint8_t
 * array with linker-visible (non-static) symbols.  The companion TU
 * lives in libmop and font.c references it via `extern`.
 *
 * We emit `.c` rather than `.h` so the embedded blob can only be
 * defined once across the link — including a `static` const in
 * multiple TUs would silently produce duplicate copies.
 * ------------------------------------------------------------------------- */

static int emit_embed_source(const char *out_path, const char *symbol,
                             const uint8_t *blob, size_t size) {
  char src_path[1024];
  snprintf(src_path, sizeof(src_path), "%s.c", out_path);
  FILE *fp = fopen(src_path, "wb");
  if (!fp)
    return 0;

  fprintf(fp,
          "/* Generated by mop_font_bake — do not edit. */\n"
          "#include <stddef.h>\n"
          "#include <stdint.h>\n\n"
          "const uint8_t mop_embedded_%s[] = {\n",
          symbol);

  for (size_t i = 0; i < size; i++) {
    fprintf(fp, "0x%02X,", blob[i]);
    if ((i & 15) == 15)
      fputc('\n', fp);
  }

  fprintf(fp,
          "\n};\n"
          "const size_t mop_embedded_%s_size = %zu;\n",
          symbol, size);
  fclose(fp);
  return 1;
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
  CliArgs args;
  if (!parse_args(argc, argv, &args)) {
    usage(argv[0]);
    return 2;
  }

  /* --- Load TTF --- */
  size_t ttf_size = 0;
  uint8_t *ttf = read_file(args.input, &ttf_size);
  if (!ttf) {
    fprintf(stderr, "mop_font_bake: failed to read '%s'\n", args.input);
    return 1;
  }

  stbtt_fontinfo info;
  if (!stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
    fprintf(stderr, "mop_font_bake: stbtt_InitFont failed\n");
    free(ttf);
    return 1;
  }

  /* --- Build glyph set --- */
  uint32_t *codepoints = NULL;
  int cp_count = build_glyph_set(args.glyph_set, &codepoints);
  if (cp_count < 0) {
    free(ttf);
    return 1;
  }

  /* --- Bake each glyph --- */
  GlyphBake *glyphs = (GlyphBake *)calloc((size_t)cp_count, sizeof(GlyphBake));
  if (!glyphs) {
    free(ttf);
    free(codepoints);
    return 1;
  }

  float pixel_scale = stbtt_ScaleForPixelHeight(&info, (float)args.source_px);
  /* em_scale converts unscaled font units → em units (em is the
   * canonical unit of the font; one em equals the design unitsPerEm). */
  int ascent_unscaled, descent_unscaled, line_gap_unscaled;
  stbtt_GetFontVMetrics(&info, &ascent_unscaled, &descent_unscaled,
                        &line_gap_unscaled);
  /* unitsPerEm isn't directly exposed, but ScaleForPixelHeight = px / (ascent
   * - descent) for stbtt's convention.  Convert by deriving font_units_per_em
   * from the scale: em = px / font_units_per_em → 1/font_units_per_em =
   * pixel_scale / source_px. */
  float em_scale = pixel_scale / (float)args.source_px;

  /* Used for SDF onedge value; 128 is a good middle. */
  const unsigned char on_edge_value = 128;
  /* pixel_dist_scale: how many units of distance per pixel. */
  const float pixel_dist_scale = (float)on_edge_value / (float)args.padding;

  for (int i = 0; i < cp_count; i++) {
    GlyphBake *g = &glyphs[i];
    g->codepoint = codepoints[i];
    g->glyph_index_in_font = stbtt_FindGlyphIndex(&info, (int)codepoints[i]);

    int adv, lsb;
    stbtt_GetGlyphHMetrics(&info, g->glyph_index_in_font, &adv, &lsb);
    g->advance_unscaled = adv;

    /* Generate SDF.  May return NULL for codepoints with no outline
     * (e.g., space) — that's fine; we keep the glyph entry with
     * w=h=0 so layout still uses the advance. */
    g->bitmap = stbtt_GetGlyphSDF(&info, pixel_scale, g->glyph_index_in_font,
                                  args.padding, on_edge_value, pixel_dist_scale,
                                  &g->w, &g->h, &g->xoff, &g->yoff);
  }

  /* --- Pack atlas --- */
  uint8_t *atlas = NULL;
  int atlas_w = 0, atlas_h = 0;
  if (!pack_and_compose(glyphs, cp_count, args.padding, &atlas, &atlas_w,
                        &atlas_h)) {
    /* Free per-glyph bitmaps before bailing. */
    for (int i = 0; i < cp_count; i++)
      stbtt_FreeSDF(glyphs[i].bitmap, NULL);
    free(glyphs);
    free(codepoints);
    free(ttf);
    return 1;
  }

  /* --- Sort glyphs back to codepoint order so the on-disk table is
   * the binary-search-ready order the runtime expects. --- */
  /* Simple O(n²) reorder is fine at ~200 glyphs.  Build a parallel
   * GlyphBake array, walking codepoints[] in order. */
  GlyphBake *ordered = (GlyphBake *)calloc((size_t)cp_count, sizeof(GlyphBake));
  for (int i = 0; i < cp_count; i++) {
    for (int j = 0; j < cp_count; j++) {
      if (glyphs[j].codepoint == codepoints[i]) {
        ordered[i] = glyphs[j];
        break;
      }
    }
  }
  free(glyphs);
  glyphs = ordered;

  /* --- Build kern table (uses ordered glyph indices, since on-disk
   * kerning keys are glyph table indices, not stbtt indices). --- */
  int kern_count = 0;
  KernEntry *kerns =
      build_kern_table(&info, glyphs, cp_count, em_scale, &kern_count);

  /* --- Lay out output file --- */
  uint64_t glyph_table_offset = (uint64_t)MOP_FONT_HEADER_SIZE;
  uint64_t glyph_table_bytes = (uint64_t)cp_count * sizeof(MopFontGlyph);
  uint64_t kern_table_offset = glyph_table_offset + glyph_table_bytes;
  uint64_t kern_table_bytes = (uint64_t)kern_count * sizeof(MopFontKern);
  uint64_t atlas_offset = kern_table_offset + kern_table_bytes;
  uint64_t atlas_bytes = (uint64_t)atlas_w * (uint64_t)atlas_h;
  uint64_t total_size = atlas_offset + atlas_bytes;

  uint8_t *blob = (uint8_t *)calloc((size_t)total_size, 1);
  if (!blob) {
    fprintf(stderr, "mop_font_bake: out of memory for output blob\n");
    return 1;
  }

  /* Header */
  MopFontHeader hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = MOP_FONT_MAGIC;
  hdr.version = MOP_FONT_VERSION;
  hdr.atlas_type = MOP_FONT_TYPE_SDF;
  hdr.atlas_channels = 1;
  hdr.atlas_width = (uint32_t)atlas_w;
  hdr.atlas_height = (uint32_t)atlas_h;
  hdr.px_range = (float)args.padding;
  hdr.em_size = (float)args.source_px;
  hdr.ascent = (float)ascent_unscaled * em_scale;
  hdr.descent = (float)descent_unscaled * em_scale;
  hdr.line_gap = (float)line_gap_unscaled * em_scale;
  hdr.glyph_count = (uint32_t)cp_count;
  hdr.kerning_count = (uint32_t)kern_count;
  hdr.glyph_table_offset = glyph_table_offset;
  hdr.kerning_table_offset = kern_table_offset;
  hdr.atlas_offset = atlas_offset;
  memcpy(blob, &hdr, sizeof(hdr));

  /* Glyph table */
  MopFontGlyph *out_glyphs = (MopFontGlyph *)(blob + glyph_table_offset);
  for (int i = 0; i < cp_count; i++) {
    const GlyphBake *g = &glyphs[i];
    out_glyphs[i].codepoint = g->codepoint;
    out_glyphs[i].atlas_uv_min_x = (uint16_t)g->atlas_x;
    out_glyphs[i].atlas_uv_min_y = (uint16_t)g->atlas_y;
    out_glyphs[i].atlas_uv_max_x = (uint16_t)(g->atlas_x + g->w);
    out_glyphs[i].atlas_uv_max_y = (uint16_t)(g->atlas_y + g->h);
    /* Convert pixel-space offsets (Y-down) → em-space (Y-up).
     * stbtt's xoff/yoff: top-left of bitmap relative to pen, Y-down.
     * Plane is in em units, baseline-relative, Y-up. */
    out_glyphs[i].plane_min_x = (float)g->xoff / (float)args.source_px;
    out_glyphs[i].plane_max_x = (float)(g->xoff + g->w) / (float)args.source_px;
    out_glyphs[i].plane_max_y = (float)(-g->yoff) / (float)args.source_px;
    out_glyphs[i].plane_min_y =
        (float)(-(g->yoff + g->h)) / (float)args.source_px;
    out_glyphs[i].advance = (float)g->advance_unscaled * em_scale;
  }

  /* Kern table */
  if (kern_count > 0) {
    MopFontKern *out_kerns = (MopFontKern *)(blob + kern_table_offset);
    for (int i = 0; i < kern_count; i++) {
      out_kerns[i].left = kerns[i].left;
      out_kerns[i].right = kerns[i].right;
      out_kerns[i].offset = kerns[i].offset;
    }
  }

  /* Atlas pixels */
  memcpy(blob + atlas_offset, atlas, (size_t)atlas_bytes);

  /* --- Write file --- */
  FILE *out_fp = fopen(args.output, "wb");
  if (!out_fp) {
    fprintf(stderr, "mop_font_bake: failed to open '%s' for writing\n",
            args.output);
    return 1;
  }
  if (fwrite(blob, 1, (size_t)total_size, out_fp) != (size_t)total_size) {
    fprintf(stderr, "mop_font_bake: short write to '%s'\n", args.output);
    fclose(out_fp);
    return 1;
  }
  fclose(out_fp);

  fprintf(stderr,
          "mop_font_bake: wrote %s (%llu bytes, %dx%d atlas, %d glyphs, "
          "%d kerning pairs)\n",
          args.output, (unsigned long long)total_size, atlas_w, atlas_h,
          cp_count, kern_count);

  /* --- Optional embed source (companion TU for libmop) --- */
  if (args.embed_name) {
    if (!emit_embed_source(args.output, args.embed_name, blob,
                           (size_t)total_size)) {
      fprintf(stderr, "mop_font_bake: failed to emit embed source\n");
      return 1;
    }
    fprintf(stderr, "mop_font_bake: wrote %s.c (mop_embedded_%s)\n",
            args.output, args.embed_name);
  }

  /* --- Cleanup --- */
  for (int i = 0; i < cp_count; i++)
    stbtt_FreeSDF(glyphs[i].bitmap, NULL);
  free(glyphs);
  free(codepoints);
  free(kerns);
  free(atlas);
  free(blob);
  free(ttf);
  return 0;
}
