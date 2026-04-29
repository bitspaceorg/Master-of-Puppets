/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * shader_plugin.h — Custom shader plugin registration and execution
 *
 * What this is (today):
 *   A render-graph hook. The host registers a callback bound to a stage
 *   (POST_OPAQUE, POST_SCENE, POST_PROCESS, OVERLAY); MOP invokes it at the
 *   right point each frame, holding the scene mutex so the callback can
 *   safely call public mutators (mop_mesh_set_*, animations, etc.).
 *   Optionally MOP compiles SPIR-V you provide into shader modules and
 *   exposes them via mop_shader_plugin_get_vertex/fragment/compute, but
 *   the public API does not yet expose the active command buffer or render
 *   pass — submitting custom draws into MOP's framebuffer is internals-only
 *   work. Treat the SPIR-V slots as managed storage you can fetch back via
 *   the accessors when (or if) you build a backend-specific bridge.
 *
 * What this is NOT (yet):
 *   A "submit your own draws into MOP's framebuffer" API. There is no
 *   exposed VkCommandBuffer / VkRenderPass / GL FBO in MopShaderDrawContext.
 *   Plugin-driven draw injection is on the roadmap; today's headline use
 *   case is "do something each frame against MOP's clock and scene state"
 *   — animating procedural content, driving meshes you already added via
 *   mop_viewport_add_mesh, ticking shader-plugin-owned simulations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_RENDER_SHADER_PLUGIN_H
#define MOP_RENDER_SHADER_PLUGIN_H

#include <mop/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
  /* RHI device handle — opaque backend pointer (`MopRhiDevice *`).
   * Provided as an escape hatch for hosts that have studied the unstable
   * internal RHI; not part of the public contract. The active command
   * buffer / render pass for the current frame are NOT exposed here, so
   * issuing your own draws into MOP's framebuffer is not supported by the
   * public API. NULL on the CPU backend. */
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

#ifdef __cplusplus
}
#endif

#endif /* MOP_RENDER_SHADER_PLUGIN_H */
