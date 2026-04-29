/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * selection.h — Per-mesh edit mode and sub-element selection
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_INTERACT_SELECTION_H
#define MOP_INTERACT_SELECTION_H

#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MopViewport MopViewport;
typedef struct MopMesh MopMesh;

/* Per-mesh edit mode — default NONE means object-level only */
typedef enum MopEditMode {
  MOP_EDIT_NONE = 0,   /* object mode (current behavior) */
  MOP_EDIT_VERTEX = 1, /* vertex edit mode */
  MOP_EDIT_EDGE = 2,   /* edge edit mode */
  MOP_EDIT_FACE = 3,   /* face edit mode */
} MopEditMode;

typedef struct MopSelection {
  MopEditMode mode;
  uint32_t mesh_object_id; /* which mesh we're editing */
  uint32_t *elements;
  uint32_t element_count;
  uint32_t element_capacity;
} MopSelection;

/* Enter/exit edit mode on a mesh */
void mop_mesh_set_edit_mode(MopMesh *mesh, MopEditMode mode);
MopEditMode mop_mesh_get_edit_mode(const MopMesh *mesh);

/* Sub-element selection API */
const MopSelection *mop_viewport_get_selection(const MopViewport *vp);
void mop_viewport_select_element(MopViewport *vp, uint32_t element_index);
void mop_viewport_deselect_element(MopViewport *vp, uint32_t element_index);
void mop_viewport_clear_selection(MopViewport *vp);
void mop_viewport_toggle_element(MopViewport *vp, uint32_t element_index);

/* Multi-object selection API */
void mop_viewport_select_object(MopViewport *vp, uint32_t id, bool additive);
void mop_viewport_deselect_object(MopViewport *vp, uint32_t id);
bool mop_viewport_is_object_selected(const MopViewport *vp, uint32_t id);
uint32_t mop_viewport_get_selected_count(const MopViewport *vp);

#ifdef __cplusplus
}
#endif

#endif /* MOP_INTERACT_SELECTION_H */
