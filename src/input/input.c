/*
 * Master of Puppets — Input Controller
 * input.c — Interaction state machine, selection, gizmo, camera
 *
 * All interaction logic lives here.  The application feeds platform events
 * as MopInputEvent structs; this module processes them and emits MopEvent
 * output events that the application polls.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include "viewport/viewport_internal.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define CLICK_THRESHOLD   5.0f
#define ORBIT_SENSITIVITY 0.005f
#define MIN_SCALE         0.05f

/* -------------------------------------------------------------------------
 * Event queue helpers
 * ------------------------------------------------------------------------- */

static void push_event(MopViewport *vp, MopEvent ev) {
    int next = (vp->event_tail + 1) % MOP_MAX_EVENTS;
    if (next == vp->event_head) return;   /* queue full — drop oldest */
    vp->events[vp->event_tail] = ev;
    vp->event_tail = next;
}

/* -------------------------------------------------------------------------
 * Selection helpers
 * ------------------------------------------------------------------------- */

/* Find the MopMesh by object_id in the viewport's mesh array */
static MopMesh *find_mesh_by_id(MopViewport *vp, uint32_t object_id) {
    for (uint32_t i = 0; i < vp->mesh_count; i++) {
        if (vp->meshes[i].active && vp->meshes[i].object_id == object_id)
            return &vp->meshes[i];
    }
    return NULL;
}

/* Is this object_id a gizmo handle? (not a scene object) */
static bool is_gizmo_handle(uint32_t id) {
    return id >= 0xFFFF0000u;
}

static void select_object(MopViewport *vp, uint32_t object_id) {
    if (vp->selected_id == object_id) return;

    vp->selected_id = object_id;

    MopMesh *mesh = find_mesh_by_id(vp, object_id);
    if (mesh) {
        mop_gizmo_show(vp->gizmo, mesh->position, mesh);
        mop_gizmo_set_rotation(vp->gizmo, mesh->rotation);
    }

    push_event(vp, (MopEvent){
        .type = MOP_EVENT_SELECTED,
        .object_id = object_id,
        .position = mesh ? mesh->position : (MopVec3){0,0,0},
        .rotation = mesh ? mesh->rotation : (MopVec3){0,0,0},
        .scale    = mesh ? mesh->scale_val : (MopVec3){1,1,1}
    });
}

static void deselect(MopViewport *vp) {
    if (!vp->selected_id) return;

    uint32_t old_id = vp->selected_id;
    vp->selected_id = 0;
    mop_gizmo_hide(vp->gizmo);

    push_event(vp, (MopEvent){
        .type = MOP_EVENT_DESELECTED,
        .object_id = old_id
    });
}

/* -------------------------------------------------------------------------
 * Input processing — the state machine
 * ------------------------------------------------------------------------- */

void mop_viewport_input(MopViewport *vp, const MopInputEvent *event) {
    if (!vp || !event) return;

    switch (event->type) {

    /* ----- Pointer down ----- */
    case MOP_INPUT_POINTER_DOWN: {
        /* Test gizmo pick first */
        MopPickResult p = mop_viewport_pick(vp, (int)event->x, (int)event->y);
        MopGizmoAxis axis = mop_gizmo_test_pick(vp->gizmo, p);

        if (axis != MOP_GIZMO_AXIS_NONE) {
            vp->interact_state = MOP_INTERACT_GIZMO_DRAG;
            vp->drag_axis = axis;
        } else {
            vp->interact_state = MOP_INTERACT_CLICK_PENDING;
            vp->click_start_x = event->x;
            vp->click_start_y = event->y;
        }
        break;
    }

    /* ----- Pointer up ----- */
    case MOP_INPUT_POINTER_UP: {
        /* End of gizmo drag — push undo entry */
        if (vp->interact_state == MOP_INTERACT_GIZMO_DRAG && vp->selected_id) {
            MopMesh *mesh = find_mesh_by_id(vp, vp->selected_id);
            if (mesh) {
                mop_viewport_push_undo(vp, mesh);
            }
        }

        if (vp->interact_state == MOP_INTERACT_CLICK_PENDING) {
            /* Mouse barely moved — this is a click */
            MopPickResult p = mop_viewport_pick(vp, (int)event->x,
                                                (int)event->y);
            MopGizmoAxis axis = mop_gizmo_test_pick(vp->gizmo, p);

            if (axis != MOP_GIZMO_AXIS_NONE) {
                /* Clicked gizmo handle (no drag) — ignore */
            } else if (p.hit && p.object_id > 0
                       && !is_gizmo_handle(p.object_id)) {
                /* Select scene object */
                select_object(vp, p.object_id);
            } else {
                /* Clicked empty — deselect */
                deselect(vp);
            }
        }
        /* Return to idle from any pointer state */
        if (vp->interact_state != MOP_INTERACT_PANNING)
            vp->interact_state = MOP_INTERACT_IDLE;
        vp->drag_axis = MOP_GIZMO_AXIS_NONE;
        break;
    }

    /* ----- Pointer move ----- */
    case MOP_INPUT_POINTER_MOVE: {
        switch (vp->interact_state) {

        case MOP_INTERACT_CLICK_PENDING: {
            float dx = event->x - vp->click_start_x;
            float dy = event->y - vp->click_start_y;
            if (dx*dx + dy*dy > CLICK_THRESHOLD * CLICK_THRESHOLD)
                vp->interact_state = MOP_INTERACT_ORBITING;
            break;
        }

        case MOP_INTERACT_ORBITING:
            mop_orbit_camera_orbit(&vp->camera, event->dx, event->dy,
                                   ORBIT_SENSITIVITY);
            break;

        case MOP_INTERACT_PANNING:
            mop_orbit_camera_pan(&vp->camera, event->dx, event->dy);
            break;

        case MOP_INTERACT_GIZMO_DRAG: {
            if (!vp->selected_id) break;
            MopMesh *mesh = find_mesh_by_id(vp, vp->selected_id);
            if (!mesh) break;

            MopGizmoDelta d = mop_gizmo_drag(vp->gizmo, vp->drag_axis,
                                             event->dx, event->dy);

            mesh->position  = mop_vec3_add(mesh->position, d.translate);
            mesh->rotation  = mop_vec3_add(mesh->rotation, d.rotate);
            mesh->scale_val = mop_vec3_add(mesh->scale_val, d.scale);

            /* Clamp scale to minimum */
            if (mesh->scale_val.x < MIN_SCALE) mesh->scale_val.x = MIN_SCALE;
            if (mesh->scale_val.y < MIN_SCALE) mesh->scale_val.y = MIN_SCALE;
            if (mesh->scale_val.z < MIN_SCALE) mesh->scale_val.z = MIN_SCALE;

            mesh->use_trs = true;

            mop_gizmo_set_position(vp->gizmo, mesh->position);
            mop_gizmo_set_rotation(vp->gizmo, mesh->rotation);

            push_event(vp, (MopEvent){
                .type = MOP_EVENT_TRANSFORM_CHANGED,
                .object_id = vp->selected_id,
                .position  = mesh->position,
                .rotation  = mesh->rotation,
                .scale     = mesh->scale_val
            });
            break;
        }

        default: break;
        }
        break;
    }

    /* ----- Secondary (right mouse) ----- */
    case MOP_INPUT_SECONDARY_DOWN:
        vp->interact_state = MOP_INTERACT_PANNING;
        break;

    case MOP_INPUT_SECONDARY_UP:
        if (vp->interact_state == MOP_INTERACT_PANNING)
            vp->interact_state = MOP_INTERACT_IDLE;
        break;

    /* ----- Scroll ----- */
    case MOP_INPUT_SCROLL:
        mop_orbit_camera_zoom(&vp->camera, event->scroll);
        break;

    /* ----- Gizmo mode actions ----- */
    case MOP_INPUT_MODE_TRANSLATE:
        if (vp->selected_id)
            mop_gizmo_set_mode(vp->gizmo, MOP_GIZMO_TRANSLATE);
        break;

    case MOP_INPUT_MODE_ROTATE:
        if (vp->selected_id)
            mop_gizmo_set_mode(vp->gizmo, MOP_GIZMO_ROTATE);
        break;

    case MOP_INPUT_MODE_SCALE:
        if (vp->selected_id)
            mop_gizmo_set_mode(vp->gizmo, MOP_GIZMO_SCALE);
        break;

    /* ----- Viewport actions ----- */
    case MOP_INPUT_DESELECT:
        deselect(vp);
        break;

    case MOP_INPUT_TOGGLE_WIREFRAME: {
        MopRenderMode old = vp->render_mode;
        vp->render_mode = (old == MOP_RENDER_WIREFRAME)
            ? MOP_RENDER_SOLID : MOP_RENDER_WIREFRAME;
        if (vp->render_mode != old)
            push_event(vp, (MopEvent){
                .type = MOP_EVENT_RENDER_MODE_CHANGED,
                .object_id = (uint32_t)vp->render_mode
            });
        break;
    }

    case MOP_INPUT_RESET_VIEW:
        deselect(vp);
        vp->camera = mop_orbit_camera_default();
        break;

    /* ----- Undo / Redo ----- */
    case MOP_INPUT_UNDO:
        mop_viewport_undo(vp);
        break;

    case MOP_INPUT_REDO:
        mop_viewport_redo(vp);
        break;

    /* ----- Camera movement (continuous) ----- */
    case MOP_INPUT_CAMERA_MOVE:
        mop_orbit_camera_move(&vp->camera, event->dy, event->dx);
        break;

    /* ----- Render state SET events ----- */
    case MOP_INPUT_SET_SHADING: {
        MopShadingMode mode = (MopShadingMode)event->value;
        if (vp->shading_mode != mode) {
            vp->shading_mode = mode;
            push_event(vp, (MopEvent){
                .type = MOP_EVENT_SHADING_CHANGED,
                .object_id = event->value
            });
        }
        break;
    }

    case MOP_INPUT_SET_RENDER_MODE: {
        MopRenderMode mode = (MopRenderMode)event->value;
        if (vp->render_mode != mode) {
            vp->render_mode = mode;
            push_event(vp, (MopEvent){
                .type = MOP_EVENT_RENDER_MODE_CHANGED,
                .object_id = event->value
            });
        }
        break;
    }

    case MOP_INPUT_SET_POST_EFFECTS: {
        uint32_t effects = event->value;
        if (vp->post_effects != effects) {
            vp->post_effects = effects;
            push_event(vp, (MopEvent){
                .type = MOP_EVENT_POST_EFFECTS_CHANGED,
                .object_id = effects
            });
        }
        break;
    }
    }
}

/* -------------------------------------------------------------------------
 * Output event polling
 * ------------------------------------------------------------------------- */

bool mop_viewport_poll_event(MopViewport *vp, MopEvent *out) {
    if (!vp || !out) return false;
    if (vp->event_head == vp->event_tail) return false;

    *out = vp->events[vp->event_head];
    vp->event_head = (vp->event_head + 1) % MOP_MAX_EVENTS;
    return true;
}

/* -------------------------------------------------------------------------
 * Selection query
 * ------------------------------------------------------------------------- */

uint32_t mop_viewport_get_selected(const MopViewport *vp) {
    return vp ? vp->selected_id : 0;
}
