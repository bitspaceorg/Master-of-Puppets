/*
 * Master of Puppets — Core
 * decal.c — Deferred decal projection API implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "viewport_internal.h"
#include <mop/render/decal.h>

int32_t mop_viewport_add_decal(MopViewport *vp, const MopDecalDesc *desc) {
  if (!vp || !desc)
    return -1;
  if (!vp->rhi || !vp->device || !vp->rhi->add_decal)
    return -1;
  return vp->rhi->add_decal(vp->device, desc->transform.d, desc->opacity,
                            desc->texture_idx);
}

void mop_viewport_remove_decal(MopViewport *vp, int32_t decal_id) {
  if (!vp || decal_id < 0)
    return;
  if (!vp->rhi || !vp->device || !vp->rhi->remove_decal)
    return;
  vp->rhi->remove_decal(vp->device, decal_id);
}

void mop_viewport_clear_decals(MopViewport *vp) {
  if (!vp)
    return;
  if (!vp->rhi || !vp->device || !vp->rhi->clear_decals)
    return;
  vp->rhi->clear_decals(vp->device);
}
