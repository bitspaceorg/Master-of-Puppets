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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define MOP_VK_MAX_PIPELINES 64
#define MOP_VK_MAX_DRAWS_PER_FRAME 1024
#define MOP_VK_UBO_SIZE (512 * 1024)          /* 512 KB per-frame UBO */
#define MOP_VK_STAGING_SIZE (4 * 1024 * 1024) /* 4 MB staging buffer */

/* -------------------------------------------------------------------------
 * Fragment uniform block — matches GLSL FragUniforms layout (std140)
 *
 * Must be kept in sync with mop_solid.frag / mop_wireframe.frag.
 * std140 alignment: vec4=16, float=4 (pad to 4), uint=4, int=4
 * Total: 576 bytes (64 base + 8 lights * 64).
 * Padded to device alignment at runtime.
 * ------------------------------------------------------------------------- */

#define MOP_VK_MAX_FRAG_LIGHTS 8

typedef struct MopVkLight {
  float position[4];  /* vec4: xyz + type (0=dir, 1=point, 2=spot) */
  float direction[4]; /* vec4: xyz + padding                       */
  float color[4];     /* vec4: rgb + intensity                     */
  float params[4];    /* vec4: range, spot_inner_cos, spot_outer_cos, active */
} MopVkLight;         /* 64 bytes per light, std140 aligned */

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
  MopVkLight lights[MOP_VK_MAX_FRAG_LIGHTS]; /* 8 * 64 = 512 bytes   */

  /* Shadow mapping (cascade) */
  int32_t shadows_enabled; /* bool: 1 = shadows active */
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

  /* Pipeline cache: flat array indexed by pipeline key */
  VkPipeline pipelines[MOP_VK_MAX_PIPELINES];
  uint32_t
      pipeline_strides[MOP_VK_MAX_PIPELINES]; /* stride used when created */

  /* Instanced pipeline (single variant — solid, depth-test, backface-cull) */
  VkPipeline instanced_pipeline;
  uint32_t instanced_pipeline_stride;

  /* Descriptor pool (reset per frame) */
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

  /* GPU timestamp queries */
  VkQueryPool timestamp_pool;
  float timestamp_period_ns;
  MopGpuTimingResult last_timing;

  /* Device limits */
  VkDeviceSize min_ubo_alignment;

  /* Feature flags */
  bool has_fill_mode_non_solid;
  bool has_timestamp_queries;
  bool reverse_z;
  VkSampleCountFlagBits msaa_samples;

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

  /* Cached projection matrix (stored during draw, used by SSAO in frame_end) */
  float cached_projection[16];
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
  bool is_hdr; /* true = R32G32B32A32_SFLOAT format */
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

/* Create a VkImageView for an image. Returns VK_SUCCESS or error. */
VkResult mop_vk_create_image_view(VkDevice device, VkImage image,
                                  VkFormat format, VkImageAspectFlags aspect,
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

/* Create the SSAO pipeline (fullscreen triangle + hemisphere sampling). */
VkPipeline mop_vk_create_ssao_pipeline(struct MopRhiDevice *dev);

/* Create the SSAO blur pipeline (fullscreen triangle + bilateral blur). */
VkPipeline mop_vk_create_ssao_blur_pipeline(struct MopRhiDevice *dev);

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

#endif /* MOP_HAS_VULKAN */
#endif /* MOP_VULKAN_INTERNAL_H */
