/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * gizmo.h — TRS gizmo system for interactive object manipulation
 *
 * Gizmos are visual handles (translate arrows, rotate rings, scale cubes)
 * that the application can attach to selected objects.  The gizmo module
 * manages handle geometry and computes transform deltas from mouse input;
 * the application owns TRS state and applies deltas itself.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_GIZMO_H
#define MOP_GIZMO_H

#include "types.h"
#include "picking.h"

/* Forward declarations */
typedef struct MopViewport MopViewport;
typedef struct MopGizmo    MopGizmo;

/* -------------------------------------------------------------------------
 * Gizmo mode — which kind of handle geometry to display
 * ------------------------------------------------------------------------- */

typedef enum MopGizmoMode {
    MOP_GIZMO_TRANSLATE = 0,
    MOP_GIZMO_ROTATE    = 1,
    MOP_GIZMO_SCALE     = 2
} MopGizmoMode;

/* -------------------------------------------------------------------------
 * Gizmo axis — which handle was picked or is being dragged
 * ------------------------------------------------------------------------- */

typedef enum MopGizmoAxis {
    MOP_GIZMO_AXIS_NONE   = -1,
    MOP_GIZMO_AXIS_X      = 0,
    MOP_GIZMO_AXIS_Y      = 1,
    MOP_GIZMO_AXIS_Z      = 2,
    MOP_GIZMO_AXIS_CENTER = 3
} MopGizmoAxis;

/* -------------------------------------------------------------------------
 * Gizmo delta — transform offset produced by a drag operation
 *
 * Only the field corresponding to the active mode is meaningful:
 *   TRANSLATE → translate (world-space offset)
 *   ROTATE    → rotate    (euler angle delta in radians)
 *   SCALE     → scale     (additive scale delta)
 * ------------------------------------------------------------------------- */

typedef struct MopGizmoDelta {
    MopVec3 translate;
    MopVec3 rotate;
    MopVec3 scale;
} MopGizmoDelta;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* Create a gizmo bound to a viewport.  Returns NULL on failure. */
MopGizmo *mop_gizmo_create(MopViewport *viewport);

/* Destroy a gizmo and remove any visible handles from the viewport. */
void mop_gizmo_destroy(MopGizmo *gizmo);

/* -------------------------------------------------------------------------
 * Visibility
 *
 * show  — creates handle meshes in the viewport at the given position.
 * hide  — removes handle meshes from the viewport.
 * ------------------------------------------------------------------------- */

void mop_gizmo_show(MopGizmo *gizmo, MopVec3 position);
void mop_gizmo_hide(MopGizmo *gizmo);

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

void         mop_gizmo_set_mode(MopGizmo *gizmo, MopGizmoMode mode);
MopGizmoMode mop_gizmo_get_mode(const MopGizmo *gizmo);
void         mop_gizmo_set_position(MopGizmo *gizmo, MopVec3 position);
void         mop_gizmo_set_rotation(MopGizmo *gizmo, MopVec3 rotation);

/* -------------------------------------------------------------------------
 * Picking
 *
 * Test whether a pick result hit one of this gizmo's handles.
 * Returns MOP_GIZMO_AXIS_NONE if the pick did not hit the gizmo.
 * ------------------------------------------------------------------------- */

MopGizmoAxis mop_gizmo_test_pick(const MopGizmo *gizmo, MopPickResult pick);

/* -------------------------------------------------------------------------
 * Drag
 *
 * Given the active axis and mouse delta (pixels), compute a transform
 * delta using the viewport's current camera state for screen-space
 * projection.  The application applies the returned delta to its own
 * TRS state.
 * ------------------------------------------------------------------------- */

MopGizmoDelta mop_gizmo_drag(const MopGizmo *gizmo, MopGizmoAxis axis,
                              float mouse_dx, float mouse_dy);

#endif /* MOP_GIZMO_H */
