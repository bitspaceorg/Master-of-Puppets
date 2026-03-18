/*
 * Master of Puppets — Vulkan Backend
 * vulkan_internal.h — Internal struct definitions and helper declarations
 *
 * This header is private to the Vulkan backend translation units.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_VULKAN_INTERNAL_H
#define MOP_VULKAN_INTERNAL_H

#if defined(MOP_HAS_VULKAN)

/* Suppress -Wpedantic warnings from vulkan.h (zero-length arrays, etc.) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <vulkan/vulkan.h>
#pragma GCC diagnostic pop

#include "rhi/rhi.h"
#include <mop/util/log.h>
#include <mop/util/profile.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define MOP_VK_MAX_DRAWS_PER_FRAME 4096
#define MOP_VK_UBO_SIZE (4 * 1024 * 1024)      /* 4 MB per-frame UBO */
#define MOP_VK_STAGING_SIZE (16 * 1024 * 1024) /* 16 MB staging buffer */
#define MOP_VK_FRAMES_IN_FLIGHT 2
#define MOP_VK_PIPELINE_CACHE_CAPACITY 256 /* must be power of 2 */

/* -------------------------------------------------------------------------
 * Fragment uniform block — matches GLSL FragUniforms layout (std140)
 *
 * Must be kept in sync with mop_solid.frag / mop_wireframe.frag.
 * std140 alignment: vec4=16, float=4 (pad to 4), uint=4, int=4
 * Total: 576 bytes (64 base + 8 lights * 64).
 * Padded to device alignment at runtime.
 * ------------------------------------------------------------------------- */

/* Per-light data stored in the shared light SSBO (std430 layout).
 * Same structure used by both the C packing code and the GLSL shader. */
typedef struct MopVkLight {
  float position[4];  /* vec4: xyz + type (0=dir, 1=point, 2=spot) */
  float direction[4]; /* vec4: xyz + padding                       */
  float color[4];     /* vec4: rgb + intensity                     */
  float params[4];    /* vec4: range, spot_inner_cos, spot_outer_cos, active */
} MopVkLight;         /* 64 bytes per light, std430 aligned */

#define MOP_VK_MAX_SSBO_LIGHTS 4096       /* max lights in the SSBO */
#define MOP_VK_MAX_BINDLESS_TEXTURES 1024 /* max textures in bindless array */

/* -------------------------------------------------------------------------
 * Per-object data stored in the shared object SSBO (std430 layout).
 * Indexed by draw_id from push constants.  One entry per draw call.
 * Must be kept in sync with ObjectData in mop_solid.frag.
 * ------------------------------------------------------------------------- */

typedef struct MopVkObjectData {
  float model[16];        /* mat4 model matrix                    (offset  0) */
  float ambient;          /* float                                (offset 64) */
  float opacity;          /* float                                (offset 68) */
  uint32_t object_id;     /* uint                                 (offset 72) */
  int32_t blend_mode;     /* int                                  (offset 76) */
  float metallic;         /* float                                (offset 80) */
  float roughness;        /* float                                (offset 84) */
  int32_t base_tex_idx;   /* int: bindless texture index (-1=none)(offset 88) */
  int32_t normal_tex_idx; /* int: normal map index (-1=none)      (offset 92) */
  float emissive[4];      /* vec4: rgb + padding                  (offset 96) */
  int32_t mr_tex_idx;     /* int: metallic-roughness index        (offset 112)*/
  int32_t ao_tex_idx;     /* int: ambient occlusion index         (offset 116)*/
  int32_t _pad0[2];       /* align to 16                          (offset 120)*/
  float bound_sphere[4];  /* vec4: local-space center xyz + radius(offset 128)*/
} MopVkObjectData;        /* 144 bytes per object, std430 aligned */

/* -------------------------------------------------------------------------
 * Per-frame global data (uniform buffer, shared by all draws).
 * Camera, shadow, exposure — things that don't change per-object.
 * Must be kept in sync with FrameGlobals in mop_solid.frag.
 * ------------------------------------------------------------------------- */

typedef struct MopVkFrameGlobals {
  float light_dir[4];         /* vec4                    (offset  0) */
  float cam_pos[4];           /* vec4                    (offset 16) */
  int32_t shadows_enabled;    /* bool                    (offset 32) */
  int32_t cascade_count;      /* int                     (offset 36) */
  int32_t num_lights;         /* int                     (offset 40) */
  float exposure;             /* float                   (offset 44) */
  float cascade_vp[4][16];    /* 4 x mat4               (offset 48) */
  float cascade_splits[4];    /* vec4                    (offset 304)*/
  float view_proj[16];        /* mat4 VP matrix          (offset 320)*/
  float frustum_planes[6][4]; /* 6 × vec4 (LRTBNF)  (offset 384)*/
  uint32_t total_draws;       /* uint: total draw count  (offset 480)*/
  uint32_t _pad_globals[3];   /* align to 16           (offset 484)*/
} MopVkFrameGlobals;          /* 496 bytes */

/* Per-draw fragment uniforms (std140 layout).
 * Lights are NO LONGER embedded here — they live in a shared SSBO
 * (binding 9) that is uploaded once per frame.
 * Must be kept in sync with mop_solid.frag / mop_wireframe.frag. */
typedef struct MopVkFragUniforms {
  float light_dir[4];     /* vec4: xyz + padding          (offset  0) */
  float ambient;          /* float                        (offset 16) */
  float opacity;          /* float                        (offset 20) */
  uint32_t object_id;     /* uint                         (offset 24) */
  int32_t blend_mode;     /* int                          (offset 28) */
  int32_t has_texture;    /* int                          (offset 32) */
  int32_t num_lights;     /* int                          (offset 36) */
  float metallic;         /* float                        (offset 40) */
  float roughness;        /* float                        (offset 44) */
  int32_t has_normal_map; /* int                          (offset 48) */
  int32_t has_mr_map;     /* int                          (offset 52) */
  int32_t has_ao_map;     /* int                          (offset 56) */
  int32_t _pad_maps;      /* int                          (offset 60) */
  float cam_pos[4];       /* vec4: xyz + padding          (offset 64) */
  float emissive[4];      /* vec4: rgb + padding          (offset 80) */

  /* Shadow mapping (cascade) */
  int32_t shadows_enabled; /* bool: 1 = shadows active    (offset 96) */
  int32_t cascade_count;   /* number of active cascades */
  float _pad_shadow[2];    /* align to vec4 boundary */
  float cascade_vp[4][16]; /* 4 x mat4 (column-major) = 256 bytes */
  float cascade_splits[4]; /* view-space Z split distances */
  float exposure;          /* HDR exposure multiplier (scene only) */
  float _pad_exposure[3];  /* align to vec4 */
} MopVkFragUniforms;

/* -------------------------------------------------------------------------
 * Pipeline cache key
 *
 * Bits: wireframe(1) + depth_test(1) + backface_cull(1) + blend_mode(2)
 *       + non_standard_stride(1) = 6 bits
 * Max 64 unique combos, stored in a flat 64-slot array.
 * ------------------------------------------------------------------------- */

static inline uint32_t mop_vk_pipeline_key(bool wireframe, bool depth_test,
                                           bool backface_cull,
                                           MopBlendMode blend_mode,
                                           uint32_t vertex_stride) {
  uint32_t non_standard = (vertex_stride != sizeof(MopVertex)) ? 1u : 0u;
  return ((uint32_t)wireframe) | ((uint32_t)depth_test << 1) |
         ((uint32_t)backface_cull << 2) | ((uint32_t)blend_mode << 3) |
         (non_standard << 5);
}

/* -------------------------------------------------------------------------
 * Pipeline cache — open-addressing hash map
 *
 * Replaces the flat 64-slot array.  Key = (stride << 32 | pipeline_key).
 * Empty slot: key == 0 (valid because stride is never 0).
 * Linear probing, no tombstones (entries only removed on device destroy).
 * ------------------------------------------------------------------------- */

typedef struct MopVkPipelineCacheEntry {
  uint64_t key; /* 0 = empty slot */
  VkPipeline pipeline;
} MopVkPipelineCacheEntry;

static inline uint64_t mop_vk_pipeline_cache_key(uint32_t pipeline_key,
                                                 uint32_t vertex_stride) {
  return ((uint64_t)vertex_stride << 32) | (uint64_t)pipeline_key;
}

static inline uint32_t mop_vk_pipeline_cache_hash(uint64_t key) {
  key *= 0x9E3779B97F4A7C15ULL; /* golden ratio multiplicative hash */
  return (uint32_t)(key >> (64 - 8)) & (MOP_VK_PIPELINE_CACHE_CAPACITY - 1);
}

/* -------------------------------------------------------------------------
 * Per-thread Vulkan resources for multi-threaded command recording.
 *
 * Each worker thread gets its own command pool (Vulkan mandate:
 * command pools are not thread-safe) and descriptor pool.
 * Secondary command buffers are recorded in parallel by workers and
 * executed by the primary command buffer on the main thread.
 * ------------------------------------------------------------------------- */

#define MOP_VK_MAX_WORKER_THREADS 16

typedef struct MopVkThreadState {
  VkCommandPool cmd_pool;       /* per-thread command pool */
  VkCommandBuffer secondary_cb; /* one secondary CB per frame */
  VkDescriptorPool desc_pool;   /* per-thread descriptor allocation */
  bool cb_recording;            /* true while secondary CB is being recorded */
} MopVkThreadState;

/* -------------------------------------------------------------------------
 * Per-frame Vulkan resources (ring-buffered)
 *
 * Each frame in flight gets its own command buffer, fence, and descriptor
 * pool.  The active frame's handles are aliased into the device struct
 * (cmd_buf, fence, desc_pool) during frame_begin so all existing draw
 * code works unchanged.
 * ------------------------------------------------------------------------- */

typedef struct MopVkFrameResources {
  VkCommandBuffer cmd_buf;
  VkFence fence;
  VkDescriptorPool desc_pool;
} MopVkFrameResources;

/* -------------------------------------------------------------------------
 * MopRhiDevice — Vulkan device and shared resources
 * ------------------------------------------------------------------------- */

/* GPU timing result — populated after frame_end, read via frame_gpu_time_ms */
typedef struct MopGpuTimingResult {
  double gpu_frame_ms;
  double cpu_submit_ms;
  double cpu_readback_ms;
  double total_frame_ms;
} MopGpuTimingResult;

/* Validation callback function pointers (set by conformance runner) */
typedef void (*MopVkValidationCallback)(void);
extern MopVkValidationCallback mop_vk_on_validation_error;
extern MopVkValidationCallback mop_vk_on_sync_hazard;

/* Forward declaration for suballocator (Phase 5) */
typedef struct MopSuballocator MopSuballocator;

struct MopRhiDevice {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family;

  /* Debug messenger (VK_EXT_debug_utils) */
  VkDebugUtilsMessengerEXT debug_messenger;

  VkPhysicalDeviceMemoryProperties mem_props;
  VkPhysicalDeviceProperties dev_props;

  /* Command infrastructure */
  VkCommandPool cmd_pool;

  /* Per-frame resources (ring-buffered for N frames in flight) */
  MopVkFrameResources frames[MOP_VK_FRAMES_IN_FLIGHT];
  uint32_t frame_index;

  /* Active-frame aliases (set in frame_begin, used by draw/frame_end).
   * These point to frames[frame_index]'s resources so existing code
   * referencing dev->cmd_buf etc. works unchanged. */
  VkCommandBuffer cmd_buf;
  VkFence fence;

  /* Shader modules */
  VkShaderModule solid_vert;
  VkShaderModule instanced_vert;
  VkShaderModule solid_frag;
  VkShaderModule wireframe_frag;

  /* Shared pipeline state */
  VkPipelineLayout pipeline_layout;
  VkDescriptorSetLayout desc_set_layout;
  VkRenderPass render_pass;

  /* Pipeline cache: open-addressing hash map (replaces flat 64-slot array) */
  MopVkPipelineCacheEntry pipeline_cache[MOP_VK_PIPELINE_CACHE_CAPACITY];

  /* Vulkan pipeline cache (Phase 3 — disk-persistent).
   * Loaded from ~/.cache/mop/pipeline_cache.bin on device creation,
   * saved back on device destruction. */
  VkPipelineCache vk_pipeline_cache;

  /* Instanced pipeline (single variant — solid, depth-test, backface-cull) */
  VkPipeline instanced_pipeline;
  uint32_t instanced_pipeline_stride;

  /* Active-frame descriptor pool alias (set in frame_begin) */
  VkDescriptorPool desc_pool;

  /* Default sampler (linear, repeat) */
  VkSampler default_sampler;

  /* Clamp-to-edge sampler for screen-space post-processing (SSAO, bloom, etc.)
   * REPEAT causes wrapping artifacts when blur kernels extend past edges. */
  VkSampler clamp_sampler;

  /* 1x1 white fallback texture */
  VkImage white_image;
  VkDeviceMemory white_memory;
  VkImageView white_view;

  /* 1x1 black fallback texture (for IBL when no env map loaded) */
  VkImage black_image;
  VkDeviceMemory black_memory;
  VkImageView black_view;

  /* Staging buffer for uploads */
  VkBuffer staging_buf;
  VkDeviceMemory staging_mem;
  void *staging_mapped;
  VkFence staging_fence; /* tracks staging upload completion */

  /* GPU timestamp queries */
  VkQueryPool timestamp_pool;
  float timestamp_period_ns;
  MopGpuTimingResult last_timing;

  /* Per-pass GPU timing (Phase 9A) — expanded query pool.
   * Query 0..1 = frame start/end (existing).
   * Query 2..2+N*2-1 = per-pass start/end pairs.
   * Results read back after fence wait in frame_end. */
#define MOP_VK_MAX_PASS_TIMESTAMPS 96 /* 48 passes × 2 queries each */
#define MOP_VK_PASS_QUERY_OFFSET 2    /* first per-pass query index */
  VkQueryPool pass_timestamp_pool;
  uint32_t pass_query_count; /* queries used this frame */
  struct MopVkPassTimingEntry {
    const char *name;
    uint32_t query_start; /* index into pass_timestamp_pool */
    uint32_t query_end;
  } pass_timing_entries[MOP_MAX_GPU_PASS_TIMINGS];
  uint32_t pass_timing_count;
  /* Results from previous frame (read back after fence) */
  struct MopVkPassTimingResult {
    const char *name;
    double gpu_ms;
  } pass_timing_results[MOP_MAX_GPU_PASS_TIMINGS];
  uint32_t pass_timing_result_count;

  /* Device limits */
  VkDeviceSize min_ubo_alignment;

  /* Feature flags */
  bool has_fill_mode_non_solid;
  bool has_timestamp_queries;
  bool has_descriptor_indexing; /* VK_EXT_descriptor_indexing / Vulkan 1.2 */
  bool reverse_z;
  VkSampleCountFlagBits msaa_samples;

  /* Bindless texture registry (Phase 2A) — maps texture indices to views.
   * Slot 0 = white fallback, slot 1 = black fallback.  User textures start
   * at index 2.  Free slots are tracked via a simple next_free counter. */
  VkImageView texture_registry[MOP_VK_MAX_BINDLESS_TEXTURES];
  uint32_t texture_registry_count; /* next free index */

  /* Bindless descriptor set layout + global per-frame descriptor set */
  VkDescriptorSetLayout bindless_desc_layout;
  VkPipelineLayout bindless_pipeline_layout;

  /* Bindless shader modules (Phase 2A) */
  VkShaderModule bindless_solid_vert;
  VkShaderModule bindless_solid_frag;
  VkShaderModule bindless_wireframe_frag;

  /* GPU culling compute pipeline (Phase 2B) */
  VkShaderModule cull_comp;
  VkPipeline cull_pipeline;
  VkPipelineLayout cull_pipeline_layout;
  VkDescriptorSetLayout cull_desc_layout;
  bool gpu_culling_enabled;

  /* When true, use vkCmdDrawIndexedIndirectCount for bindless draws
   * instead of per-draw vkCmdDrawIndexed.  Requires gpu_culling_enabled
   * and at least one prior frame of cull data. */
  bool indirect_draw_enabled;
  uint32_t indirect_draw_frame_count; /* frames since cull data available */

  /* Hi-Z occlusion culling (Phase 2C) */
  VkShaderModule hiz_downsample_comp;
  VkPipeline hiz_pipeline;
  VkPipelineLayout hiz_pipeline_layout;
  VkDescriptorSetLayout hiz_desc_layout;
  VkSampler hiz_sampler; /* nearest, clamp-to-edge */
  bool hiz_enabled;

  /* Shadow mapping */
#define MOP_VK_SHADOW_MAP_SIZE 2048
#define MOP_VK_CASCADE_COUNT 4

  VkImage shadow_image; /* D32_SFLOAT, 2048x2048, 4 array layers */
  VkDeviceMemory shadow_memory;
  VkImageView shadow_views[MOP_VK_CASCADE_COUNT]; /* per-layer views */
  VkImageView shadow_array_view;                  /* array view for sampling */
  VkSampler shadow_sampler; /* comparison sampler with PCF */
  VkFramebuffer shadow_fbs[MOP_VK_CASCADE_COUNT];
  VkRenderPass shadow_render_pass;
  VkPipeline shadow_pipeline;
  VkPipelineLayout shadow_pipeline_layout;
  VkShaderModule shadow_vert;
  VkShaderModule shadow_frag;
  MopMat4 cascade_vp[MOP_VK_CASCADE_COUNT]; /* light-space VP per cascade */
  float cascade_splits[MOP_VK_CASCADE_COUNT + 1]; /* frustum split distances */
  bool shadows_enabled;

  /* Deferred shadow draw storage (populated during vk_draw, replayed in
   * frame_end).  Shadow maps are 1 frame behind (temporal shadow mapping). */
  struct MopVkShadowDraw {
    VkBuffer vertex_buf;
    VkBuffer index_buf;
    uint32_t index_count;
    float model[16]; /* for computing light_vp * model */
  } *shadow_draws;
  uint32_t shadow_draw_count;
  uint32_t shadow_draw_capacity;

  /* Cached view/projection/light_dir for shadow cascade computation
   * (stored during vk_draw, used in frame_end to compute cascades) */
  float cached_view[16];
  MopVec3 cached_light_dir;
  bool shadow_data_valid; /* true when view/proj/light captured this frame */

  /* Post-processing */
  VkRenderPass postprocess_render_pass;
  VkPipeline postprocess_pipeline;
  VkPipelineLayout postprocess_pipeline_layout;
  VkDescriptorSetLayout postprocess_desc_layout;
  VkShaderModule fullscreen_vert;
  VkShaderModule fxaa_frag;
  bool postprocess_enabled; /* controlled by MOP_POST_FXAA flag */

  /* HDR tonemapping */
  VkRenderPass tonemap_render_pass;
  VkPipeline tonemap_pipeline;
  VkPipelineLayout tonemap_pipeline_layout;
  VkDescriptorSetLayout tonemap_desc_layout;
  VkShaderModule tonemap_frag;
  float hdr_exposure; /* set per-frame before frame_end */
  bool tonemap_enabled;

  /* Skybox (equirectangular environment map) */
  VkPipeline skybox_pipeline;
  VkPipelineLayout skybox_pipeline_layout;
  VkDescriptorSetLayout skybox_desc_layout;
  VkShaderModule skybox_frag;
  bool skybox_enabled;

  /* IBL descriptor bindings (binding 3 = irradiance, 4 = prefiltered,
   * 5 = BRDF LUT) — textures stored on viewport, views cached here */
  VkImageView env_map_view;     /* environment map for skybox */
  VkImageView irradiance_view;  /* diffuse irradiance */
  VkImageView prefiltered_view; /* prefiltered specular */
  VkImageView brdf_lut_view;    /* BRDF LUT */

  /* SDF overlay pass (lines, circles, diamonds via fullscreen SDF shader) */
  VkRenderPass overlay_render_pass;
  VkPipeline overlay_pipeline;
  VkPipelineLayout overlay_pipeline_layout;
  VkDescriptorSetLayout overlay_desc_layout;
  VkShaderModule overlay_frag;
  bool overlay_enabled;

  /* Analytical grid overlay (fullscreen shader on Y=0 plane) */
  VkPipeline grid_pipeline;
  VkPipelineLayout grid_pipeline_layout;
  VkDescriptorSetLayout grid_desc_layout;
  VkShaderModule grid_frag;
  bool grid_enabled;

  /* Bloom post-process */
  VkRenderPass bloom_render_pass;
  VkPipeline bloom_extract_pipeline;
  VkPipeline bloom_blur_pipeline;
  VkPipeline bloom_upsample_pipeline; /* two-texture upsample (LOAD_OP_CLEAR) */
  VkPipelineLayout bloom_pipeline_layout;
  VkPipelineLayout bloom_upsample_pl_layout; /* 2-binding layout for upsample */
  VkDescriptorSetLayout bloom_desc_layout;
  VkDescriptorSetLayout
      bloom_upsample_desc_layout; /* 2 samplers for upsample */
  VkShaderModule bloom_extract_frag;
  VkShaderModule bloom_blur_frag;
  VkShaderModule bloom_upsample_frag;
  bool bloom_enabled;
  float bloom_threshold; /* default 1.0 */
  float bloom_intensity; /* default 0.5 */

  /* SSAO */
  VkRenderPass ssao_render_pass;
  VkPipeline ssao_pipeline;
  VkPipeline ssao_blur_pipeline;
  VkPipelineLayout ssao_pipeline_layout;
  VkDescriptorSetLayout ssao_desc_layout;
  VkShaderModule ssao_frag;
  VkShaderModule ssao_blur_frag;
  VkImage ssao_noise_image;
  VkDeviceMemory ssao_noise_memory;
  VkImageView ssao_noise_view;
  VkBuffer ssao_kernel_ubo;
  VkDeviceMemory ssao_kernel_mem;
  bool ssao_enabled;
  VkRenderPass ssao_merged_render_pass; /* Phase 6: merged SSAO+blur */

  /* GTAO (upgraded SSAO) — shares render pass, pipeline layout, noise, and
   * framebuffers with SSAO.  When gtao_pipeline is available, the SSAO pass
   * in frame_end uses it instead of the classic ssao_pipeline. */
  VkPipeline gtao_pipeline;
  VkPipeline gtao_blur_pipeline;
  VkShaderModule gtao_frag;
  VkShaderModule gtao_blur_frag;
  bool gtao_available; /* true when GTAO shaders compiled successfully */

  /* Cached projection matrix (stored during draw, used by SSAO in frame_end) */
  float cached_projection[16];

  /* SSR pipeline resources */
  VkRenderPass ssr_render_pass;
  VkPipeline ssr_pipeline;
  VkPipelineLayout ssr_pipeline_layout;
  VkDescriptorSetLayout ssr_desc_layout;
  VkShaderModule ssr_frag;
  bool ssr_enabled;
  float ssr_intensity; /* default 0.5 */

  /* OIT (Weighted Blended Order-Independent Transparency) */
  VkRenderPass oit_render_pass; /* 2 color (accum+reveal) + depth r/o */
  VkPipeline oit_pipeline;      /* accumulation pass */
  VkShaderModule oit_accum_frag;
  VkRenderPass oit_composite_render_pass; /* 1 color (blend into opaque) */
  VkPipeline oit_composite_pipeline;
  VkPipelineLayout oit_composite_pipeline_layout;
  VkDescriptorSetLayout oit_composite_desc_layout;
  VkShaderModule oit_composite_frag;
  bool oit_enabled;

  /* OIT deferred transparent draw storage */
  struct MopVkDeferredOitDraw {
    VkBuffer vertex_buf;
    VkBuffer index_buf;
    uint32_t index_count;
    uint32_t draw_id;
    float push_data[32]; /* mvp(16) + model(16) */
  } *oit_draws;
  uint32_t oit_draw_count;
  uint32_t oit_draw_capacity;

  /* Deferred decals */
  VkRenderPass decal_render_pass; /* alpha-blend into HDR color */
  VkPipeline decal_pipeline;
  VkPipelineLayout decal_pipeline_layout;
  VkDescriptorSetLayout decal_desc_layout; /* depth + decal_tex + UBO */
  VkShaderModule decal_vert;
  VkShaderModule decal_frag;

  /* Decal list (managed by public API, rendered in frame_end) */
#define MOP_VK_MAX_DECALS 256
  struct MopVkDecal {
    float model[16];     /* decal_model (decal → world) */
    float inv_model[16]; /* inverse(decal_model) */
    float opacity;
    int32_t texture_idx; /* bindless texture index, or -1 for white */
    bool active;
  } decals[MOP_VK_MAX_DECALS];
  uint32_t decal_count;

  /* Per-frame decal UBO (inv_vp + reverse_z + opacity) */
  VkBuffer decal_ubo;
  VkDeviceMemory decal_ubo_mem;
  void *decal_ubo_mapped;

  /* Volumetric fog (fullscreen raymarch, blends into HDR color) */
  VkRenderPass volumetric_render_pass;
  VkPipeline volumetric_pipeline;
  VkPipelineLayout volumetric_pipeline_layout;
  VkDescriptorSetLayout volumetric_desc_layout; /* depth + light SSBO + UBO */
  VkShaderModule volumetric_frag;
  bool volumetric_enabled;
  float volumetric_density;    /* fog density (default 0.02) */
  float volumetric_color[3];   /* scattering color (default white) */
  float volumetric_anisotropy; /* HG phase g parameter (default 0.3) */
  int32_t volumetric_steps;    /* raymarch steps (default 32) */

  /* TAA pipeline resources */
  VkRenderPass taa_render_pass;
  VkPipeline taa_pipeline;
  VkPipelineLayout taa_pipeline_layout;
  VkDescriptorSetLayout taa_desc_layout;
  VkShaderModule taa_frag;
  bool taa_enabled;

  /* TAA per-frame cached data (set via set_taa_params before frame_end) */
  float taa_inv_vp_jittered[16]; /* inverse(jittered_proj * view) */
  float taa_prev_vp[16];         /* prev frame unjittered VP */
  float taa_jitter[2];           /* jitter in pixels */
  bool taa_first_frame;          /* true = no history yet */

  /* Multi-threaded command recording (Phase 1B) */
  MopVkThreadState thread_states[MOP_VK_MAX_WORKER_THREADS];
  uint32_t thread_count; /* number of initialized thread states */

  /* Async compute queue (Phase 1C) — separate queue for GPU culling,
   * particle simulation, and future compute workloads.  Falls back to
   * the graphics queue when no dedicated/separate compute family exists. */
  VkQueue compute_queue;
  uint32_t compute_queue_family;
  VkCommandPool compute_cmd_pool;
  VkCommandBuffer compute_cmd_buf; /* per-frame compute CB */
  VkSemaphore compute_semaphore;   /* signals when compute work finishes */
  VkFence compute_fence;           /* CPU wait for compute completion */
  bool has_async_compute;          /* true = separate compute queue family */

  /* Mesh shading (Phase 10) — VK_EXT_mesh_shader.
   * When supported, task shaders do per-meshlet frustum + backface culling
   * and mesh shaders emit vertices/primitives from meshlet data.
   * Falls back to traditional vertex pipeline when not available. */
  bool has_mesh_shader;              /* device supports VK_EXT_mesh_shader */
  uint32_t max_mesh_output_vertices; /* device limit */
  uint32_t max_mesh_output_prims;    /* device limit */
  uint32_t max_mesh_workgroup_size;  /* device limit */
  uint32_t max_task_workgroup_size;  /* device limit */

  /* Mesh shader pipeline resources */
  VkShaderModule meshlet_task; /* task shader module */
  VkShaderModule meshlet_mesh; /* mesh shader module */
  VkPipeline meshlet_pipeline; /* task → mesh → frag pipeline */
  VkPipelineLayout meshlet_pipeline_layout;
  VkDescriptorSetLayout meshlet_desc_layout;
  VkDescriptorSet meshlet_desc_set; /* per-frame descriptor set */

  /* Meshlet GPU buffers (uploaded once per mesh, re-bound per draw) */
  VkBuffer meshlet_ssbo; /* MopMeshlet array */
  VkDeviceMemory meshlet_ssbo_mem;
  VkBuffer meshlet_cone_ssbo; /* MopMeshletCone array */
  VkDeviceMemory meshlet_cone_ssbo_mem;
  VkBuffer meshlet_vert_idx_ssbo; /* meshlet vertex indices */
  VkDeviceMemory meshlet_vert_idx_ssbo_mem;
  VkBuffer meshlet_prim_idx_ssbo; /* meshlet primitive indices */
  VkDeviceMemory meshlet_prim_idx_ssbo_mem;
  uint32_t meshlet_total_count; /* total meshlets across all meshes */

  /* Cached extension function pointer (Phase 8) */
  PFN_vkVoidFunction pfn_draw_mesh_tasks; /* vkCmdDrawMeshTasksEXT */

  /* RTX readiness (Phase 9 — design only).
   * Capability probed at device creation.  No BLAS/TLAS building yet. */
  bool has_raytracing;   /* VK_KHR_acceleration_structure available */
  bool has_ray_query;    /* VK_KHR_ray_query available */
  bool has_ray_pipeline; /* VK_KHR_ray_tracing_pipeline available */

  /* Reserved descriptor binding slots for acceleration structures */
#define MOP_VK_ACCEL_STRUCT_BINDING 20 /* binding slot for TLAS in shaders */

  /* Previous frame's framebuffer — needed for deferred readback (Phase 1) */
  MopRhiFramebuffer *prev_framebuffer;

  /* Memory suballocator (Phase 5) */
  MopSuballocator *suballocator;
};

/* -------------------------------------------------------------------------
 * MopRhiFramebuffer — Offscreen render target with readback
 * ------------------------------------------------------------------------- */

struct MopRhiFramebuffer {
  int width;
  int height;

  /* Color attachment (R8G8B8A8_SRGB — linear→sRGB on write) */
  VkImage color_image;
  VkDeviceMemory color_memory;
  VkImageView color_view;

  /* Picking attachment (R32_UINT) */
  VkImage pick_image;
  VkDeviceMemory pick_memory;
  VkImageView pick_view;

  /* Depth attachment (D32_SFLOAT) */
  VkImage depth_image;
  VkDeviceMemory depth_memory;
  VkImageView depth_view;

  /* R32_SFLOAT copy of depth for overlay sampling.
   * MoltenVK cannot reliably sample D32_SFLOAT across CB boundaries,
   * so we copy depth data into a regular color image for the overlay pass. */
  VkImage depth_copy_image;
  VkDeviceMemory depth_copy_memory;
  VkImageView depth_copy_view;

  /* MSAA render targets (4x, only when msaa_samples > 1) */
  VkImage msaa_color_image;
  VkDeviceMemory msaa_color_memory;
  VkImageView msaa_color_view;

  VkImage msaa_pick_image;
  VkDeviceMemory msaa_pick_memory;
  VkImageView msaa_pick_view;

  VkImage msaa_depth_image;
  VkDeviceMemory msaa_depth_memory;
  VkImageView msaa_depth_view;

  VkFramebuffer framebuffer;

  /* LDR output image (R8G8B8A8_SRGB — tonemap resolve target) */
  VkImage ldr_color_image;
  VkDeviceMemory ldr_color_memory;
  VkImageView ldr_color_view;
  VkFramebuffer tonemap_framebuffer;

  /* Overlay framebuffer — renders on top of LDR color image */
  VkFramebuffer overlay_framebuffer;

  /* Readback staging buffers (host-visible, persistently mapped) */
  VkBuffer readback_color_buf;
  VkDeviceMemory readback_color_mem;
  void *readback_color_mapped;

  VkBuffer readback_pick_buf;
  VkDeviceMemory readback_pick_mem;
  void *readback_pick_mapped;

  VkBuffer readback_depth_buf;
  VkDeviceMemory readback_depth_mem;
  void *readback_depth_mapped;

  /* CPU-side readback arrays */
  uint8_t *readback_color;
  uint32_t *readback_pick;
  float *readback_depth;

  /* Per-frame dynamic UBO (host-visible, persistently mapped) */
  VkBuffer ubo_buf;
  VkDeviceMemory ubo_mem;
  void *ubo_mapped;
  VkDeviceSize ubo_offset; /* current write offset */

  /* Per-frame object SSBO (host-visible, indexed by draw_id).
   * Contains MopVkObjectData[MOP_VK_MAX_DRAWS_PER_FRAME]. */
  VkBuffer object_ssbo;
  VkDeviceMemory object_ssbo_mem;
  void *object_ssbo_mapped;
  uint32_t draw_count_this_frame; /* number of objects written */
  bool globals_scene_written; /* true after globals UBO re-written with scene
                                 data */

  /* Per-frame globals UBO (camera, shadow, exposure — shared across draws) */
  VkBuffer globals_ubo;
  VkDeviceMemory globals_ubo_mem;
  void *globals_ubo_mapped;

  /* Bindless descriptor set (allocated per-frame, one for all draws) */
  VkDescriptorSet bindless_ds;

  /* Indirect draw buffers (Phase 2B — GPU culling).
   * input_draw_cmds: CPU-populated VkDrawIndexedIndirectCommand[max_draws]
   * output_draw_cmds: GPU-compacted visible commands (written by cull shader)
   * draw_count_buf: single uint32_t atomic counter for visible draw count */
  VkBuffer input_draw_cmds;
  VkDeviceMemory input_draw_cmds_mem;
  void *input_draw_cmds_mapped;

  VkBuffer output_draw_cmds;
  VkDeviceMemory output_draw_cmds_mem;

  VkBuffer draw_count_buf;
  VkDeviceMemory draw_count_buf_mem;
  void *draw_count_buf_mapped;

  /* Readback copy of draw_count_buf for CPU-side stats (Phase 4).
   * Read back 1 frame behind (after fence wait). */
  uint32_t last_visible_draws;
  uint32_t last_culled_draws;

  /* Compute culling descriptor set (per-frame) */
  VkDescriptorSet cull_ds;

  /* Hi-Z depth pyramid (Phase 2C — occlusion culling).
   * R32_SFLOAT image with mip chain.  Level 0 = full resolution depth,
   * each subsequent level = max of 2×2 block from previous level.
   * Sampled by the culling compute shader for occlusion tests. */
#define MOP_VK_HIZ_MAX_LEVELS 16
  VkImage hiz_image;
  VkDeviceMemory hiz_memory;
  VkImageView hiz_views[MOP_VK_HIZ_MAX_LEVELS]; /* per-mip views */
  uint32_t hiz_levels;                          /* actual mip count */
  uint32_t hiz_width;                           /* level-0 width */
  uint32_t hiz_height;                          /* level-0 height */

  /* Per-frame light SSBO (host-visible, shared across all draw calls).
   * Contains MopVkLight[num_lights] — uploaded once in frame_begin,
   * bound at binding 9 for every descriptor set. */
  VkBuffer light_ssbo;
  VkDeviceMemory light_ssbo_mem;
  void *light_ssbo_mapped;
  VkDeviceSize light_ssbo_size;    /* allocated size in bytes */
  uint32_t light_count_this_frame; /* number of lights uploaded */

  /* Per-frame instance buffer (host-visible, used as vertex buffer) */
  VkBuffer instance_buf;
  VkDeviceMemory instance_mem;
  void *instance_mapped;
  VkDeviceSize instance_buf_size; /* allocated size in bytes */
  VkDeviceSize instance_offset;   /* current write offset */

  /* Bloom mip chain (half-res, R16G16B16A16_SFLOAT).
   * MOP_VK_BLOOM_DOWNSAMPLE: levels actually rendered (deeper levels corrupt
   * on MoltenVK TBDR when image dimensions fall below Metal tile size). */
#define MOP_VK_BLOOM_LEVELS 5
#define MOP_VK_BLOOM_DOWNSAMPLE 5
  VkImage bloom_images[MOP_VK_BLOOM_LEVELS];
  VkDeviceMemory bloom_memory[MOP_VK_BLOOM_LEVELS];
  VkImageView bloom_views[MOP_VK_BLOOM_LEVELS];
  VkFramebuffer bloom_fbs[MOP_VK_BLOOM_LEVELS];

  /* Bloom upsample output images (separate targets to avoid LOAD_OP_LOAD) */
  VkImage bloom_up_images[MOP_VK_BLOOM_LEVELS];
  VkDeviceMemory bloom_up_memory[MOP_VK_BLOOM_LEVELS];
  VkImageView bloom_up_views[MOP_VK_BLOOM_LEVELS];
  VkFramebuffer bloom_up_fbs[MOP_VK_BLOOM_LEVELS];

  /* SSAO attachments (R8_UNORM) */
  VkImage ssao_image;
  VkDeviceMemory ssao_memory;
  VkImageView ssao_view;
  VkFramebuffer ssao_fb;
  VkImage ssao_blur_image;
  VkDeviceMemory ssao_blur_memory;
  VkImageView ssao_blur_view;
  VkFramebuffer ssao_blur_fb;

  /* Merged SSAO+blur framebuffer (Phase 6): 2 attachments in single render pass
   */
  VkFramebuffer ssao_merged_fb;

  /* SSR output (R16G16B16A16_SFLOAT — reflection RGB + confidence alpha) */
  VkImage ssr_image;
  VkDeviceMemory ssr_memory;
  VkImageView ssr_view;
  VkFramebuffer ssr_framebuffer;

  /* OIT render targets */
  VkImage oit_accum_image; /* R16G16B16A16_SFLOAT */
  VkDeviceMemory oit_accum_mem;
  VkImageView oit_accum_view;
  VkImage oit_revealage_image; /* R8_UNORM */
  VkDeviceMemory oit_revealage_mem;
  VkImageView oit_revealage_view;
  VkFramebuffer oit_framebuffer;           /* accum + revealage + depth */
  VkFramebuffer oit_composite_framebuffer; /* writes into color_image */

  /* Decal framebuffer: blends into color_image (reuses depth for read) */
  VkFramebuffer decal_framebuffer;

  /* Volumetric fog framebuffer (blends into color_image) + per-frame UBO */
  VkFramebuffer volumetric_framebuffer;
  VkBuffer volumetric_ubo;
  VkDeviceMemory volumetric_ubo_mem;
  void *volumetric_ubo_mapped;

  /* TAA history textures (ping-pong, R8G8B8A8_SRGB — same as ldr_color) */
  VkImage taa_history[2];
  VkDeviceMemory taa_history_mem[2];
  VkImageView taa_history_view[2];
  VkFramebuffer taa_framebuffer[2]; /* render into history[current] */
  uint32_t taa_current;             /* 0 or 1 — ping-pong index */

  /* Persistent overlay SSBO (reused across frames, grows with power-of-2) */
  VkBuffer overlay_ssbo;
  VkDeviceMemory overlay_ssbo_mem;
  void *overlay_ssbo_mapped;
  VkDeviceSize overlay_ssbo_size; /* current allocated size in bytes */
};

/* -------------------------------------------------------------------------
 * MopRhiBuffer — Device-local buffer with CPU shadow
 * ------------------------------------------------------------------------- */

struct MopRhiBuffer {
  VkBuffer buffer;
  VkDeviceMemory memory;
  size_t size;
  void *shadow; /* CPU-side copy (matches OpenGL pattern) */

  /* Meshlet data (Phase 8 — populated during mesh upload when meshlet
   * builder runs).  When meshlet_count > 0, the mesh can use the mesh
   * shading pipeline instead of the traditional vertex pipeline. */
  uint32_t meshlet_count;
  uint32_t meshlet_offset; /* offset into device meshlet SSBO */
};

/* -------------------------------------------------------------------------
 * MopRhiShader — Vulkan shader module wrapper
 * ------------------------------------------------------------------------- */

struct MopRhiShader {
  VkShaderModule module;
};

/* -------------------------------------------------------------------------
 * MopRhiTexture — Device-local image with view
 * ------------------------------------------------------------------------- */

struct MopRhiTexture {
  VkImage image;
  VkDeviceMemory memory;
  VkImageView view;
  int width;
  int height;
  bool is_hdr;            /* true = R32G32B32A32_SFLOAT format */
  int32_t bindless_index; /* index into device texture registry (-1 = none) */
};

/* -------------------------------------------------------------------------
 * Helper declarations — vulkan_memory.c
 * ------------------------------------------------------------------------- */

/* Find a memory type index matching requirements and property flags.
 * Returns -1 on failure. */
int mop_vk_find_memory_type(const VkPhysicalDeviceMemoryProperties *props,
                            uint32_t type_filter, VkMemoryPropertyFlags flags);

/* Create a VkBuffer + VkDeviceMemory pair. Returns VK_SUCCESS or error. */
VkResult mop_vk_create_buffer(VkDevice device,
                              const VkPhysicalDeviceMemoryProperties *props,
                              VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags mem_flags,
                              VkBuffer *out_buffer, VkDeviceMemory *out_memory);

/* Create a VkImage + VkDeviceMemory pair. Returns VK_SUCCESS or error. */
VkResult mop_vk_create_image(VkDevice device,
                             const VkPhysicalDeviceMemoryProperties *props,
                             uint32_t width, uint32_t height, VkFormat format,
                             VkSampleCountFlagBits samples,
                             VkImageUsageFlags usage, VkImage *out_image,
                             VkDeviceMemory *out_memory);

/* Create a VkImage with mip chain + VkDeviceMemory. Returns VK_SUCCESS. */
VkResult mop_vk_create_image_mipped(
    VkDevice device, const VkPhysicalDeviceMemoryProperties *props,
    uint32_t width, uint32_t height, VkFormat format, uint32_t mip_levels,
    VkImageUsageFlags usage, VkImage *out_image, VkDeviceMemory *out_memory);

/* Create a VkImageView for an image. Returns VK_SUCCESS or error. */
VkResult mop_vk_create_image_view(VkDevice device, VkImage image,
                                  VkFormat format, VkImageAspectFlags aspect,
                                  VkImageView *out_view);

/* Create a VkImageView for a specific mip level of an image. */
VkResult mop_vk_create_image_view_mip(VkDevice device, VkImage image,
                                      VkFormat format,
                                      VkImageAspectFlags aspect,
                                      uint32_t mip_level,
                                      VkImageView *out_view);

/* -------------------------------------------------------------------------
 * Helper declarations — vulkan_pipeline.c
 * ------------------------------------------------------------------------- */

/* Create the render pass with color + picking + depth attachments.
 * When samples > 1, uses VkRenderPassCreateInfo2 with MSAA resolve. */
VkResult mop_vk_create_render_pass(VkDevice device,
                                   VkSampleCountFlagBits samples,
                                   VkRenderPass *out);

/* Create the descriptor set layout (UBO + combined image sampler). */
VkResult mop_vk_create_desc_set_layout(VkDevice device,
                                       VkDescriptorSetLayout *out);

/* Create the pipeline layout (push constants + one descriptor set). */
VkResult mop_vk_create_pipeline_layout(VkDevice device,
                                       VkDescriptorSetLayout desc_layout,
                                       VkPipelineLayout *out);

/* Get or lazily create a pipeline for the given state key. */
VkPipeline mop_vk_get_pipeline(struct MopRhiDevice *dev, uint32_t key,
                               uint32_t vertex_stride);

/* Get or lazily create the instanced rendering pipeline. */
VkPipeline mop_vk_get_instanced_pipeline(struct MopRhiDevice *dev,
                                         uint32_t vertex_stride);

/* Compute cascade shadow map split distances and light VP matrices. */
void vk_compute_cascades(struct MopRhiDevice *dev, const MopMat4 *view,
                         const MopMat4 *proj, MopVec3 light_dir);

/* Create the shadow-only render pass (depth-only, single attachment). */
VkResult mop_vk_create_shadow_render_pass(VkDevice device, VkRenderPass *out);

/* Create the shadow pipeline (depth-only with bias). */
VkPipeline mop_vk_create_shadow_pipeline(struct MopRhiDevice *dev);

/* Create the post-process render pass (single color attachment). */
VkResult mop_vk_create_postprocess_render_pass(VkDevice device,
                                               VkRenderPass *out);

/* Create the FXAA post-process pipeline. */
VkPipeline mop_vk_create_postprocess_pipeline(struct MopRhiDevice *dev);

/* Create the HDR tonemap render pass (single R8G8B8A8_SRGB attachment). */
VkResult mop_vk_create_tonemap_render_pass(VkDevice device, VkRenderPass *out);

/* Create the HDR tonemap pipeline (fullscreen triangle + ACES). */
VkPipeline mop_vk_create_tonemap_pipeline(struct MopRhiDevice *dev);

/* Create the skybox pipeline (fullscreen triangle + equirectangular env map).
 * Writes to the main render pass color + picking attachments. */
VkPipeline mop_vk_create_skybox_pipeline(struct MopRhiDevice *dev);

/* Create the overlay render pass (single color attachment, alpha blend).
 * Loads existing LDR color and blends SDF overlays on top.
 * Final layout is TRANSFER_SRC_OPTIMAL for readback. */
VkResult mop_vk_create_overlay_render_pass(VkDevice device, VkRenderPass *out);

/* Create the SDF overlay pipeline (fullscreen triangle + SDF prims). */
VkPipeline mop_vk_create_overlay_pipeline(struct MopRhiDevice *dev);

/* Create the analytical grid pipeline (fullscreen triangle + grid shader). */
VkPipeline mop_vk_create_grid_pipeline(struct MopRhiDevice *dev);

/* Create the bloom render pass (R16G16B16A16_SFLOAT, single color). */
VkResult mop_vk_create_bloom_render_pass(VkDevice device, VkRenderPass *out);

/* Create the bloom extract pipeline (fullscreen triangle + threshold). */
VkPipeline mop_vk_create_bloom_extract_pipeline(struct MopRhiDevice *dev);

/* Create the bloom blur pipeline (fullscreen triangle + 9-tap Gaussian). */
VkPipeline mop_vk_create_bloom_blur_pipeline(struct MopRhiDevice *dev);

/* Create the bloom upsample pipeline (two-texture, LOAD_OP_CLEAR). */
VkPipeline mop_vk_create_bloom_upsample_pipeline(struct MopRhiDevice *dev);

/* Create the SSAO render pass (R8_UNORM, single color attachment). */
VkResult mop_vk_create_ssao_render_pass(VkDevice device, VkRenderPass *out);

/* Merged SSAO + blur render pass (Phase 6 — pass merging) */
VkResult mop_vk_create_ssao_merged_render_pass(VkDevice device,
                                               VkRenderPass *out);

/* Create the SSAO pipeline (fullscreen triangle + hemisphere sampling). */
VkPipeline mop_vk_create_ssao_pipeline(struct MopRhiDevice *dev);

/* Create the SSAO blur pipeline (fullscreen triangle + bilateral blur). */
VkPipeline mop_vk_create_ssao_blur_pipeline(struct MopRhiDevice *dev);

/* GTAO pipelines — same render pass and layout as SSAO, different shaders. */
VkPipeline mop_vk_create_gtao_pipeline(struct MopRhiDevice *dev);
VkPipeline mop_vk_create_gtao_blur_pipeline(struct MopRhiDevice *dev);

/* Create the bindless descriptor set layout (Phase 2A). */
VkResult mop_vk_create_bindless_desc_layout(VkDevice device,
                                            uint32_t max_textures,
                                            VkDescriptorSetLayout *out);

/* Create the bindless pipeline layout (Phase 2A). */
VkResult
mop_vk_create_bindless_pipeline_layout(VkDevice device,
                                       VkDescriptorSetLayout bindless_layout,
                                       VkPipelineLayout *out);

/* Get or lazily create a bindless pipeline for the given state key. */
VkPipeline mop_vk_get_bindless_pipeline(struct MopRhiDevice *dev, uint32_t key,
                                        uint32_t vertex_stride);

/* GPU culling compute pipeline (Phase 2B) */
VkResult mop_vk_create_cull_desc_layout(VkDevice device,
                                        VkDescriptorSetLayout *out);
VkResult mop_vk_create_cull_pipeline(struct MopRhiDevice *dev);

/* GPU culling dispatch (records compute commands into the given CB) */
void mop_vk_dispatch_gpu_cull(struct MopRhiDevice *dev,
                              struct MopRhiFramebuffer *fb, VkCommandBuffer cb);

/* Hi-Z occlusion culling (Phase 2C) */
VkResult mop_vk_create_hiz_desc_layout(VkDevice device,
                                       VkDescriptorSetLayout *out);
VkResult mop_vk_create_hiz_pipeline(struct MopRhiDevice *dev);
void mop_vk_build_hiz_pyramid(struct MopRhiDevice *dev,
                              struct MopRhiFramebuffer *fb, VkCommandBuffer cb);

/* Per-pass GPU timing (Phase 9A) */
void mop_vk_pass_timestamp_begin(struct MopRhiDevice *dev,
                                 const char *pass_name);
void mop_vk_pass_timestamp_end(struct MopRhiDevice *dev);

/* Async compute helpers (Phase 1C) */
typedef void (*MopVkComputeRecordFn)(struct MopRhiDevice *dev,
                                     VkCommandBuffer cb, void *user_data);
bool mop_vk_submit_async_compute(struct MopRhiDevice *dev,
                                 MopVkComputeRecordFn record_fn,
                                 void *user_data);

/* SSR (Screen-Space Reflections) pass */
VkResult mop_vk_create_ssr_render_pass(VkDevice device, VkRenderPass *out);
VkPipeline mop_vk_create_ssr_pipeline(struct MopRhiDevice *dev);

/* OIT (Order-Independent Transparency) */
VkResult mop_vk_create_oit_render_pass(VkDevice device, VkFormat depth_format,
                                       VkRenderPass *out);
VkPipeline mop_vk_create_oit_pipeline(struct MopRhiDevice *dev);
VkResult mop_vk_create_oit_composite_render_pass(VkDevice device,
                                                 VkRenderPass *out);
VkPipeline mop_vk_create_oit_composite_pipeline(struct MopRhiDevice *dev);

/* Deferred decals */
VkResult mop_vk_create_decal_render_pass(VkDevice device, VkRenderPass *out);
VkPipeline mop_vk_create_decal_pipeline(struct MopRhiDevice *dev);

/* Volumetric fog */
VkResult mop_vk_create_volumetric_render_pass(VkDevice device,
                                              VkRenderPass *out);
VkPipeline mop_vk_create_volumetric_pipeline(struct MopRhiDevice *dev);

/* TAA resolve pass */
VkResult mop_vk_create_taa_render_pass(VkDevice device, VkRenderPass *out);
VkPipeline mop_vk_create_taa_pipeline(struct MopRhiDevice *dev);

/* Mesh shading pipeline (Phase 10) */
VkResult mop_vk_create_meshlet_desc_layout(VkDevice device,
                                           VkDescriptorSetLayout *out);
VkResult mop_vk_create_meshlet_pipeline(struct MopRhiDevice *dev);

/* -------------------------------------------------------------------------
 * Utility: one-shot command buffer for upload/transition
 * ------------------------------------------------------------------------- */

static inline VkCommandBuffer mop_vk_begin_oneshot(VkDevice device,
                                                   VkCommandPool pool) {
  VkCommandBufferAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  VkCommandBuffer cb;
  vkAllocateCommandBuffers(device, &ai, &cb);

  VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer(cb, &bi);
  return cb;
}

static inline void mop_vk_end_oneshot(VkDevice device, VkQueue queue,
                                      VkCommandPool pool, VkCommandBuffer cb) {
  vkEndCommandBuffer(cb);
  VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cb,
  };
  vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue);
  vkFreeCommandBuffers(device, pool, 1, &cb);
}

/* -------------------------------------------------------------------------
 * Utility: image layout transition
 * ------------------------------------------------------------------------- */

static inline void mop_vk_transition_image(
    VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
    VkImageLayout old_layout, VkImageLayout new_layout,
    VkAccessFlags src_access, VkAccessFlags dst_access,
    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = src_access,
      .dstAccessMask = dst_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange =
          {
              .aspectMask = aspect,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
                       &barrier);
}

/* -------------------------------------------------------------------------
 * Utility: align a value up to the given alignment
 * ------------------------------------------------------------------------- */

static inline VkDeviceSize mop_vk_align(VkDeviceSize value,
                                        VkDeviceSize alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

/* -------------------------------------------------------------------------
 * Memory suballocator (Phase 5)
 * ------------------------------------------------------------------------- */

/* Suballocation result — returned by mop_suballoc_alloc */
typedef struct MopSuballocResult {
  VkDeviceMemory memory; /* parent block's VkDeviceMemory */
  VkDeviceSize offset;   /* byte offset within the block */
  VkDeviceSize size;     /* allocated size */
  void *mapped;          /* mapped pointer (NULL if not host-visible) */
  bool success;          /* true if allocation succeeded */
} MopSuballocResult;

MopSuballocator *
mop_suballoc_create(VkDevice device,
                    const VkPhysicalDeviceMemoryProperties *props,
                    VkDeviceSize min_alignment);
void mop_suballoc_destroy(MopSuballocator *sa);

/* Allocate from the suballocator.
 * persistent=true uses free-list (for long-lived resources like mesh VBOs).
 * persistent=false uses linear bump allocator (for per-frame transients). */
MopSuballocResult mop_suballoc_alloc(MopSuballocator *sa, VkDeviceSize size,
                                     VkDeviceSize alignment,
                                     uint32_t memory_type_index,
                                     VkMemoryPropertyFlags mem_flags,
                                     bool persistent);

void mop_suballoc_free(MopSuballocator *sa, VkDeviceMemory memory,
                       VkDeviceSize offset, VkDeviceSize size);

/* Reset all linear (non-persistent) allocations for a memory type.
 * Called at frame start for per-frame transient buffers. */
void mop_suballoc_reset_linear(MopSuballocator *sa, uint32_t memory_type_index);

void mop_suballoc_get_stats(const MopSuballocator *sa, uint32_t *out_vk_allocs,
                            uint64_t *out_bytes_allocated,
                            uint64_t *out_bytes_used);

#endif /* MOP_HAS_VULKAN */
#endif /* MOP_VULKAN_INTERNAL_H */
