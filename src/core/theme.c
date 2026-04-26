/*
 * Master of Puppets — Viewport Theme
 * theme.c — Default theme (MOP's design language)
 *
 * MOP's visual identity:
 *   - Very dark navy background (#040117)
 *   - Electric pink accent for ALL UI chrome (gizmos, outlines, indicators)
 *   - Single accent color — change theme.accent to reskin everything
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <mop/core/theme.h>

MopTheme mop_theme_default(void) {
  /* Brighter mid-gray background (linear space) — fresh, airy viewport */
  MopColor bg = {0.10f, 0.10f, 0.11f, 1.0f};

  /* Bright white accent — high contrast selection on mid-gray bg */
  MopColor accent = {1.0f, 1.0f, 1.0f, 1.0f};

  /* Derived mid shade (for face selection fill) */
  MopColor accent_mid = {0.35f, 0.35f, 0.35f, 1.0f};

  return (MopTheme){
      .accent = accent,

      /* Background: brighter gray with subtle gradient */
      .bg_top = {bg.r + 0.012f, bg.g + 0.012f, bg.b + 0.012f, 1.0f},
      .bg_bottom = bg,

      /* Grid: visible gray lines.  The world-origin axes are also gray
       * (slightly brighter than grid_major) instead of saturated red/blue
       * — the corner navigator + transform gizmo already carry the X/Y/Z
       * color coding, and a colored grid axis competes with those signals
       * at oblique camera angles. */
      .grid_minor = {0.30f, 0.30f, 0.30f, 1.0f},
      .grid_major = {0.45f, 0.45f, 0.45f, 1.0f},
      .grid_axis_x = {0.70f, 0.70f, 0.70f, 1.0f},
      .grid_axis_z = {0.70f, 0.70f, 0.70f, 1.0f},
      .grid_line_width_minor = 1.0f,
      .grid_line_width_major = 1.0f,
      .grid_line_width_axis = 4.0f,

      /* Gizmo: bright saturated RGB per axis */
      .gizmo_x = {0.93f, 0.27f, 0.27f, 1.0f},
      .gizmo_y = {0.27f, 0.72f, 0.27f, 1.0f},
      .gizmo_z = {0.30f, 0.45f, 0.93f, 1.0f},
      .gizmo_center = {0.95f, 0.95f, 0.95f, 1.0f},
      .gizmo_hover = {1.0f, 1.0f, 0.4f, 1.0f},
      .gizmo_line_width = 6.0f,
      .gizmo_opacity = 1.0f,
      .gizmo_target_opacity = 0.45f,

      /* Wireframe: bright white */
      .wireframe_color = accent,
      .wireframe_opacity = 0.22f,
      .wireframe_line_width = 1.0f,

      /* Selection: bright white — high contrast */
      .selection_outline = accent,
      .selection_outline_width = 2.5f,
      .vertex_select_color = accent,
      .edge_select_color = accent,
      .face_select_color = accent_mid,
      .vertex_select_size = 5.0f,
      .edge_select_width = 2.5f,
      .face_select_opacity = 0.35f,

      /* Object outline: only on selected objects */
      .outline_opacity_selected = 0.90f,
      .outline_opacity_unselected = 0.0f,

      /* Normals: bright white */
      .normal_color = accent,
      .normal_line_width = 1.0f,

      /* Bounds: bright white */
      .bounds_color = accent,
      .bounds_line_width = 1.0f,

      /* Axis indicator: bright saturated RGB */
      .axis_x = {1.0f, 0.2f, 0.2f, 1.0f},
      .axis_y = {0.2f, 0.85f, 0.2f, 1.0f},
      .axis_z = {0.25f, 0.4f, 1.0f, 1.0f},
      .axis_neg_x = {0.55f, 0.20f, 0.20f, 1.0f},
      .axis_neg_y = {0.20f, 0.42f, 0.20f, 1.0f},
      .axis_neg_z = {0.22f, 0.28f, 0.55f, 1.0f},

      /* Camera frustum: bright white */
      .camera_frustum_color = {0.90f, 0.90f, 0.90f, 1.0f},
      .camera_frustum_line_width = 3.0f,

      /* Depth bias */
      .depth_bias = 0.0001f,
  };
}
