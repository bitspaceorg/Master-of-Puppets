/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * shader_plugin.h — Custom shader plugin registration and execution
 *
 * Allows host applications to inject custom rendering passes (VFX shaders,
 * post-process effects, volume rendering, overlays) into the MOP pipeline.
 * Each plugin registers SPIR-V bytecode and a draw callback; MOP creates
 * the shader modules and invokes the callback at the designated stage.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_RENDER_SHADER_PLUGIN_H
#define MOP_RENDER_SHADER_PLUGIN_H

#include <mop/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct MopViewport MopViewport;
typedef struct MopRhiShader MopRhiShader;

/* -------------------------------------------------------------------------
 * Pipeline stage — where in the render graph the plugin executes
 * ------------------------------------------------------------------------- */

typedef enum MopShaderPluginStage {
  MOP_SHADER_PLUGIN_POST_OPAQUE = 0,  /* After opaque, before transparent */
  MOP_SHADER_PLUGIN_POST_SCENE = 1,   /* After all scene geometry          */
  MOP_SHADER_PLUGIN_POST_PROCESS = 2, /* After frame_end (post-FX chain)   */
  MOP_SHADER_PLUGIN_OVERLAY = 3,      /* During overlay pass               */
  MOP_SHADER_PLUGIN_STAGE_COUNT
} MopShaderPluginStage;

/* -------------------------------------------------------------------------
 * Draw context — passed to the plugin's draw callback each frame
 *
 * Provides camera state, framebuffer dimensions, timing, and an RHI
 * device handle for advanced usage (submitting draw calls, binding
 * custom descriptors, etc.).
 * ------------------------------------------------------------------------- */

typedef struct MopShaderDrawContext {
  MopMat4 view_matrix;
  MopMat4 projection_matrix;
  MopVec3 camera_eye;
  MopVec3 camera_target;
  int width;
  int height;
  float time;
  float delta_time;
  /* RHI device handle — for direct backend calls (Vulkan command
   * recording, etc.).  NULL on the CPU backend. */
  void *rhi_device;
} MopShaderDrawContext;

/* -------------------------------------------------------------------------
 * Draw callback — invoked by MOP during render graph execution
 *
 * The plugin receives the draw context and its user_data pointer.
 * Shader modules (created from the provided SPIR-V) are available
 * via the MopShaderPlugin handle.
 * ------------------------------------------------------------------------- */

typedef void (*MopShaderDrawFn)(const MopShaderDrawContext *ctx,
                                void *user_data);

/* -------------------------------------------------------------------------
 * Plugin descriptor — passed to mop_viewport_register_shader
 * ------------------------------------------------------------------------- */

typedef struct MopShaderPluginDesc {
  const char *name; /* Human-readable label (copied internally) */
  MopShaderPluginStage stage;

  /* SPIR-V bytecode (optional — NULL for CPU-only plugins).
   * Bytecode is consumed immediately; the caller may free it after
   * registration returns. */
  const uint32_t *vertex_spirv;
  size_t vertex_spirv_size; /* bytes */
  const uint32_t *fragment_spirv;
  size_t fragment_spirv_size;    /* bytes */
  const uint32_t *compute_spirv; /* optional */
  size_t compute_spirv_size;     /* bytes */

  /* Draw callback — invoked once per frame at the registered stage. */
  MopShaderDrawFn draw;
  void *user_data;
} MopShaderPluginDesc;

/* -------------------------------------------------------------------------
 * Opaque handle — returned by register, used to query or unregister
 * ------------------------------------------------------------------------- */

typedef struct MopShaderPlugin MopShaderPlugin;

/* -------------------------------------------------------------------------
 * Registration API
 * ------------------------------------------------------------------------- */

/* Register a custom shader plugin.
 * Compiles SPIR-V into shader modules (if provided) and inserts the
 * plugin into the render graph at the designated stage.
 * Returns NULL on failure (invalid desc, OOM, shader compilation error). */
MopShaderPlugin *mop_viewport_register_shader(MopViewport *vp,
                                              const MopShaderPluginDesc *desc);

/* Unregister and destroy a shader plugin.
 * Releases shader modules and removes the plugin from the render graph.
 * The plugin pointer is invalid after this call. */
void mop_viewport_unregister_shader(MopViewport *vp, MopShaderPlugin *plugin);

/* -------------------------------------------------------------------------
 * Accessors — query plugin state
 * ------------------------------------------------------------------------- */

/* Return the plugin's name (never NULL for a valid plugin). */
const char *mop_shader_plugin_get_name(const MopShaderPlugin *plugin);

/* Return the RHI shader module for vertex/fragment/compute stage.
 * Returns NULL if no SPIR-V was provided for that stage. */
MopRhiShader *mop_shader_plugin_get_vertex(const MopShaderPlugin *plugin);
MopRhiShader *mop_shader_plugin_get_fragment(const MopShaderPlugin *plugin);
MopRhiShader *mop_shader_plugin_get_compute(const MopShaderPlugin *plugin);

#endif /* MOP_RENDER_SHADER_PLUGIN_H */
