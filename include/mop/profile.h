/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * profile.h — Frame profiling statistics
 *
 * After each call to mop_viewport_render, timing data is stored in the
 * viewport.  Retrieve it with mop_viewport_get_stats to display or log
 * performance information.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_PROFILE_H
#define MOP_PROFILE_H

#include "types.h"

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Frame statistics
 *
 * All time values are in milliseconds.
 * triangle_count : total triangles submitted (index_count / 3 per mesh)
 * pixel_count    : total framebuffer pixels (width * height)
 * ------------------------------------------------------------------------- */

typedef struct MopFrameStats {
    double   frame_time_ms;
    double   clear_ms;
    double   transform_ms;
    double   rasterize_ms;
    uint32_t triangle_count;
    uint32_t pixel_count;
} MopFrameStats;

/* Retrieve the statistics from the most recent mop_viewport_render call.
 * Returns a zeroed struct if viewport is NULL. */
MopFrameStats mop_viewport_get_stats(const MopViewport *viewport);

#endif /* MOP_PROFILE_H */
