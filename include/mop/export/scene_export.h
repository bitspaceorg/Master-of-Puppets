/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * scene_export.h — Scene state export to JSON
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_EXPORT_SCENE_EXPORT_H
#define MOP_EXPORT_SCENE_EXPORT_H

#ifdef __cplusplus
extern "C" {
#endif
/* Forward declaration */
typedef struct MopViewport MopViewport;

/* Export viewport scene state to JSON scene definition.
 * Exports camera, lights, mesh transforms, colors, and materials.
 * Returns 0 on success, -1 on failure. */
int mop_export_scene_json(const MopViewport *vp, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* MOP_EXPORT_SCENE_EXPORT_H */
