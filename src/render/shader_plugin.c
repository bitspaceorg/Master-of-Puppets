/*
 * Master of Puppets — Custom Shader Plugin System
 * shader_plugin.c — Registration, lifecycle, and draw context
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"

#include <mop/render/shader_plugin.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal plugin struct
 * ------------------------------------------------------------------------- */

struct MopShaderPlugin {
  char *name;
  MopShaderPluginStage stage;
  MopShaderDrawFn draw;
  void *user_data;
  MopRhiShader *vertex_shader;
  MopRhiShader *fragment_shader;
  MopRhiShader *compute_shader;
  bool active;
};

/* -------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

MopShaderPlugin *mop_viewport_register_shader(MopViewport *vp,
                                              const MopShaderPluginDesc *desc) {
  if (!vp || !desc || !desc->draw || !desc->name)
    return NULL;
  if ((unsigned)desc->stage >= MOP_SHADER_PLUGIN_STAGE_COUNT)
    return NULL;

  MOP_VP_LOCK(vp);
  /* Allocate plugin */
  MopShaderPlugin *plugin = calloc(1, sizeof(MopShaderPlugin));
  if (!plugin) {
    MOP_VP_UNLOCK(vp);
    return NULL;
  }

  plugin->name = strdup(desc->name);
  if (!plugin->name) {
    free(plugin);
    MOP_VP_UNLOCK(vp);
    return NULL;
  }
  plugin->stage = desc->stage;
  plugin->draw = desc->draw;
  plugin->user_data = desc->user_data;
  plugin->active = true;

  /* Compile SPIR-V into shader modules via RHI (if backend supports it) */
  const MopRhiBackend *rhi = vp->rhi;
  MopRhiDevice *dev = vp->device;
  if (rhi && dev && rhi->shader_create) {
    if (desc->vertex_spirv && desc->vertex_spirv_size >= 4) {
      plugin->vertex_shader =
          rhi->shader_create(dev, desc->vertex_spirv, desc->vertex_spirv_size);
    }
    if (desc->fragment_spirv && desc->fragment_spirv_size >= 4) {
      plugin->fragment_shader = rhi->shader_create(dev, desc->fragment_spirv,
                                                   desc->fragment_spirv_size);
    }
    if (desc->compute_spirv && desc->compute_spirv_size >= 4) {
      plugin->compute_shader = rhi->shader_create(dev, desc->compute_spirv,
                                                  desc->compute_spirv_size);
    }
  }

  /* Grow array if needed */
  if (vp->shader_plugin_count >= vp->shader_plugin_capacity) {
    if (!mop_dyn_grow((void **)&vp->shader_plugins, &vp->shader_plugin_capacity,
                      sizeof(MopShaderPlugin *), 8)) {
      /* OOM — clean up */
      if (rhi && dev && rhi->shader_destroy) {
        if (plugin->vertex_shader)
          rhi->shader_destroy(dev, plugin->vertex_shader);
        if (plugin->fragment_shader)
          rhi->shader_destroy(dev, plugin->fragment_shader);
        if (plugin->compute_shader)
          rhi->shader_destroy(dev, plugin->compute_shader);
      }
      free(plugin->name);
      free(plugin);
      MOP_VP_UNLOCK(vp);
      return NULL;
    }
  }

  vp->shader_plugins[vp->shader_plugin_count++] = plugin;
  MOP_VP_UNLOCK(vp);
  return plugin;
}

/* -------------------------------------------------------------------------
 * Unregistration
 * ------------------------------------------------------------------------- */

void mop_viewport_unregister_shader(MopViewport *vp, MopShaderPlugin *plugin) {
  if (!vp || !plugin)
    return;

  MOP_VP_LOCK(vp);
  /* Find and remove from array */
  for (uint32_t i = 0; i < vp->shader_plugin_count; i++) {
    if (vp->shader_plugins[i] == plugin) {
      /* Shift remaining entries down */
      for (uint32_t j = i; j + 1 < vp->shader_plugin_count; j++)
        vp->shader_plugins[j] = vp->shader_plugins[j + 1];
      vp->shader_plugin_count--;
      break;
    }
  }

  /* Destroy shader modules */
  const MopRhiBackend *rhi = vp->rhi;
  MopRhiDevice *dev = vp->device;
  if (rhi && dev && rhi->shader_destroy) {
    if (plugin->vertex_shader)
      rhi->shader_destroy(dev, plugin->vertex_shader);
    if (plugin->fragment_shader)
      rhi->shader_destroy(dev, plugin->fragment_shader);
    if (plugin->compute_shader)
      rhi->shader_destroy(dev, plugin->compute_shader);
  }
  free(plugin->name);
  free(plugin);
  MOP_VP_UNLOCK(vp);
}

/* -------------------------------------------------------------------------
 * Destroy all plugins — called from mop_viewport_destroy
 * ------------------------------------------------------------------------- */

void mop_shader_plugins_destroy_all(MopViewport *vp) {
  if (!vp)
    return;
  /* Destroy in reverse order */
  while (vp->shader_plugin_count > 0) {
    MopShaderPlugin *p = vp->shader_plugins[vp->shader_plugin_count - 1];
    mop_viewport_unregister_shader(vp, p);
  }
  free(vp->shader_plugins);
  vp->shader_plugins = NULL;
  vp->shader_plugin_count = 0;
  vp->shader_plugin_capacity = 0;
}

/* -------------------------------------------------------------------------
 * Dispatch — execute all plugins at a given stage
 *
 * Called from viewport.c render graph node wrappers.
 * ------------------------------------------------------------------------- */

void mop_shader_plugins_dispatch(MopViewport *vp, MopShaderPluginStage stage) {
  if (!vp)
    return;

  /* Build draw context */
  MopShaderDrawContext ctx = {
      .view_matrix = vp->view_matrix,
      .projection_matrix = vp->projection_matrix,
      .camera_eye = vp->cam_eye,
      .camera_target = vp->cam_target,
      .width = vp->width,
      .height = vp->height,
      .time = vp->last_frame_time,
      .delta_time = vp->last_frame_time - vp->prev_frame_time,
      .rhi_device = vp->device,
  };

  for (uint32_t i = 0; i < vp->shader_plugin_count; i++) {
    MopShaderPlugin *p = vp->shader_plugins[i];
    if (p && p->active && p->stage == stage && p->draw)
      p->draw(&ctx, p->user_data);
  }
}

/* -------------------------------------------------------------------------
 * Accessors
 * ------------------------------------------------------------------------- */

const char *mop_shader_plugin_get_name(const MopShaderPlugin *plugin) {
  return plugin ? plugin->name : NULL;
}

MopRhiShader *mop_shader_plugin_get_vertex(const MopShaderPlugin *plugin) {
  return plugin ? plugin->vertex_shader : NULL;
}

MopRhiShader *mop_shader_plugin_get_fragment(const MopShaderPlugin *plugin) {
  return plugin ? plugin->fragment_shader : NULL;
}

MopRhiShader *mop_shader_plugin_get_compute(const MopShaderPlugin *plugin) {
  return plugin ? plugin->compute_shader : NULL;
}
