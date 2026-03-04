/*
 * Master of Puppets — Conformance Framework
 * camera_paths.h — Parametric camera stress paths
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CONFORMANCE_CAMERA_PATHS_H
#define MOP_CONFORMANCE_CAMERA_PATHS_H

#include "conformance.h"

/* Get the total frame count for a path */
uint32_t mop_camera_path_frame_count(MopCameraPathId path);

/* Get camera state at frame t for the given path */
MopConfCameraState mop_camera_path_evaluate(MopCameraPathId path,
                                            uint32_t frame);

/* Get all path names */
const char *mop_camera_path_name(MopCameraPathId path);

/* For HIERARCHY_FLY: set world positions of tower nodes (from scene gen) */
void mop_camera_path_set_tower_positions(const MopVec3 *positions,
                                         uint32_t count);

#endif /* MOP_CONFORMANCE_CAMERA_PATHS_H */
