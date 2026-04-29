/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * display.h — Per-viewport display settings
 *
 * Controls visual overlays and debug visualizations:
 * wireframe-on-shaded, vertex normals, bounding boxes, vertex maps.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_DISPLAY_H
#define MOP_CORE_DISPLAY_H

#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Debug visualization modes (Phase 9B)
 *
 * These replace normal shading with diagnostic visualizations.
 * Set via mop_viewport_set_debug_viz().  MOP_DEBUG_VIZ_NONE restores
 * normal rendering.
 * ------------------------------------------------------------------------- */

typedef enum MopDebugViz {
  MOP_DEBUG_VIZ_NONE = 0,            /* normal rendering */
  MOP_DEBUG_VIZ_OVERDRAW = 1,        /* overdraw heatmap (red = high) */
  MOP_DEBUG_VIZ_SHADOW_CASCADES = 2, /* tint by shadow cascade index */
  MOP_DEBUG_VIZ_LOD_LEVEL = 3,       /* color by active LOD level */
  MOP_DEBUG_VIZ_CULL_RESULT = 4, /* green=visible, red=culled (1-frame lag) */
  MOP_DEBUG_VIZ_DEPTH = 5,       /* linear depth grayscale */
  MOP_DEBUG_VIZ_NORMALS = 6,     /* world normals as RGB */
  MOP_DEBUG_VIZ_MIPMAP = 7,      /* Hi-Z mip level visualization */
} MopDebugViz;

/* -------------------------------------------------------------------------
 * Vertex map visualization modes
 * ------------------------------------------------------------------------- */

typedef enum MopVertexMapDisplay {
  MOP_VTXMAP_NONE = 0,
  MOP_VTXMAP_UV = 1,      /* UV coords as color */
  MOP_VTXMAP_WEIGHTS = 2, /* bone weights heatmap */
  MOP_VTXMAP_NORMALS = 3, /* normal direction as RGB */
  MOP_VTXMAP_CUSTOM = 4,  /* custom attrib channel */
  MOP_VTXMAP_FACE_ID = 5, /* per-face deterministic random color */
} MopVertexMapDisplay;

/* -------------------------------------------------------------------------
 * Display settings — controls all visual overlays
 *
 * Default: all overlays disabled, matching legacy behavior.
 * ------------------------------------------------------------------------- */

typedef struct MopDisplaySettings {
  /* Wireframe overlay */
  bool wireframe_overlay;
  MopColor wireframe_color; /* default: (1, 0.6, 0.2, 1) — orange */
  float wireframe_opacity;  /* 0..1, default: 0.15 */

  /* Vertex visualization */
  bool show_normals;
  float normal_display_length; /* world units, default: 0.1 */
  bool show_bounds;
  bool show_vertices;
  float vertex_display_size; /* pixels, default: 3.0 */

  /* Vertex map coloring */
  MopVertexMapDisplay vertex_map_mode;
  uint32_t vertex_map_channel; /* which CUSTOM attrib */

  /* Debug visualization (Phase 9B) */
  MopDebugViz debug_viz;
} MopDisplaySettings;

/* -------------------------------------------------------------------------
 * Functions
 * ------------------------------------------------------------------------- */

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* Returns default settings (all overlays disabled). */
MopDisplaySettings mop_display_settings_default(void);

/* Set display settings for a viewport. */
void mop_viewport_set_display(MopViewport *vp, const MopDisplaySettings *ds);

/* Get the current display settings. */
MopDisplaySettings mop_viewport_get_display(const MopViewport *vp);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_DISPLAY_H */
