/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * input.h — Input event system and viewport interaction
 *
 * The app maps platform events (SDL, GLFW, etc.) to MopInputEvent structs
 * and feeds them to MOP via mop_viewport_input().  MOP handles all
 * interaction logic internally: selection, gizmo, camera, click-vs-drag.
 *
 * MOP emits output events (selection changes, transform updates) that the
 * app polls via mop_viewport_poll_event() and reacts to as needed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_INPUT_H
#define MOP_INPUT_H

#include "types.h"

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Input event types — what the app sends to MOP
 * ------------------------------------------------------------------------- */

typedef enum MopInputType {
    /* Pointer (primary / left mouse) */
    MOP_INPUT_POINTER_DOWN,
    MOP_INPUT_POINTER_UP,
    MOP_INPUT_POINTER_MOVE,

    /* Secondary (right mouse) */
    MOP_INPUT_SECONDARY_DOWN,
    MOP_INPUT_SECONDARY_UP,

    /* Scroll wheel */
    MOP_INPUT_SCROLL,

    /* Gizmo mode actions */
    MOP_INPUT_MODE_TRANSLATE,
    MOP_INPUT_MODE_ROTATE,
    MOP_INPUT_MODE_SCALE,

    /* Viewport actions */
    MOP_INPUT_DESELECT,
    MOP_INPUT_TOGGLE_WIREFRAME,
    MOP_INPUT_RESET_VIEW,

    /* Undo/redo */
    MOP_INPUT_UNDO,
    MOP_INPUT_REDO,

    /* Camera movement (continuous — send each frame with magnitude) */
    MOP_INPUT_CAMERA_MOVE,

    /* Render state — use 'value' field to pass the desired state.
     * MOP updates its internal state and emits a corresponding output
     * event so the app can sync its UI. */
    MOP_INPUT_SET_SHADING,       /* value = MopShadingMode          */
    MOP_INPUT_SET_RENDER_MODE,   /* value = MopRenderMode           */
    MOP_INPUT_SET_POST_EFFECTS,  /* value = MopPostEffect bitmask   */
} MopInputType;

/* -------------------------------------------------------------------------
 * Input event — platform-agnostic representation of user input
 * ------------------------------------------------------------------------- */

typedef struct MopInputEvent {
    MopInputType type;
    float x, y;       /* absolute position (POINTER_DOWN/UP/MOVE) */
    float dx, dy;     /* relative delta (POINTER_MOVE, CAMERA_MOVE) */
    float scroll;     /* scroll amount (SCROLL, positive = zoom in) */
    uint32_t value;   /* generic payload for SET_* events            */
} MopInputEvent;

/* -------------------------------------------------------------------------
 * Output event types — what MOP tells the app
 * ------------------------------------------------------------------------- */

typedef enum MopEventType {
    MOP_EVENT_NONE = 0,
    MOP_EVENT_SELECTED,
    MOP_EVENT_DESELECTED,
    MOP_EVENT_TRANSFORM_CHANGED,
    MOP_EVENT_RENDER_MODE_CHANGED,   /* object_id = MopRenderMode      */
    MOP_EVENT_SHADING_CHANGED,       /* object_id = MopShadingMode     */
    MOP_EVENT_POST_EFFECTS_CHANGED,  /* object_id = post effect mask   */
    MOP_EVENT_LIGHT_CHANGED,         /* object_id = 0xFFFE0000 + idx   */
} MopEventType;

/* -------------------------------------------------------------------------
 * Output event — describes a state change in the viewport
 * ------------------------------------------------------------------------- */

typedef struct MopEvent {
    MopEventType type;
    uint32_t     object_id;
    MopVec3      position;
    MopVec3      rotation;
    MopVec3      scale;
} MopEvent;

/* -------------------------------------------------------------------------
 * Input processing
 *
 * Feed input events from the platform event loop.  MOP processes them
 * internally: click-vs-drag detection, object selection, gizmo
 * manipulation, camera orbit/pan/zoom.
 * ------------------------------------------------------------------------- */

void mop_viewport_input(MopViewport *vp, const MopInputEvent *event);

/* -------------------------------------------------------------------------
 * Output event polling
 *
 * After processing input, poll for events.  Returns true and fills *out
 * if an event is available, false when the queue is empty.
 * ------------------------------------------------------------------------- */

bool mop_viewport_poll_event(MopViewport *vp, MopEvent *out);

/* -------------------------------------------------------------------------
 * Selection query
 *
 * Returns the object_id of the currently selected mesh, or 0 if nothing
 * is selected.
 * ------------------------------------------------------------------------- */

uint32_t mop_viewport_get_selected(const MopViewport *vp);

#endif /* MOP_INPUT_H */
