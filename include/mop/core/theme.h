/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * core/theme.h — Viewport theme and design language
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_THEME_H
#define MOP_CORE_THEME_H

#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * MOP Design Language — all visual constants in one place
 *
 * The theme defines MOP's unique visual identity.  Every color, line
 * width, and opacity used in viewport chrome, overlays, and gizmos is
 * controlled by the active theme.
 * ------------------------------------------------------------------------- */

typedef struct MopTheme {
  /* UI accent — single color for all UI chrome (outlines, indicators, etc.) */
  MopColor accent;

  /* Background gradient */
  MopColor bg_top;
  MopColor bg_bottom;

  /* Grid */
  MopColor grid_minor;
  MopColor grid_major;
  MopColor grid_axis_x;
  MopColor grid_axis_z;
  float grid_line_width_minor;
  float grid_line_width_major;
  float grid_line_width_axis;

  /* Gizmo */
  MopColor gizmo_x;
  MopColor gizmo_y;
  MopColor gizmo_z;
  MopColor gizmo_center;
  MopColor gizmo_hover;
  float gizmo_line_width;
  float gizmo_opacity;
  float gizmo_target_opacity;

  /* Wireframe overlay */
  MopColor wireframe_color;
  float wireframe_opacity;
  float wireframe_line_width;

  /* Selection */
  MopColor selection_outline;
  float selection_outline_width;
  MopColor vertex_select_color;
  MopColor edge_select_color;
  MopColor face_select_color;
  float vertex_select_size;
  float edge_select_width;
  float face_select_opacity;

  /* Object outline (always-on wireframe border) */
  float outline_opacity_selected;
  float outline_opacity_unselected;

  /* Overlays */
  MopColor normal_color;
  float normal_line_width;
  MopColor bounds_color;
  float bounds_line_width;

  /* HUD axis indicator */
  MopColor axis_x;
  MopColor axis_y;
  MopColor axis_z;
  MopColor axis_neg_x;
  MopColor axis_neg_y;
  MopColor axis_neg_z;

  /* Camera frustum visualization */
  MopColor camera_frustum_color;
  float camera_frustum_line_width;

  /* Depth bias for coplanar overlays */
  float depth_bias;
} MopTheme;

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* Built-in themes */
MopTheme mop_theme_default(void);

/* Set/get the active theme for a viewport */
void mop_viewport_set_theme(MopViewport *vp, const MopTheme *theme);
const MopTheme *mop_viewport_get_theme(const MopViewport *vp);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_THEME_H */
