/*
 * Master of Puppets — Display Settings
 * display.c — Default construction and viewport get/set
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "viewport/viewport_internal.h"

MopDisplaySettings mop_display_settings_default(void) {
    MopDisplaySettings ds;
    ds.wireframe_overlay   = false;
    ds.wireframe_color     = (MopColor){ 1.0f, 0.6f, 0.2f, 1.0f };
    ds.wireframe_opacity   = 0.15f;
    ds.show_normals        = false;
    ds.normal_display_length = 0.1f;
    ds.show_bounds         = false;
    ds.show_vertices       = false;
    ds.vertex_display_size = 3.0f;
    ds.vertex_map_mode     = MOP_VTXMAP_NONE;
    ds.vertex_map_channel  = 0;
    return ds;
}

void mop_viewport_set_display(MopViewport *vp, const MopDisplaySettings *ds) {
    if (!vp || !ds) return;
    vp->display = *ds;
}

MopDisplaySettings mop_viewport_get_display(const MopViewport *vp) {
    if (!vp) return mop_display_settings_default();
    return vp->display;
}
