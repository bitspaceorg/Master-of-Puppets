/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * obj_export.h — Wavefront OBJ export
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_EXPORT_OBJ_EXPORT_H
#define MOP_EXPORT_OBJ_EXPORT_H

#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct MopMesh MopMesh;
typedef struct MopViewport MopViewport;

/* Export a single mesh to Wavefront OBJ.
 * Reads vertex/index data via the query API.
 * Returns 0 on success, -1 on failure. */
int mop_export_obj_mesh(const MopMesh *mesh, const MopViewport *vp,
                        const char *path);

/* Export entire viewport scene to OBJ (all active meshes).
 * Transforms baked into vertex positions via world_transform.
 * Returns 0 on success, -1 on failure. */
int mop_export_obj_scene(const MopViewport *vp, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* MOP_EXPORT_OBJ_EXPORT_H */
