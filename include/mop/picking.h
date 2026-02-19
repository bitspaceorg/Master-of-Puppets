/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * picking.h — Object picking via ID buffer
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_PICKING_H
#define MOP_PICKING_H

#include "types.h"

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Pick result
 *
 * hit       : true if a rendered object was found at (x, y)
 * object_id : the object_id of the hit mesh (valid only when hit == true)
 * depth     : normalized depth at the hit point, [0, 1] (valid when hit)
 * ------------------------------------------------------------------------- */

typedef struct MopPickResult {
    bool     hit;
    uint32_t object_id;
    float    depth;
} MopPickResult;

/* -------------------------------------------------------------------------
 * Picking
 *
 * Query the object at pixel coordinates (x, y).  Coordinates are relative
 * to the top-left corner of the framebuffer.
 *
 * Picking reads from the most recently rendered frame.  Call
 * mop_viewport_render before picking to get up-to-date results.
 *
 * If (x, y) is outside the framebuffer bounds, returns { .hit = false }.
 * ------------------------------------------------------------------------- */

MopPickResult mop_viewport_pick(MopViewport *viewport, int x, int y);

#endif /* MOP_PICKING_H */
