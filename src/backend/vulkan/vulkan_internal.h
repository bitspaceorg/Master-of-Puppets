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
#include <mop/log.h>

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define MOP_VK_MAX_PIPELINES     64
#define MOP_VK_MAX_DRAWS_PER_FRAME 1024
#define MOP_VK_UBO_SIZE          (512 * 1024)   /* 512 KB per-frame UBO */
#define MOP_VK_STAGING_SIZE      (4 * 1024 * 1024) /* 4 MB staging buffer */

/* -------------------------------------------------------------------------
 * Fragment uniform block — matches GLSL FragUniforms layout (std140)
 *
 * Must be kept in sync with mop_solid.frag / mop_wireframe.frag.
 * std140 alignment: vec4=16, float=4 (pad to 4), uint=4, int=4
 * Total: 304 bytes (48 base + 4 lights * 64).
 * Padded to device alignment at runtime.
 * ------------------------------------------------------------------------- */

#define MOP_VK_MAX_FRAG_LIGHTS 4

typedef struct MopVkLight {
    float position[4];      /* vec4: xyz + type (0=dir, 1=point, 2=spot) */
    float direction[4];     /* vec4: xyz + padding                       */
    float color[4];         /* vec4: rgb + intensity                     */
    float params[4];        /* vec4: range, spot_inner_cos, spot_outer_cos, active */
} MopVkLight;               /* 64 bytes per light, std140 aligned */

typedef struct MopVkFragUniforms {
    float light_dir[4];     /* vec4: xyz + padding          (offset  0) */
    float ambient;          /* float                        (offset 16) */
    float opacity;          /* float                        (offset 20) */
    uint32_t object_id;     /* uint                         (offset 24) */
    int32_t blend_mode;     /* int                          (offset 28) */
    int32_t has_texture;    /* int                          (offset 32) */
    int32_t num_lights;     /* int                          (offset 36) */
    float _pad1;            /*                              (offset 40) */
    float _pad2;            /*                              (offset 44) */
    MopVkLight lights[MOP_VK_MAX_FRAG_LIGHTS]; /* 4 * 64 = 256 bytes   */
} MopVkFragUniforms;        /* Total: 304 bytes */

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
    return ((uint32_t)wireframe)
         | ((uint32_t)depth_test     << 1)
         | ((uint32_t)backface_cull  << 2)
         | ((uint32_t)blend_mode     << 3)
         | (non_standard             << 5);
}

/* -------------------------------------------------------------------------
 * MopRhiDevice — Vulkan device and shared resources
 * ------------------------------------------------------------------------- */

struct MopRhiDevice {
    VkInstance       instance;
    VkPhysicalDevice physical_device;
    VkDevice         device;
    VkQueue          queue;
    uint32_t         queue_family;

    VkPhysicalDeviceMemoryProperties mem_props;
    VkPhysicalDeviceProperties       dev_props;

    /* Command infrastructure */
    VkCommandPool   cmd_pool;
    VkCommandBuffer cmd_buf;
    VkFence         fence;

    /* Shader modules */
    VkShaderModule  solid_vert;
    VkShaderModule  solid_frag;
    VkShaderModule  wireframe_frag;

    /* Shared pipeline state */
    VkPipelineLayout      pipeline_layout;
    VkDescriptorSetLayout desc_set_layout;
    VkRenderPass          render_pass;

    /* Pipeline cache: flat array indexed by pipeline key */
    VkPipeline pipelines[MOP_VK_MAX_PIPELINES];
    uint32_t   pipeline_strides[MOP_VK_MAX_PIPELINES]; /* stride used when created */

    /* Descriptor pool (reset per frame) */
    VkDescriptorPool desc_pool;

    /* Default sampler (linear, repeat) */
    VkSampler default_sampler;

    /* 1x1 white fallback texture */
    VkImage        white_image;
    VkDeviceMemory white_memory;
    VkImageView    white_view;

    /* Staging buffer for uploads */
    VkBuffer       staging_buf;
    VkDeviceMemory staging_mem;
    void          *staging_mapped;

    /* Device limits */
    VkDeviceSize min_ubo_alignment;

    /* Feature flags */
    bool has_fill_mode_non_solid;
};

/* -------------------------------------------------------------------------
 * MopRhiFramebuffer — Offscreen render target with readback
 * ------------------------------------------------------------------------- */

struct MopRhiFramebuffer {
    int width;
    int height;

    /* Color attachment (R8G8B8A8_SRGB — linear→sRGB on write) */
    VkImage        color_image;
    VkDeviceMemory color_memory;
    VkImageView    color_view;

    /* Picking attachment (R32_UINT) */
    VkImage        pick_image;
    VkDeviceMemory pick_memory;
    VkImageView    pick_view;

    /* Depth attachment (D32_SFLOAT) */
    VkImage        depth_image;
    VkDeviceMemory depth_memory;
    VkImageView    depth_view;

    VkFramebuffer  framebuffer;

    /* Readback staging buffers (host-visible, persistently mapped) */
    VkBuffer       readback_color_buf;
    VkDeviceMemory readback_color_mem;
    void          *readback_color_mapped;

    VkBuffer       readback_pick_buf;
    VkDeviceMemory readback_pick_mem;
    void          *readback_pick_mapped;

    VkBuffer       readback_depth_buf;
    VkDeviceMemory readback_depth_mem;
    void          *readback_depth_mapped;

    /* CPU-side readback arrays */
    uint8_t  *readback_color;
    uint32_t *readback_pick;
    float    *readback_depth;

    /* Per-frame dynamic UBO (host-visible, persistently mapped) */
    VkBuffer       ubo_buf;
    VkDeviceMemory ubo_mem;
    void          *ubo_mapped;
    VkDeviceSize   ubo_offset;   /* current write offset */
};

/* -------------------------------------------------------------------------
 * MopRhiBuffer — Device-local buffer with CPU shadow
 * ------------------------------------------------------------------------- */

struct MopRhiBuffer {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    size_t         size;
    void          *shadow;  /* CPU-side copy (matches OpenGL pattern) */
};

/* -------------------------------------------------------------------------
 * MopRhiTexture — Device-local image with view
 * ------------------------------------------------------------------------- */

struct MopRhiTexture {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    int            width;
    int            height;
};

/* -------------------------------------------------------------------------
 * Helper declarations — vulkan_memory.c
 * ------------------------------------------------------------------------- */

/* Find a memory type index matching requirements and property flags.
 * Returns -1 on failure. */
int mop_vk_find_memory_type(const VkPhysicalDeviceMemoryProperties *props,
                             uint32_t type_filter,
                             VkMemoryPropertyFlags flags);

/* Create a VkBuffer + VkDeviceMemory pair. Returns VK_SUCCESS or error. */
VkResult mop_vk_create_buffer(VkDevice device,
                               const VkPhysicalDeviceMemoryProperties *props,
                               VkDeviceSize size,
                               VkBufferUsageFlags usage,
                               VkMemoryPropertyFlags mem_flags,
                               VkBuffer *out_buffer,
                               VkDeviceMemory *out_memory);

/* Create a VkImage + VkDeviceMemory pair. Returns VK_SUCCESS or error. */
VkResult mop_vk_create_image(VkDevice device,
                              const VkPhysicalDeviceMemoryProperties *props,
                              uint32_t width, uint32_t height,
                              VkFormat format,
                              VkImageUsageFlags usage,
                              VkImage *out_image,
                              VkDeviceMemory *out_memory);

/* Create a VkImageView for an image. Returns VK_SUCCESS or error. */
VkResult mop_vk_create_image_view(VkDevice device,
                                   VkImage image,
                                   VkFormat format,
                                   VkImageAspectFlags aspect,
                                   VkImageView *out_view);

/* -------------------------------------------------------------------------
 * Helper declarations — vulkan_pipeline.c
 * ------------------------------------------------------------------------- */

/* Create the render pass with color + picking + depth attachments. */
VkResult mop_vk_create_render_pass(VkDevice device, VkRenderPass *out);

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
                                       VkCommandPool pool,
                                       VkCommandBuffer cb) {
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

static inline void mop_vk_transition_image(VkCommandBuffer cb,
                                            VkImage image,
                                            VkImageAspectFlags aspect,
                                            VkImageLayout old_layout,
                                            VkImageLayout new_layout,
                                            VkAccessFlags src_access,
                                            VkAccessFlags dst_access,
                                            VkPipelineStageFlags src_stage,
                                            VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0,
                         0, NULL, 0, NULL, 1, &barrier);
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
