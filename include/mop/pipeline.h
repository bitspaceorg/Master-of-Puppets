/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * pipeline.h — Render pipeline hooks and custom pass injection
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_PIPELINE_H
#define MOP_PIPELINE_H

#include "types.h"

/* Forward declaration */
typedef struct MopViewport MopViewport;

/* -------------------------------------------------------------------------
 * Pipeline stages — where hooks can be injected
 * ------------------------------------------------------------------------- */

typedef enum MopPipelineStage {
    MOP_STAGE_PRE_RENDER   = 0,   /* before frame_begin (scene setup) */
    MOP_STAGE_POST_CLEAR   = 1,   /* after clear, before background */
    MOP_STAGE_PRE_SCENE    = 2,   /* after background, before opaque pass */
    MOP_STAGE_POST_OPAQUE  = 3,   /* after opaque, before transparent pass */
    MOP_STAGE_POST_SCENE   = 4,   /* after all scene passes, before overlays */
    MOP_STAGE_POST_OVERLAY = 5,   /* after overlays */
    MOP_STAGE_POST_RENDER  = 6,   /* after frame_end + postprocess */
    MOP_STAGE_COUNT        = 7
} MopPipelineStage;

/* -------------------------------------------------------------------------
 * Pipeline hook callback
 * ------------------------------------------------------------------------- */

typedef void (*MopPipelineHookFn)(MopViewport *vp, void *user_data);

/* Register a hook at a pipeline stage.  Returns a handle for removal,
 * or UINT32_MAX on failure.  Multiple hooks per stage are supported
 * and execute in registration order. */
uint32_t mop_viewport_add_hook(MopViewport *vp,
                                MopPipelineStage stage,
                                MopPipelineHookFn fn,
                                void *user_data);

/* Remove a previously registered hook. */
void mop_viewport_remove_hook(MopViewport *vp, uint32_t handle);

/* -------------------------------------------------------------------------
 * Frame callback — lightweight notification for external frame sync
 *
 * A simpler alternative to pipeline hooks for consumers that just need
 * to know when a frame starts/ends (e.g. to kick off async raytracing,
 * sync game state, record frame timing).
 * ------------------------------------------------------------------------- */

typedef void (*MopFrameCallbackFn)(MopViewport *vp,
                                    bool is_pre_render,
                                    void *user_data);

void mop_viewport_set_frame_callback(MopViewport *vp,
                                      MopFrameCallbackFn fn,
                                      void *user_data);

#endif /* MOP_PIPELINE_H */
