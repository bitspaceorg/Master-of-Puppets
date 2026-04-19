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

#include "core/viewport_internal.h"
#include <mop/interact/camera.h>
#include <mop/mop.h>

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define CLICK_THRESHOLD 5.0f
#define ORBIT_SENSITIVITY 0.005f
#define MIN_SCALE 0.05f

/* -------------------------------------------------------------------------
 * Event queue helpers
 * ------------------------------------------------------------------------- */

static void push_event(MopViewport *vp, MopEvent ev) {
  int next = (vp->event_tail + 1) % (int)vp->event_capacity;
  if (next == vp->event_head) {
    MOP_WARN("event queue full, dropping event");
    return; /* queue full — drop newest event */
  }
  vp->events[vp->event_tail] = ev;
  vp->event_tail = next;
}

/* -------------------------------------------------------------------------
 * Selection helpers
 * ------------------------------------------------------------------------- */

/* Find the MopMesh by object_id in the viewport's mesh array */
static MopMesh *find_mesh_by_id(MopViewport *vp, uint32_t object_id) {
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    MopMesh *m = vp->meshes[i];
    if (m->active && m->object_id == object_id)
      return m;
  }
  return NULL;
}

/* Is this object_id a gizmo handle? (not a scene object) */
static bool is_gizmo_handle(uint32_t id) { return id >= 0xFFFF0000u; }

/* Is this object_id a light indicator? */
static bool is_light_indicator(uint32_t id) {
  return id >= 0xFFFE0000u && id < 0xFFFF0000u;
}

/* Find a camera object by its object_id.  Returns NULL if not found. */
static MopCameraObject *find_camera_by_id(MopViewport *vp, uint32_t id) {
  for (uint32_t i = 0; i < vp->camera_count; i++) {
    if (vp->cameras[i].active && vp->cameras[i].object_id == id)
      return &vp->cameras[i];
  }
  return NULL;
}

static bool is_camera_object(MopViewport *vp, uint32_t id) {
  return find_camera_by_id(vp, id) != NULL;
}

static uint32_t light_index_from_id(uint32_t id) { return id - 0xFFFE0000u; }

/* Compute centroid of all selected meshes for gizmo placement */
static MopVec3 compute_selection_centroid(MopViewport *vp) {
  MopVec3 sum = {0, 0, 0};
  uint32_t count = 0;
  for (uint32_t i = 0; i < vp->selected_count; i++) {
    if (is_light_indicator(vp->selected_ids[i]))
      continue;
    MopMesh *m = find_mesh_by_id(vp, vp->selected_ids[i]);
    if (m) {
      sum = mop_vec3_add(sum, m->position);
      count++;
    }
  }
  if (count > 0)
    return mop_vec3_scale(sum, 1.0f / (float)count);
  return sum;
}

static void select_object(MopViewport *vp, uint32_t object_id, bool additive) {
  mop_viewport_select_object(vp, object_id, additive);

  if (is_light_indicator(object_id)) {
    /* Light indicator selected — show gizmo at light position */
    uint32_t li = light_index_from_id(object_id);
    if (li < vp->light_count && vp->lights[li].active) {
      MopVec3 pos = vp->lights[li].position;
      if (vp->lights[li].type == MOP_LIGHT_DIRECTIONAL) {
        MopVec3 dir = mop_vec3_normalize(vp->lights[li].direction);
        pos = mop_vec3_add(vp->cam_target, mop_vec3_scale(dir, 3.0f));
      }
      mop_gizmo_set_mode(vp->gizmo, MOP_GIZMO_TRANSLATE);
      mop_gizmo_show(vp->gizmo, pos, NULL);
      mop_gizmo_set_rotation(vp->gizmo, (MopVec3){0, 0, 0});
    }
    push_event(vp, (MopEvent){
                       .type = MOP_EVENT_SELECTED,
                       .object_id = object_id,
                   });
  } else if (is_camera_object(vp, object_id)) {
    /* Camera object selected — show gizmo at camera position */
    MopCameraObject *cam = find_camera_by_id(vp, object_id);
    if (cam) {
      mop_gizmo_set_mode(vp->gizmo, MOP_GIZMO_TRANSLATE);
      mop_gizmo_show(vp->gizmo, cam->position, NULL);
      mop_gizmo_set_rotation(vp->gizmo, (MopVec3){0, 0, 0});
    }
    push_event(vp, (MopEvent){
                       .type = MOP_EVENT_SELECTED,
                       .object_id = object_id,
                       .position = cam ? cam->position : (MopVec3){0, 0, 0},
                   });
  } else {
    /* Regular mesh — show gizmo at centroid of all selected */
    MopMesh *mesh = find_mesh_by_id(vp, object_id);
    if (vp->selected_count == 1 && mesh) {
      mop_gizmo_show(vp->gizmo, mesh->position, mesh);
      mop_gizmo_set_rotation(vp->gizmo, mesh->rotation);
    } else if (vp->selected_count > 1) {
      MopVec3 centroid = compute_selection_centroid(vp);
      mop_gizmo_show(vp->gizmo, centroid, NULL);
      mop_gizmo_set_rotation(vp->gizmo, (MopVec3){0, 0, 0});
    }

    push_event(
        vp, (MopEvent){.type = MOP_EVENT_SELECTED,
                       .object_id = object_id,
                       .position = mesh ? mesh->position : (MopVec3){0, 0, 0},
                       .rotation = mesh ? mesh->rotation : (MopVec3){0, 0, 0},
                       .scale = mesh ? mesh->scale_val : (MopVec3){1, 1, 1}});
  }
}

static void deselect(MopViewport *vp) {
  if (!vp->selected_count)
    return;

  uint32_t old_id = vp->selected_id;
  vp->selected_count = 0;
  vp->selected_id = 0;
  mop_gizmo_hide(vp->gizmo);

  push_event(vp, (MopEvent){.type = MOP_EVENT_DESELECTED, .object_id = old_id});
}

/* -------------------------------------------------------------------------
 * Input processing — the state machine
 * ------------------------------------------------------------------------- */

void mop_viewport_input(MopViewport *vp, const MopInputEvent *event) {
  if (!vp || !event)
    return;

  MOP_VP_LOCK(vp);
  switch (event->type) {

  /* ----- Pointer down ----- */
  case MOP_INPUT_POINTER_DOWN: {
    /* Test gizmo pick — defer drag until threshold is reached.
     * This lets the app delay relative-mouse-mode (which breaks
     * gizmo coords) until we know it's an orbit, not a gizmo drag. */
    MopPickResult p = mop_viewport_pick(vp, (int)event->x, (int)event->y);
    MopGizmoAxis axis = mop_gizmo_test_pick(vp->gizmo, p);

    vp->interact_state = MOP_INTERACT_CLICK_PENDING;
    vp->click_start_x = event->x;
    vp->click_start_y = event->y;
    vp->pending_gizmo_axis = axis;
    break;
  }

  /* ----- Pointer up ----- */
  case MOP_INPUT_POINTER_UP: {
    /* End of gizmo drag — push undo for all selected meshes */
    if (vp->interact_state == MOP_INTERACT_GIZMO_DRAG &&
        vp->selected_count > 0) {
      /* Collect non-light-indicator meshes that were moved */
      MopMesh **moved = malloc(vp->selected_count * sizeof(MopMesh *));
      if (!moved)
        break;
      uint32_t moved_count = 0;
      for (uint32_t si = 0; si < vp->selected_count; si++) {
        if (is_light_indicator(vp->selected_ids[si]))
          continue;
        MopMesh *m = find_mesh_by_id(vp, vp->selected_ids[si]);
        if (m)
          moved[moved_count++] = m;
      }
      if (moved_count == 1) {
        mop_viewport_push_undo(vp, moved[0]);
      } else if (moved_count > 1) {
        mop_viewport_push_undo_batch(vp, moved, moved_count);
      }
      free(moved);
    }

    if (vp->interact_state == MOP_INTERACT_CLICK_PENDING) {
      /* Check axis indicator click — highest priority, snaps camera */
      int axis_hit = mop_viewport_pick_axis_indicator(vp, event->x, event->y);
      if (axis_hit > 0) {
        mop_orbit_camera_snap_to_view(&vp->camera, (MopViewAxis)(axis_hit - 1));
        vp->interact_state = MOP_INTERACT_IDLE;
        vp->drag_axis = MOP_GIZMO_AXIS_NONE;
        break;
      }

      /* Mouse barely moved — this is a click */
      MopPickResult p = mop_viewport_pick(vp, (int)event->x, (int)event->y);
      MopGizmoAxis axis = mop_gizmo_test_pick(vp->gizmo, p);

      if (axis != MOP_GIZMO_AXIS_NONE) {
        /* Clicked gizmo handle (no drag) — ignore */
      } else if (p.hit && p.object_id > 0 && !is_gizmo_handle(p.object_id)) {
        /* Select scene object or light indicator (shift = additive) */
        bool additive = (event->modifiers & MOP_MOD_SHIFT) != 0;
        select_object(vp, p.object_id, additive);
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
      if (dx * dx + dy * dy > CLICK_THRESHOLD * CLICK_THRESHOLD) {
        if (vp->pending_gizmo_axis != MOP_GIZMO_AXIS_NONE) {
          /* User dragged after clicking on a gizmo handle → gizmo drag */
          vp->interact_state = MOP_INTERACT_GIZMO_DRAG;
          vp->drag_axis = vp->pending_gizmo_axis;
        } else {
          vp->interact_state = MOP_INTERACT_ORBITING;
        }
      }
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
      if (!vp->selected_id)
        break;

      if (is_light_indicator(vp->selected_id)) {
        /* Dragging a light indicator — update the light */
        uint32_t li = light_index_from_id(vp->selected_id);
        if (li >= vp->light_count || !vp->lights[li].active)
          break;

        MopGizmoDelta d =
            mop_gizmo_drag(vp->gizmo, vp->drag_axis, event->dx, event->dy);

        MopLight *light = &vp->lights[li];
        if (light->type == MOP_LIGHT_DIRECTIONAL) {
          /* Directional: translate the virtual indicator position,
           * then derive direction = normalize(pos - cam_target).
           * The indicator sits at cam_target + dir * 3, so moving
           * it around the target sphere changes the direction. */
          MopVec3 cur_dir = mop_vec3_normalize(light->direction);
          MopVec3 cur_pos =
              mop_vec3_add(vp->cam_target, mop_vec3_scale(cur_dir, 3.0f));
          MopVec3 new_pos = mop_vec3_add(cur_pos, d.translate);
          MopVec3 new_dir = mop_vec3_sub(new_pos, vp->cam_target);
          float len = mop_vec3_length(new_dir);
          if (len > 0.01f) {
            mop_light_set_direction(light, mop_vec3_scale(new_dir, 1.0f / len));
          }
          /* Snap indicator back to the sphere at radius 3 */
          MopVec3 snapped = mop_vec3_add(
              vp->cam_target,
              mop_vec3_scale(mop_vec3_normalize(light->direction), 3.0f));
          mop_gizmo_set_position(vp->gizmo, snapped);
        } else {
          /* Point/spot: translate the position directly */
          MopVec3 new_pos = mop_vec3_add(light->position, d.translate);
          mop_light_set_position(light, new_pos);
          mop_gizmo_set_position(vp->gizmo, new_pos);

          /* For spot lights, also update direction if needed */
        }

        push_event(vp, (MopEvent){
                           .type = MOP_EVENT_LIGHT_CHANGED,
                           .object_id = vp->selected_id,
                           .position = light->position,
                       });
      } else if (is_camera_object(vp, vp->selected_id)) {
        /* Dragging a camera object — update its position */
        MopGizmoDelta d =
            mop_gizmo_drag(vp->gizmo, vp->drag_axis, event->dx, event->dy);
        MopCameraObject *cam = find_camera_by_id(vp, vp->selected_id);
        if (cam) {
          cam->position = mop_vec3_add(cam->position, d.translate);
          cam->target = mop_vec3_add(cam->target, d.translate);
          mop_gizmo_set_position(vp->gizmo, cam->position);
        }
        push_event(vp, (MopEvent){.type = MOP_EVENT_TRANSFORM_CHANGED,
                                  .object_id = vp->selected_id,
                                  .position = cam ? cam->position
                                                  : (MopVec3){0, 0, 0}});
      } else {
        /* Regular mesh drag — apply to ALL selected meshes */
        MopGizmoDelta d =
            mop_gizmo_drag(vp->gizmo, vp->drag_axis, event->dx, event->dy);

        for (uint32_t si = 0; si < vp->selected_count; si++) {
          if (is_light_indicator(vp->selected_ids[si]))
            continue;
          MopMesh *mesh = find_mesh_by_id(vp, vp->selected_ids[si]);
          if (!mesh)
            continue;

          mesh->position = mop_vec3_add(mesh->position, d.translate);
          mesh->rotation = mop_vec3_add(mesh->rotation, d.rotate);
          mesh->scale_val = mop_vec3_add(mesh->scale_val, d.scale);

          if (mesh->scale_val.x < MIN_SCALE)
            mesh->scale_val.x = MIN_SCALE;
          if (mesh->scale_val.y < MIN_SCALE)
            mesh->scale_val.y = MIN_SCALE;
          if (mesh->scale_val.z < MIN_SCALE)
            mesh->scale_val.z = MIN_SCALE;

          mesh->use_trs = true;
        }

        /* Update gizmo position to centroid */
        MopVec3 centroid = compute_selection_centroid(vp);
        mop_gizmo_set_position(vp->gizmo, centroid);
        if (vp->selected_count == 1) {
          MopMesh *m = find_mesh_by_id(vp, vp->selected_ids[0]);
          if (m)
            mop_gizmo_set_rotation(vp->gizmo, m->rotation);
        }

        push_event(vp, (MopEvent){.type = MOP_EVENT_TRANSFORM_CHANGED,
                                  .object_id = vp->selected_id,
                                  .position = centroid});
      }
      break;
    }

    default:
      break;
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

  case MOP_INPUT_SCROLL_ORBIT:
    mop_orbit_camera_orbit(&vp->camera, event->dx, event->dy,
                           ORBIT_SENSITIVITY);
    break;

  /* ----- Gizmo mode actions ----- */
  case MOP_INPUT_MODE_TRANSLATE:
    if (vp->selected_id)
      mop_gizmo_set_mode(vp->gizmo, MOP_GIZMO_TRANSLATE);
    break;

  case MOP_INPUT_MODE_ROTATE:
    /* Ignore mode switch for light indicators — only translate allowed */
    if (vp->selected_id && !is_light_indicator(vp->selected_id))
      mop_gizmo_set_mode(vp->gizmo, MOP_GIZMO_ROTATE);
    break;

  case MOP_INPUT_MODE_SCALE:
    /* Ignore mode switch for light indicators — only translate allowed */
    if (vp->selected_id && !is_light_indicator(vp->selected_id))
      mop_gizmo_set_mode(vp->gizmo, MOP_GIZMO_SCALE);
    break;

  /* ----- Viewport actions ----- */
  case MOP_INPUT_DESELECT:
    deselect(vp);
    break;

  case MOP_INPUT_TOGGLE_WIREFRAME: {
    MopRenderMode old = vp->render_mode;
    vp->render_mode =
        (old == MOP_RENDER_WIREFRAME) ? MOP_RENDER_SOLID : MOP_RENDER_WIREFRAME;
    if (vp->render_mode != old)
      push_event(vp, (MopEvent){.type = MOP_EVENT_RENDER_MODE_CHANGED,
                                .object_id = (uint32_t)vp->render_mode});
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
      push_event(vp, (MopEvent){.type = MOP_EVENT_SHADING_CHANGED,
                                .object_id = event->value});
    }
    break;
  }

  case MOP_INPUT_SET_RENDER_MODE: {
    MopRenderMode mode = (MopRenderMode)event->value;
    if (vp->render_mode != mode) {
      vp->render_mode = mode;
      push_event(vp, (MopEvent){.type = MOP_EVENT_RENDER_MODE_CHANGED,
                                .object_id = event->value});
    }
    break;
  }

  case MOP_INPUT_SET_POST_EFFECTS: {
    uint32_t effects = event->value;
    if (vp->post_effects != effects) {
      vp->post_effects = effects;
      push_event(vp, (MopEvent){.type = MOP_EVENT_POST_EFFECTS_CHANGED,
                                .object_id = effects});
    }
    break;
  }

  /* Edit mode switching (Phase 3) */
  case MOP_INPUT_EDIT_MODE_NONE:
  case MOP_INPUT_EDIT_MODE_VERTEX:
  case MOP_INPUT_EDIT_MODE_EDGE:
  case MOP_INPUT_EDIT_MODE_FACE:
    break;
  }
  MOP_VP_UNLOCK(vp);
}

/* -------------------------------------------------------------------------
 * Output event polling
 * ------------------------------------------------------------------------- */

bool mop_viewport_poll_event(MopViewport *vp, MopEvent *out) {
  if (!vp || !out)
    return false;
  MOP_VP_LOCK(vp);
  if (vp->event_head == vp->event_tail) {
    MOP_VP_UNLOCK(vp);
    return false;
  }

  *out = vp->events[vp->event_head];
  vp->event_head = (vp->event_head + 1) % (int)vp->event_capacity;
  MOP_VP_UNLOCK(vp);
  return true;
}

/* -------------------------------------------------------------------------
 * Selection query
 * ------------------------------------------------------------------------- */

uint32_t mop_viewport_get_selected(const MopViewport *vp) {
  return vp ? vp->selected_id : 0;
}
