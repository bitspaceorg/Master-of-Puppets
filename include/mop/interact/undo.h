/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * undo.h — Undo/redo for TRS transform operations
 *
 * The undo system records mesh TRS snapshots before gizmo manipulations.
 * Calling undo restores the previous state; redo re-applies it.
 *
 * The history is a fixed-size ring buffer of 256 entries.  Older entries
 * are silently discarded when the buffer wraps.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_INTERACT_UNDO_H
#define MOP_INTERACT_UNDO_H

#include <mop/types.h>

/* Forward declarations */
typedef struct MopViewport MopViewport;
typedef struct MopMesh MopMesh;

/* Record the current TRS state of a mesh as an undo snapshot.
 * Clears any pending redo history. */
void mop_viewport_push_undo(MopViewport *viewport, MopMesh *mesh);

/* Record the current material of a mesh for undo. */
void mop_viewport_push_undo_material(MopViewport *viewport, MopMesh *mesh);

/* Record TRS state of multiple meshes as a single batch undo entry.
 * Undoing a batch restores all meshes in one step. */
void mop_viewport_push_undo_batch(MopViewport *viewport, MopMesh **meshes,
                                  uint32_t count);

/* Undo the most recent change.  No-op if the undo stack is empty. */
void mop_viewport_undo(MopViewport *viewport);

/* Redo the most recently undone change.  No-op if nothing to redo. */
void mop_viewport_redo(MopViewport *viewport);

#endif /* MOP_INTERACT_UNDO_H */
