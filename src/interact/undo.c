/*
 * Master of Puppets — Undo/Redo
 * undo.c — Ring buffer of TRS snapshots for undo/redo
 *
 * The undo stack is a ring buffer of MOP_UNDO_CAPACITY entries embedded
 * in the MopViewport struct.  push_undo records the mesh's *current* TRS
 * so that undo can restore it.  Redo re-applies the state that was undone.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include "core/viewport_internal.h"

/* -------------------------------------------------------------------------
 * Push undo — record current TRS of the given mesh
 * ------------------------------------------------------------------------- */

void mop_viewport_push_undo(MopViewport *viewport, MopMesh *mesh) {
    if (!viewport || !mesh) return;

    /* Find the mesh index in the viewport's array */
    uint32_t idx = 0;
    bool found = false;
    for (uint32_t i = 0; i < viewport->mesh_count; i++) {
        if (&viewport->meshes[i] == mesh) {
            idx = i;
            found = true;
            break;
        }
    }
    if (!found) return;

    /* Write entry at head position */
    int slot = (viewport->undo_head + viewport->undo_count) % MOP_UNDO_CAPACITY;

    /* If the buffer is full, advance head (discard oldest) */
    if (viewport->undo_count == MOP_UNDO_CAPACITY) {
        viewport->undo_head = (viewport->undo_head + 1) % MOP_UNDO_CAPACITY;
    } else {
        viewport->undo_count++;
    }

    viewport->undo_entries[slot] = (MopUndoEntry){
        .mesh_index = idx,
        .pos   = mesh->position,
        .rot   = mesh->rotation,
        .scale = mesh->scale_val
    };

    /* Any new push invalidates redo history */
    viewport->redo_count = 0;
}

/* -------------------------------------------------------------------------
 * Undo — restore the most recent TRS snapshot
 * ------------------------------------------------------------------------- */

void mop_viewport_undo(MopViewport *viewport) {
    if (!viewport || viewport->undo_count == 0) return;

    /* Pop the most recent entry */
    viewport->undo_count--;
    int slot = (viewport->undo_head + viewport->undo_count) % MOP_UNDO_CAPACITY;

    MopUndoEntry *entry = &viewport->undo_entries[slot];

    if (entry->mesh_index >= viewport->mesh_count) return;
    struct MopMesh *mesh = &viewport->meshes[entry->mesh_index];
    if (!mesh->active) return;

    /* Save the mesh's current state as the redo entry (reuse same slot) */
    MopVec3 cur_pos   = mesh->position;
    MopVec3 cur_rot   = mesh->rotation;
    MopVec3 cur_scale = mesh->scale_val;

    /* Restore from undo entry */
    mesh->position  = entry->pos;
    mesh->rotation  = entry->rot;
    mesh->scale_val = entry->scale;
    mesh->use_trs   = true;

    /* Overwrite the entry with the state we just replaced (for redo) */
    entry->pos   = cur_pos;
    entry->rot   = cur_rot;
    entry->scale = cur_scale;

    viewport->redo_count++;
}

/* -------------------------------------------------------------------------
 * Redo — re-apply the most recently undone change
 * ------------------------------------------------------------------------- */

void mop_viewport_redo(MopViewport *viewport) {
    if (!viewport || viewport->redo_count == 0) return;

    /* The redo entry sits just past the current undo stack top */
    int slot = (viewport->undo_head + viewport->undo_count) % MOP_UNDO_CAPACITY;

    MopUndoEntry *entry = &viewport->undo_entries[slot];

    if (entry->mesh_index >= viewport->mesh_count) return;
    struct MopMesh *mesh = &viewport->meshes[entry->mesh_index];
    if (!mesh->active) return;

    /* Save current state for undo */
    MopVec3 cur_pos   = mesh->position;
    MopVec3 cur_rot   = mesh->rotation;
    MopVec3 cur_scale = mesh->scale_val;

    /* Apply the redo entry */
    mesh->position  = entry->pos;
    mesh->rotation  = entry->rot;
    mesh->scale_val = entry->scale;
    mesh->use_trs   = true;

    /* Overwrite entry with what we replaced (for undo again) */
    entry->pos   = cur_pos;
    entry->rot   = cur_rot;
    entry->scale = cur_scale;

    viewport->undo_count++;
    viewport->redo_count--;
}
