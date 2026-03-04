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
  /* Neutral gray background (linear space) — standard viewport look */
  MopColor bg = {0.028f, 0.028f, 0.032f, 1.0f};

  /* #FFBB00 amber accent — all UI chrome uses this */
  MopColor accent = {1.00f, 0.733f, 0.0f, 1.0f};

  /* Derived accent shades */
  MopColor accent_dim = {accent.r * 0.15f, accent.g * 0.15f, accent.b * 0.15f,
                         1.0f};
  MopColor accent_mid = {accent.r * 0.25f, accent.g * 0.25f, accent.b * 0.25f,
                         1.0f};
  MopColor accent_bright = {fminf(accent.r * 1.3f, 1.0f),
                            fminf(accent.g * 1.3f, 1.0f),
                            fminf(accent.b * 1.3f, 1.0f), 1.0f};

  return (MopTheme){
      .accent = accent,

      /* Background: neutral gray with subtle gradient */
      .bg_top = {bg.r + 0.006f, bg.g + 0.006f, bg.b + 0.006f, 1.0f},
      .bg_bottom = bg,

      /* Grid: derived from accent */
      .grid_minor = accent_dim,
      .grid_major = accent_mid,
      .grid_axis_x = accent,
      .grid_axis_z = accent,
      .grid_line_width_minor = 1.0f,
      .grid_line_width_major = 1.0f,
      .grid_line_width_axis = 1.5f,

      /* Gizmo: ALL handles use accent color */
      .gizmo_x = accent,
      .gizmo_y = accent,
      .gizmo_z = accent,
      .gizmo_center = accent,
      .gizmo_hover = accent_bright,
      .gizmo_line_width = 1.5f,
      .gizmo_opacity = 1.0f,
      .gizmo_target_opacity = 0.45f,

      /* Wireframe */
      .wireframe_color = accent,
      .wireframe_opacity = 0.18f,
      .wireframe_line_width = 1.0f,

      /* Selection: uses accent color */
      .selection_outline = accent,
      .selection_outline_width = 2.5f,
      .vertex_select_color = accent,
      .edge_select_color = accent,
      .face_select_color = accent_mid,
      .vertex_select_size = 5.0f,
      .edge_select_width = 2.5f,
      .face_select_opacity = 0.35f,

      /* Object outline: only on selected objects */
      .outline_opacity_selected = 0.85f,
      .outline_opacity_unselected = 0.0f,

      /* Normals */
      .normal_color = accent,
      .normal_line_width = 1.0f,

      /* Bounds */
      .bounds_color = accent,
      .bounds_line_width = 1.0f,

      /* Axis indicator */
      .axis_x = accent,
      .axis_y = accent,
      .axis_z = accent,

      /* Camera frustum */
      .camera_frustum_color = accent,
      .camera_frustum_line_width = 1.0f,

      /* Depth bias */
      .depth_bias = 0.0001f,
  };
}
