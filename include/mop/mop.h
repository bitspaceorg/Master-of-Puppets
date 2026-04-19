/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * mop.h — Unified public header
 *
 * Include this single header to access the full public API.
 * For finer-grained control, include individual group headers:
 *   <mop/core/core.h>         — viewport, scene, lights, materials, overlays
 *   <mop/interact/interact.h> — input, camera, gizmo, undo
 *   <mop/query/query.h>       — scene queries, camera export, spatial,
 * snapshots <mop/render/render.h>     — backend selection, picking,
 * post-processing <mop/loader/loader.h>     — OBJ and binary mesh loading
 *   <mop/util/util.h>         — logging, profiling
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_H
#define MOP_H

#include <mop/types.h>

#include <mop/core/core.h>
#include <mop/export/export.h>
#include <mop/interact/interact.h>
#include <mop/loader/loader.h>
#include <mop/query/query.h>
#include <mop/render/render.h>
#include <mop/util/util.h>

#endif /* MOP_H */
