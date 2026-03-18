/*
 * Master of Puppets — Undo/Redo
 * undo.c — Ring buffer of TRS/material/batch snapshots for undo/redo
 *
 * The undo stack is a dynamic ring buffer of undo_capacity entries
 * in the MopViewport struct.  Each entry is tagged (TRS, MATERIAL, BATCH)
 * and dispatched accordingly.  Batch entries heap-allocate sub-entries so
 * that multi-object transforms undo/redo atomically.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include <mop/mop.h>

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal: find mesh index in viewport array
 * ------------------------------------------------------------------------- */

static bool find_mesh_index(MopViewport *vp, MopMesh *mesh, uint32_t *out) {
  for (uint32_t i = 0; i < vp->mesh_count; i++) {
    if (&vp->meshes[i] == mesh) {
      *out = i;
      return true;
    }
  }
  return false;
}

/* -------------------------------------------------------------------------
 * Internal: free heap memory owned by a batch entry
 * ------------------------------------------------------------------------- */

static void free_batch_entries(MopUndoEntry *entry) {
  if (entry->type == MOP_UNDO_BATCH && entry->batch.entries) {
    free(entry->batch.entries);
    entry->batch.entries = NULL;
    entry->batch.count = 0;
  }
}

/* -------------------------------------------------------------------------
 * Internal: allocate a ring slot, discarding oldest if full
 * ------------------------------------------------------------------------- */

static int alloc_slot(MopViewport *vp) {
  int cap = (int)vp->undo_capacity;
  int slot = (vp->undo_head + vp->undo_count) % cap;

  if (vp->undo_count == cap) {
    /* Discard oldest — free batch heap memory if present */
    free_batch_entries(&vp->undo_entries[vp->undo_head]);
    vp->undo_head = (vp->undo_head + 1) % cap;
  } else {
    vp->undo_count++;
  }

  /* Free any stale redo entries that occupied this slot */
  free_batch_entries(&vp->undo_entries[slot]);

  /* Any new push invalidates redo history — free batch memory in redo zone */
  for (int i = 0; i < vp->redo_count; i++) {
    int redo_slot = (vp->undo_head + vp->undo_count + i) % cap;
    free_batch_entries(&vp->undo_entries[redo_slot]);
  }
  vp->redo_count = 0;

  return slot;
}

/* -------------------------------------------------------------------------
 * Push undo — record current TRS of the given mesh
 * ------------------------------------------------------------------------- */

void mop_viewport_push_undo(MopViewport *vp, MopMesh *mesh) {
  if (!vp || !mesh)
    return;

  uint32_t idx;
  if (!find_mesh_index(vp, mesh, &idx))
    return;

  int slot = alloc_slot(vp);
  vp->undo_entries[slot] = (MopUndoEntry){
      .type = MOP_UNDO_TRS,
      .trs = {.mesh_index = idx,
              .pos = mesh->position,
              .rot = mesh->rotation,
              .scale = mesh->scale_val},
  };
}

/* -------------------------------------------------------------------------
 * Push material undo — record current material of the given mesh
 * ------------------------------------------------------------------------- */

void mop_viewport_push_undo_material(MopViewport *vp, MopMesh *mesh) {
  if (!vp || !mesh)
    return;

  uint32_t idx;
  if (!find_mesh_index(vp, mesh, &idx))
    return;

  int slot = alloc_slot(vp);
  vp->undo_entries[slot] = (MopUndoEntry){
      .type = MOP_UNDO_MATERIAL,
      .mat = {.mesh_index = idx, .material = mesh->material},
  };
}

/* -------------------------------------------------------------------------
 * Push batch undo — record TRS of multiple meshes as one atomic entry
 * ------------------------------------------------------------------------- */

void mop_viewport_push_undo_batch(MopViewport *vp, MopMesh **meshes,
                                  uint32_t count) {
  if (!vp || !meshes || count == 0)
    return;

  MopUndoEntry *sub = malloc(count * sizeof(MopUndoEntry));
  if (!sub)
    return;

  uint32_t valid = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t idx;
    if (!find_mesh_index(vp, meshes[i], &idx))
      continue;
    sub[valid++] = (MopUndoEntry){
        .type = MOP_UNDO_TRS,
        .trs = {.mesh_index = idx,
                .pos = meshes[i]->position,
                .rot = meshes[i]->rotation,
                .scale = meshes[i]->scale_val},
    };
  }

  if (valid == 0) {
    free(sub);
    return;
  }

  int slot = alloc_slot(vp);
  vp->undo_entries[slot] = (MopUndoEntry){
      .type = MOP_UNDO_BATCH,
      .batch = {.count = valid, .entries = sub},
  };
}

/* -------------------------------------------------------------------------
 * Internal: undo/redo a single TRS entry (swap current ↔ stored)
 * ------------------------------------------------------------------------- */

static void swap_trs(MopViewport *vp, MopUndoEntry *entry) {
  if (entry->trs.mesh_index >= vp->mesh_count)
    return;
  MopMesh *mesh = &vp->meshes[entry->trs.mesh_index];
  if (!mesh->active)
    return;

  MopVec3 cur_pos = mesh->position;
  MopVec3 cur_rot = mesh->rotation;
  MopVec3 cur_scale = mesh->scale_val;

  mesh->position = entry->trs.pos;
  mesh->rotation = entry->trs.rot;
  mesh->scale_val = entry->trs.scale;
  mesh->use_trs = true;

  entry->trs.pos = cur_pos;
  entry->trs.rot = cur_rot;
  entry->trs.scale = cur_scale;
}

/* -------------------------------------------------------------------------
 * Internal: undo/redo a single material entry (swap current ↔ stored)
 * ------------------------------------------------------------------------- */

static void swap_material(MopViewport *vp, MopUndoEntry *entry) {
  if (entry->mat.mesh_index >= vp->mesh_count)
    return;
  MopMesh *mesh = &vp->meshes[entry->mat.mesh_index];
  if (!mesh->active)
    return;

  MopMaterial cur = mesh->material;
  mesh->material = entry->mat.material;
  entry->mat.material = cur;
}

/* -------------------------------------------------------------------------
 * Internal: dispatch undo/redo for any entry type
 * ------------------------------------------------------------------------- */

static void apply_entry(MopViewport *vp, MopUndoEntry *entry) {
  switch (entry->type) {
  case MOP_UNDO_TRS:
    swap_trs(vp, entry);
    break;
  case MOP_UNDO_MATERIAL:
    swap_material(vp, entry);
    break;
  case MOP_UNDO_BATCH:
    for (uint32_t i = 0; i < entry->batch.count; i++)
      apply_entry(vp, &entry->batch.entries[i]);
    break;
  }
}

/* -------------------------------------------------------------------------
 * Undo — restore the most recent snapshot
 * ------------------------------------------------------------------------- */

void mop_viewport_undo(MopViewport *vp) {
  if (!vp || vp->undo_count == 0)
    return;

  int cap = (int)vp->undo_capacity;
  vp->undo_count--;
  int slot = (vp->undo_head + vp->undo_count) % cap;

  apply_entry(vp, &vp->undo_entries[slot]);

  vp->redo_count++;
}

/* -------------------------------------------------------------------------
 * Redo — re-apply the most recently undone change
 * ------------------------------------------------------------------------- */

void mop_viewport_redo(MopViewport *vp) {
  if (!vp || vp->redo_count == 0)
    return;

  int cap = (int)vp->undo_capacity;
  int slot = (vp->undo_head + vp->undo_count) % cap;

  apply_entry(vp, &vp->undo_entries[slot]);

  vp->undo_count++;
  vp->redo_count--;
}
