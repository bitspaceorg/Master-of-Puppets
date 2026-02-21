/*
 * Master of Puppets — Vulkan Backend
 * vulkan_backend.c — Full RHI implementation via Vulkan 1.0
 *
 * This backend is compiled only when MOP_ENABLE_VULKAN=1 is set.
 * It renders offscreen — no swapchain/WSI needed.  The application
 * reads back RGBA8 via framebuffer_read_color and blits to SDL.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(MOP_HAS_VULKAN)

#include "vulkan_internal.h"
#include "vulkan_shaders.h"

#include <stdio.h>
#include <math.h>

/* =========================================================================
 * Helper: create VkShaderModule from SPIR-V
 * ========================================================================= */

static VkShaderModule create_shader_module(VkDevice device,
                                            const uint32_t *code,
                                            size_t size) {
    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = code,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device, &ci, NULL, &mod);
    if (r != VK_SUCCESS) {
        MOP_ERROR("[VK] shader module creation failed: %d", r);
        return VK_NULL_HANDLE;
    }
    return mod;
}

/* =========================================================================
 * Helper: upload data through staging buffer
 * ========================================================================= */

static void staging_upload(MopRhiDevice *dev, VkBuffer dst,
                            const void *data, size_t size) {
    if (size > MOP_VK_STAGING_SIZE) {
        MOP_ERROR("[VK] staging upload too large: %zu > %d",
                  size, MOP_VK_STAGING_SIZE);
        return;
    }

    memcpy(dev->staging_mapped, data, size);

    VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);

    VkBufferCopy region = { .size = size };
    vkCmdCopyBuffer(cb, dev->staging_buf, dst, 1, &region);

    mop_vk_end_oneshot(dev->device, dev->queue, dev->cmd_pool, cb);
}

/* =========================================================================
 * Helper: upload image data through staging buffer
 * ========================================================================= */

static void staging_upload_image(MopRhiDevice *dev, VkImage image,
                                  uint32_t width, uint32_t height,
                                  const uint8_t *rgba_data) {
    size_t size = (size_t)width * height * 4;
    if (size > MOP_VK_STAGING_SIZE) {
        MOP_ERROR("[VK] image staging upload too large: %zu", size);
        return;
    }

    memcpy(dev->staging_mapped, rgba_data, size);

    VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);

    /* Transition to TRANSFER_DST */
    mop_vk_transition_image(cb, image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region = {
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageExtent = { width, height, 1 },
    };
    vkCmdCopyBufferToImage(cb, dev->staging_buf, image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    /* Transition to SHADER_READ_ONLY */
    mop_vk_transition_image(cb, image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    mop_vk_end_oneshot(dev->device, dev->queue, dev->cmd_pool, cb);
}

/* =========================================================================
 * 1. device_create
 * ========================================================================= */

static MopRhiDevice *vk_device_create(void) {
    MopRhiDevice *dev = calloc(1, sizeof(MopRhiDevice));
    if (!dev) return NULL;

    /* ---- Instance ---- */
    VkApplicationInfo app_info = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "Master of Puppets",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName        = "MOP",
        .engineVersion      = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion         = VK_API_VERSION_1_1,
    };

    /* Request validation layers in debug builds (optional — fall back if
     * the layer is not installed on the system) */
    uint32_t layer_count = 0;
    const char *const *layer_names = NULL;

#ifndef NDEBUG
    static const char *validation_layer = "VK_LAYER_KHRONOS_validation";
    {
        uint32_t avail_count = 0;
        vkEnumerateInstanceLayerProperties(&avail_count, NULL);
        if (avail_count > 0) {
            VkLayerProperties *avail = malloc(avail_count * sizeof(*avail));
            vkEnumerateInstanceLayerProperties(&avail_count, avail);
            for (uint32_t i = 0; i < avail_count; i++) {
                if (strcmp(avail[i].layerName, validation_layer) == 0) {
                    layer_count = 1;
                    layer_names = &validation_layer;
                    break;
                }
            }
            free(avail);
        }
        if (layer_count > 0) {
            MOP_INFO("[VK] validation layers enabled");
        } else {
            MOP_DEBUG("[VK] validation layers not available, skipping");
        }
    }
#endif

    VkInstanceCreateInfo inst_ci = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &app_info,
        .enabledLayerCount       = layer_count,
        .ppEnabledLayerNames     = layer_names,
#if defined(__APPLE__)
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
#endif
    };

    /* MoltenVK portability extension */
#if defined(__APPLE__)
    const char *inst_exts[] = {
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
    };
    inst_ci.enabledExtensionCount   = 1;
    inst_ci.ppEnabledExtensionNames = inst_exts;
#endif

    VkResult r = vkCreateInstance(&inst_ci, NULL, &dev->instance);
    if (r != VK_SUCCESS) {
        MOP_ERROR("[VK] vkCreateInstance failed: %d", r);
        free(dev);
        return NULL;
    }

    /* ---- Physical device ---- */
    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(dev->instance, &gpu_count, NULL);
    if (gpu_count == 0) {
        MOP_ERROR("[VK] no Vulkan-capable GPU found");
        vkDestroyInstance(dev->instance, NULL);
        free(dev);
        return NULL;
    }

    VkPhysicalDevice *gpus = malloc(gpu_count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(dev->instance, &gpu_count, gpus);
    dev->physical_device = gpus[0]; /* pick first */
    free(gpus);

    vkGetPhysicalDeviceMemoryProperties(dev->physical_device, &dev->mem_props);
    vkGetPhysicalDeviceProperties(dev->physical_device, &dev->dev_props);
    dev->min_ubo_alignment = dev->dev_props.limits.minUniformBufferOffsetAlignment;
    if (dev->min_ubo_alignment == 0) dev->min_ubo_alignment = 256;

    MOP_INFO("[VK] GPU: %s", dev->dev_props.deviceName);

    /* Check for fillModeNonSolid (wireframe support) */
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(dev->physical_device, &features);
    dev->has_fill_mode_non_solid = features.fillModeNonSolid;

    /* ---- Queue family ---- */
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical_device,
                                              &qf_count, NULL);
    VkQueueFamilyProperties *qf_props = malloc(qf_count * sizeof(*qf_props));
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical_device,
                                              &qf_count, qf_props);

    dev->queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            dev->queue_family = i;
            break;
        }
    }
    free(qf_props);

    if (dev->queue_family == UINT32_MAX) {
        MOP_ERROR("[VK] no graphics queue family found");
        vkDestroyInstance(dev->instance, NULL);
        free(dev);
        return NULL;
    }

    /* ---- Logical device ---- */
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = dev->queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &priority,
    };

    VkPhysicalDeviceFeatures enabled_features = {0};
    if (dev->has_fill_mode_non_solid) {
        enabled_features.fillModeNonSolid = VK_TRUE;
    }

    /* Device extensions for MoltenVK portability */
    const char *dev_exts[] = {
#if defined(__APPLE__)
        "VK_KHR_portability_subset",
#endif
    };
    uint32_t dev_ext_count = sizeof(dev_exts) / sizeof(dev_exts[0]);

    VkDeviceCreateInfo dev_ci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &queue_ci,
        .pEnabledFeatures        = &enabled_features,
        .enabledExtensionCount   = dev_ext_count,
        .ppEnabledExtensionNames = dev_ext_count > 0 ? dev_exts : NULL,
    };

    r = vkCreateDevice(dev->physical_device, &dev_ci, NULL, &dev->device);
    if (r != VK_SUCCESS) {
        MOP_ERROR("[VK] vkCreateDevice failed: %d", r);
        vkDestroyInstance(dev->instance, NULL);
        free(dev);
        return NULL;
    }

    vkGetDeviceQueue(dev->device, dev->queue_family, 0, &dev->queue);

    /* ---- Command pool + buffer + fence ---- */
    VkCommandPoolCreateInfo pool_ci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = dev->queue_family,
    };
    r = vkCreateCommandPool(dev->device, &pool_ci, NULL, &dev->cmd_pool);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] vkCreateCommandPool failed: %d", r); goto fail; }

    VkCommandBufferAllocateInfo cb_ai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = dev->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    r = vkAllocateCommandBuffers(dev->device, &cb_ai, &dev->cmd_buf);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] vkAllocateCommandBuffers failed: %d", r); goto fail; }

    VkFenceCreateInfo fence_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    r = vkCreateFence(dev->device, &fence_ci, NULL, &dev->fence);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] vkCreateFence failed: %d", r); goto fail; }

    /* ---- Shader modules ---- */
    dev->solid_vert = create_shader_module(dev->device,
        mop_solid_vert_spv, mop_solid_vert_spv_size);
    dev->solid_frag = create_shader_module(dev->device,
        mop_solid_frag_spv, mop_solid_frag_spv_size);
    dev->wireframe_frag = create_shader_module(dev->device,
        mop_wireframe_frag_spv, mop_wireframe_frag_spv_size);

    if (!dev->solid_vert || !dev->solid_frag || !dev->wireframe_frag) {
        MOP_ERROR("[VK] shader module creation failed");
        /* device_destroy handles partial cleanup */
        goto fail;
    }

    /* ---- Render pass, layouts ---- */
    r = mop_vk_create_render_pass(dev->device, &dev->render_pass);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] render pass: %d", r); goto fail; }

    r = mop_vk_create_desc_set_layout(dev->device, &dev->desc_set_layout);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] desc layout: %d", r); goto fail; }

    r = mop_vk_create_pipeline_layout(dev->device, dev->desc_set_layout,
                                       &dev->pipeline_layout);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] pipeline layout: %d", r); goto fail; }

    /* ---- Descriptor pool ---- */
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, MOP_VK_MAX_DRAWS_PER_FRAME },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MOP_VK_MAX_DRAWS_PER_FRAME },
    };
    VkDescriptorPoolCreateInfo dp_ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = MOP_VK_MAX_DRAWS_PER_FRAME,
        .poolSizeCount = 2,
        .pPoolSizes    = pool_sizes,
    };
    r = vkCreateDescriptorPool(dev->device, &dp_ci, NULL, &dev->desc_pool);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] desc pool: %d", r); goto fail; }

    /* ---- Default sampler (linear, repeat) ---- */
    VkSamplerCreateInfo samp_ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod       = 1.0f,
    };
    r = vkCreateSampler(dev->device, &samp_ci, NULL, &dev->default_sampler);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] vkCreateSampler failed: %d", r); goto fail; }

    /* ---- Staging buffer (4 MB host-visible) ---- */
    r = mop_vk_create_buffer(dev->device, &dev->mem_props,
        MOP_VK_STAGING_SIZE,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &dev->staging_buf, &dev->staging_mem);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] staging buffer: %d", r); goto fail; }
    r = vkMapMemory(dev->device, dev->staging_mem, 0, MOP_VK_STAGING_SIZE, 0,
                    &dev->staging_mapped);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] vkMapMemory staging failed: %d", r); goto fail; }

    /* ---- 1x1 white fallback texture ---- */
    r = mop_vk_create_image(dev->device, &dev->mem_props, 1, 1,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        &dev->white_image, &dev->white_memory);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] white image: %d", r); goto fail; }

    r = mop_vk_create_image_view(dev->device, dev->white_image,
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
        &dev->white_view);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] white view: %d", r); goto fail; }

    /* Upload white pixel */
    {
        uint8_t white[4] = { 255, 255, 255, 255 };
        staging_upload_image(dev, dev->white_image, 1, 1, white);
    }

    MOP_INFO("[VK] device created successfully");
    return dev;

fail:
    /* Partial cleanup — destroy what was created */
    {
        /* Use the destroy function via forward declaration — but since we're
         * in the same TU, just inline cleanup of what was allocated. */
        VkDevice d = dev->device;
        if (d) {
            vkDeviceWaitIdle(d);

            if (dev->staging_mapped)
                vkUnmapMemory(d, dev->staging_mem);
            if (dev->staging_buf)
                vkDestroyBuffer(d, dev->staging_buf, NULL);
            if (dev->staging_mem)
                vkFreeMemory(d, dev->staging_mem, NULL);

            if (dev->white_view)
                vkDestroyImageView(d, dev->white_view, NULL);
            if (dev->white_image)
                vkDestroyImage(d, dev->white_image, NULL);
            if (dev->white_memory)
                vkFreeMemory(d, dev->white_memory, NULL);

            if (dev->default_sampler)
                vkDestroySampler(d, dev->default_sampler, NULL);
            if (dev->desc_pool)
                vkDestroyDescriptorPool(d, dev->desc_pool, NULL);
            if (dev->pipeline_layout)
                vkDestroyPipelineLayout(d, dev->pipeline_layout, NULL);
            if (dev->desc_set_layout)
                vkDestroyDescriptorSetLayout(d, dev->desc_set_layout, NULL);
            if (dev->render_pass)
                vkDestroyRenderPass(d, dev->render_pass, NULL);

            if (dev->solid_vert)
                vkDestroyShaderModule(d, dev->solid_vert, NULL);
            if (dev->solid_frag)
                vkDestroyShaderModule(d, dev->solid_frag, NULL);
            if (dev->wireframe_frag)
                vkDestroyShaderModule(d, dev->wireframe_frag, NULL);

            if (dev->fence)
                vkDestroyFence(d, dev->fence, NULL);
            if (dev->cmd_pool)
                vkDestroyCommandPool(d, dev->cmd_pool, NULL);

            vkDestroyDevice(d, NULL);
        }
        if (dev->instance)
            vkDestroyInstance(dev->instance, NULL);
    }
    free(dev);
    return NULL;
}

/* =========================================================================
 * 2. device_destroy
 * ========================================================================= */

static void vk_device_destroy(MopRhiDevice *dev) {
    if (!dev) return;

    VkDevice d = dev->device;
    if (d) {
        vkDeviceWaitIdle(d);

        /* Pipelines */
        for (int i = 0; i < MOP_VK_MAX_PIPELINES; i++) {
            if (dev->pipelines[i])
                vkDestroyPipeline(d, dev->pipelines[i], NULL);
        }

        /* Staging */
        if (dev->staging_mapped)
            vkUnmapMemory(d, dev->staging_mem);
        if (dev->staging_buf)
            vkDestroyBuffer(d, dev->staging_buf, NULL);
        if (dev->staging_mem)
            vkFreeMemory(d, dev->staging_mem, NULL);

        /* White texture */
        if (dev->white_view)
            vkDestroyImageView(d, dev->white_view, NULL);
        if (dev->white_image)
            vkDestroyImage(d, dev->white_image, NULL);
        if (dev->white_memory)
            vkFreeMemory(d, dev->white_memory, NULL);

        if (dev->default_sampler)
            vkDestroySampler(d, dev->default_sampler, NULL);
        if (dev->desc_pool)
            vkDestroyDescriptorPool(d, dev->desc_pool, NULL);
        if (dev->pipeline_layout)
            vkDestroyPipelineLayout(d, dev->pipeline_layout, NULL);
        if (dev->desc_set_layout)
            vkDestroyDescriptorSetLayout(d, dev->desc_set_layout, NULL);
        if (dev->render_pass)
            vkDestroyRenderPass(d, dev->render_pass, NULL);

        if (dev->solid_vert)
            vkDestroyShaderModule(d, dev->solid_vert, NULL);
        if (dev->solid_frag)
            vkDestroyShaderModule(d, dev->solid_frag, NULL);
        if (dev->wireframe_frag)
            vkDestroyShaderModule(d, dev->wireframe_frag, NULL);

        if (dev->fence)
            vkDestroyFence(d, dev->fence, NULL);
        if (dev->cmd_pool)
            vkDestroyCommandPool(d, dev->cmd_pool, NULL);

        vkDestroyDevice(d, NULL);
    }

    if (dev->instance)
        vkDestroyInstance(dev->instance, NULL);

    free(dev);
}

/* =========================================================================
 * 3. buffer_create
 * ========================================================================= */

static MopRhiBuffer *vk_buffer_create(MopRhiDevice *dev,
                                       const MopRhiBufferDesc *desc) {
    MopRhiBuffer *buf = calloc(1, sizeof(MopRhiBuffer));
    if (!buf) return NULL;

    buf->size = desc->size;

    /* Shadow copy */
    buf->shadow = malloc(desc->size);
    if (!buf->shadow) { free(buf); return NULL; }
    memcpy(buf->shadow, desc->data, desc->size);

    /* Device-local buffer */
    VkResult r = mop_vk_create_buffer(dev->device, &dev->mem_props,
        desc->size,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &buf->buffer, &buf->memory);
    if (r != VK_SUCCESS) {
        MOP_ERROR("[VK] buffer_create failed: %d", r);
        free(buf->shadow);
        free(buf);
        return NULL;
    }

    /* Upload via staging */
    staging_upload(dev, buf->buffer, desc->data, desc->size);

    return buf;
}

/* =========================================================================
 * 4. buffer_destroy
 * ========================================================================= */

static void vk_buffer_destroy(MopRhiDevice *dev, MopRhiBuffer *buf) {
    if (!buf) return;
    if (dev && dev->device) {
        vkDestroyBuffer(dev->device, buf->buffer, NULL);
        vkFreeMemory(dev->device, buf->memory, NULL);
    }
    free(buf->shadow);
    free(buf);
}

/* =========================================================================
 * 5. buffer_update
 * ========================================================================= */

static void vk_buffer_update(MopRhiDevice *dev, MopRhiBuffer *buf,
                               const void *data, size_t offset, size_t size) {
    if (!buf || !data) return;

    /* Update shadow */
    memcpy((uint8_t *)buf->shadow + offset, data, size);

    /* Upload via staging — for partial updates, copy to staging with offset */
    if (size > MOP_VK_STAGING_SIZE) {
        MOP_ERROR("[VK] buffer_update too large: %zu", size);
        return;
    }

    memcpy(dev->staging_mapped, data, size);

    VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);
    VkBufferCopy region = { .srcOffset = 0, .dstOffset = offset, .size = size };
    vkCmdCopyBuffer(cb, dev->staging_buf, buf->buffer, 1, &region);

    /* Memory barrier: ensure the transfer write is visible to subsequent
     * vertex attribute reads.  Required on MoltenVK where implicit
     * synchronization between submissions may not flush GPU caches. */
    VkBufferMemoryBarrier barrier = {
        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                               VK_ACCESS_INDEX_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer              = buf->buffer,
        .offset              = offset,
        .size                = size,
    };
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0, 0, NULL, 1, &barrier, 0, NULL);

    mop_vk_end_oneshot(dev->device, dev->queue, dev->cmd_pool, cb);
}

/* =========================================================================
 * Framebuffer helpers
 * ========================================================================= */

static void vk_fb_create_attachments(MopRhiDevice *dev,
                                      MopRhiFramebuffer *fb,
                                      int width, int height) {
    fb->width  = width;
    fb->height = height;
    size_t npixels = (size_t)width * (size_t)height;

    VkResult r;

    /* ---- Color (R8G8B8A8_SRGB — hardware linear→sRGB on write) ---- */
    r = mop_vk_create_image(dev->device, &dev->mem_props,
        (uint32_t)width, (uint32_t)height, VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        &fb->color_image, &fb->color_memory);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] fb color image: %d", r); return; }

    r = mop_vk_create_image_view(dev->device, fb->color_image,
        VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, &fb->color_view);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] fb color view: %d", r); return; }

    /* ---- Picking (R32_UINT) ---- */
    r = mop_vk_create_image(dev->device, &dev->mem_props,
        (uint32_t)width, (uint32_t)height, VK_FORMAT_R32_UINT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        &fb->pick_image, &fb->pick_memory);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] fb pick image: %d", r); return; }

    r = mop_vk_create_image_view(dev->device, fb->pick_image,
        VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, &fb->pick_view);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] fb pick view: %d", r); return; }

    /* ---- Depth (D32_SFLOAT) ---- */
    r = mop_vk_create_image(dev->device, &dev->mem_props,
        (uint32_t)width, (uint32_t)height, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        &fb->depth_image, &fb->depth_memory);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] fb depth image: %d", r); return; }

    r = mop_vk_create_image_view(dev->device, fb->depth_image,
        VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, &fb->depth_view);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] fb depth view: %d", r); return; }

    /* ---- VkFramebuffer ---- */
    VkImageView views[3] = { fb->color_view, fb->pick_view, fb->depth_view };
    VkFramebufferCreateInfo fb_ci = {
        .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass      = dev->render_pass,
        .attachmentCount = 3,
        .pAttachments    = views,
        .width           = (uint32_t)width,
        .height          = (uint32_t)height,
        .layers          = 1,
    };
    r = vkCreateFramebuffer(dev->device, &fb_ci, NULL, &fb->framebuffer);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] vkCreateFramebuffer failed: %d", r); return; }

    /* ---- Readback staging buffers (host-visible, persistently mapped) ---- */
    size_t color_size = npixels * 4;
    size_t pick_size  = npixels * sizeof(uint32_t);
    size_t depth_size = npixels * sizeof(float);

    r = mop_vk_create_buffer(dev->device, &dev->mem_props, color_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &fb->readback_color_buf, &fb->readback_color_mem);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] fb readback color buffer: %d", r); return; }
    r = vkMapMemory(dev->device, fb->readback_color_mem, 0, color_size, 0,
                    &fb->readback_color_mapped);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] vkMapMemory readback color failed: %d", r); return; }

    r = mop_vk_create_buffer(dev->device, &dev->mem_props, pick_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &fb->readback_pick_buf, &fb->readback_pick_mem);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] fb readback pick buffer: %d", r); return; }
    r = vkMapMemory(dev->device, fb->readback_pick_mem, 0, pick_size, 0,
                    &fb->readback_pick_mapped);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] vkMapMemory readback pick failed: %d", r); return; }

    r = mop_vk_create_buffer(dev->device, &dev->mem_props, depth_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &fb->readback_depth_buf, &fb->readback_depth_mem);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] fb readback depth buffer: %d", r); return; }
    r = vkMapMemory(dev->device, fb->readback_depth_mem, 0, depth_size, 0,
                    &fb->readback_depth_mapped);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] vkMapMemory readback depth failed: %d", r); return; }

    /* ---- CPU readback arrays ---- */
    fb->readback_color = malloc(color_size);
    if (!fb->readback_color) { MOP_ERROR("[VK] malloc readback_color failed"); return; }
    fb->readback_pick  = malloc(pick_size);
    if (!fb->readback_pick) { MOP_ERROR("[VK] malloc readback_pick failed"); return; }
    fb->readback_depth = malloc(depth_size);
    if (!fb->readback_depth) { MOP_ERROR("[VK] malloc readback_depth failed"); return; }

    /* ---- Per-frame dynamic UBO ---- */
    r = mop_vk_create_buffer(dev->device, &dev->mem_props, MOP_VK_UBO_SIZE,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &fb->ubo_buf, &fb->ubo_mem);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] fb UBO buffer: %d", r); return; }
    r = vkMapMemory(dev->device, fb->ubo_mem, 0, MOP_VK_UBO_SIZE, 0,
                    &fb->ubo_mapped);
    if (r != VK_SUCCESS) { MOP_ERROR("[VK] vkMapMemory UBO failed: %d", r); return; }
    fb->ubo_offset = 0;
}

static void vk_fb_destroy_attachments(MopRhiDevice *dev,
                                       MopRhiFramebuffer *fb) {
    VkDevice d = dev->device;

    if (fb->framebuffer) vkDestroyFramebuffer(d, fb->framebuffer, NULL);

    /* Color */
    if (fb->color_view) vkDestroyImageView(d, fb->color_view, NULL);
    if (fb->color_image) vkDestroyImage(d, fb->color_image, NULL);
    if (fb->color_memory) vkFreeMemory(d, fb->color_memory, NULL);

    /* Pick */
    if (fb->pick_view) vkDestroyImageView(d, fb->pick_view, NULL);
    if (fb->pick_image) vkDestroyImage(d, fb->pick_image, NULL);
    if (fb->pick_memory) vkFreeMemory(d, fb->pick_memory, NULL);

    /* Depth */
    if (fb->depth_view) vkDestroyImageView(d, fb->depth_view, NULL);
    if (fb->depth_image) vkDestroyImage(d, fb->depth_image, NULL);
    if (fb->depth_memory) vkFreeMemory(d, fb->depth_memory, NULL);

    /* Readback staging */
    if (fb->readback_color_mapped)
        vkUnmapMemory(d, fb->readback_color_mem);
    if (fb->readback_color_buf)
        vkDestroyBuffer(d, fb->readback_color_buf, NULL);
    if (fb->readback_color_mem)
        vkFreeMemory(d, fb->readback_color_mem, NULL);

    if (fb->readback_pick_mapped)
        vkUnmapMemory(d, fb->readback_pick_mem);
    if (fb->readback_pick_buf)
        vkDestroyBuffer(d, fb->readback_pick_buf, NULL);
    if (fb->readback_pick_mem)
        vkFreeMemory(d, fb->readback_pick_mem, NULL);

    if (fb->readback_depth_mapped)
        vkUnmapMemory(d, fb->readback_depth_mem);
    if (fb->readback_depth_buf)
        vkDestroyBuffer(d, fb->readback_depth_buf, NULL);
    if (fb->readback_depth_mem)
        vkFreeMemory(d, fb->readback_depth_mem, NULL);

    /* CPU readback */
    free(fb->readback_color);
    free(fb->readback_pick);
    free(fb->readback_depth);

    /* UBO */
    if (fb->ubo_mapped) vkUnmapMemory(d, fb->ubo_mem);
    if (fb->ubo_buf) vkDestroyBuffer(d, fb->ubo_buf, NULL);
    if (fb->ubo_mem) vkFreeMemory(d, fb->ubo_mem, NULL);

    memset(fb, 0, sizeof(*fb));
}

/* =========================================================================
 * 6. framebuffer_create
 * ========================================================================= */

static MopRhiFramebuffer *vk_framebuffer_create(MopRhiDevice *dev,
                                                  const MopRhiFramebufferDesc *desc) {
    MopRhiFramebuffer *fb = calloc(1, sizeof(MopRhiFramebuffer));
    if (!fb) return NULL;

    vk_fb_create_attachments(dev, fb, desc->width, desc->height);
    return fb;
}

/* =========================================================================
 * 7. framebuffer_destroy
 * ========================================================================= */

static void vk_framebuffer_destroy(MopRhiDevice *dev, MopRhiFramebuffer *fb) {
    if (!fb) return;
    if (dev && dev->device) {
        vkDeviceWaitIdle(dev->device);
        vk_fb_destroy_attachments(dev, fb);
    }
    free(fb);
}

/* =========================================================================
 * 8. framebuffer_resize
 * ========================================================================= */

static void vk_framebuffer_resize(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                                   int width, int height) {
    if (!fb) return;
    vkDeviceWaitIdle(dev->device);
    vk_fb_destroy_attachments(dev, fb);
    vk_fb_create_attachments(dev, fb, width, height);
}

/* =========================================================================
 * 9. frame_begin
 * ========================================================================= */

static void vk_frame_begin(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                             MopColor clear_color) {
    /* Wait for previous frame to finish */
    vkWaitForFences(dev->device, 1, &dev->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev->device, 1, &dev->fence);

    /* Reset descriptor pool for this frame */
    vkResetDescriptorPool(dev->device, dev->desc_pool, 0);

    /* Reset UBO offset */
    fb->ubo_offset = 0;

    /* Begin command buffer */
    vkResetCommandBuffer(dev->cmd_buf, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(dev->cmd_buf, &begin_info);

    /* Begin render pass */
    VkClearValue clears[3] = {
        { .color = {{ clear_color.r, clear_color.g,
                      clear_color.b, clear_color.a }} },
        { .color = {{ 0 }} },  /* picking = 0 */
        { .depthStencil = { 1.0f, 0 } },
    };

    VkRenderPassBeginInfo rp_info = {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass  = dev->render_pass,
        .framebuffer = fb->framebuffer,
        .renderArea  = { .extent = { (uint32_t)fb->width,
                                     (uint32_t)fb->height } },
        .clearValueCount = 3,
        .pClearValues    = clears,
    };
    vkCmdBeginRenderPass(dev->cmd_buf, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

    /* Set dynamic viewport + scissor */
    /* Negative viewport height flips Y to match OpenGL/CPU clip space
     * conventions.  This is core in Vulkan 1.1 (VK_KHR_maintenance1).
     * Without this, the scene is upside-down and winding order is reversed,
     * breaking backface culling. */
    VkViewport viewport = {
        .x = 0, .y = (float)fb->height,
        .width    = (float)fb->width,
        .height   = -(float)fb->height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(dev->cmd_buf, 0, 1, &viewport);

    VkRect2D scissor = {
        .extent = { (uint32_t)fb->width, (uint32_t)fb->height },
    };
    vkCmdSetScissor(dev->cmd_buf, 0, 1, &scissor);
}

/* =========================================================================
 * 10. draw
 * ========================================================================= */

static void vk_draw(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                      const MopRhiDrawCall *call) {
    /* Determine vertex stride from format (or default to MopVertex) */
    uint32_t vertex_stride = call->vertex_format
        ? (uint32_t)call->vertex_format->stride
        : (uint32_t)sizeof(MopVertex);

    /* Select pipeline */
    uint32_t key = mop_vk_pipeline_key(call->wireframe, call->depth_test,
                                        call->backface_cull, call->blend_mode,
                                        vertex_stride);
    VkPipeline pipeline = mop_vk_get_pipeline(dev, key, vertex_stride);
    if (!pipeline) return;

    vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    /* Push constants: mat4 mvp + mat4 model = 128 bytes */
    float push_data[32];
    memcpy(push_data, call->mvp.d, 64);
    memcpy(push_data + 16, call->model.d, 64);
    vkCmdPushConstants(dev->cmd_buf, dev->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, 128, push_data);

    /* Write fragment UBO at current offset */
    VkDeviceSize aligned_size = mop_vk_align(sizeof(MopVkFragUniforms),
                                              dev->min_ubo_alignment);
    if (fb->ubo_offset + aligned_size > MOP_VK_UBO_SIZE) {
        MOP_WARN("[VK] UBO exhausted, skipping draw");
        return;
    }

    MopVkFragUniforms *ubo = (MopVkFragUniforms *)
        ((uint8_t *)fb->ubo_mapped + fb->ubo_offset);
    ubo->light_dir[0] = call->light_dir.x;
    ubo->light_dir[1] = call->light_dir.y;
    ubo->light_dir[2] = call->light_dir.z;
    ubo->light_dir[3] = 0.0f;
    ubo->ambient      = call->ambient;
    ubo->opacity       = call->opacity;
    ubo->object_id     = call->object_id;
    ubo->blend_mode    = (int32_t)call->blend_mode;
    ubo->has_texture   = call->texture ? 1 : 0;
    ubo->_pad1 = 0.0f;
    ubo->_pad2 = 0.0f;

    /* Multi-light: populate light array from draw call */
    ubo->num_lights = (int32_t)(call->light_count < MOP_VK_MAX_FRAG_LIGHTS
                                ? call->light_count : MOP_VK_MAX_FRAG_LIGHTS);
    for (int i = 0; i < ubo->num_lights; i++) {
        const MopLight *src = &call->lights[i];
        MopVkLight *dst = &ubo->lights[i];
        dst->position[0]  = src->position.x;
        dst->position[1]  = src->position.y;
        dst->position[2]  = src->position.z;
        dst->position[3]  = (float)src->type;
        dst->direction[0] = src->direction.x;
        dst->direction[1] = src->direction.y;
        dst->direction[2] = src->direction.z;
        dst->direction[3] = 0.0f;
        dst->color[0]     = src->color.r;
        dst->color[1]     = src->color.g;
        dst->color[2]     = src->color.b;
        dst->color[3]     = src->intensity;
        dst->params[0]    = src->range;
        dst->params[1]    = src->spot_inner_cos;
        dst->params[2]    = src->spot_outer_cos;
        dst->params[3]    = src->active ? 1.0f : 0.0f;
    }
    /* Zero unused light slots */
    for (int i = ubo->num_lights; i < MOP_VK_MAX_FRAG_LIGHTS; i++) {
        memset(&ubo->lights[i], 0, sizeof(MopVkLight));
    }

    uint32_t dynamic_offset = (uint32_t)fb->ubo_offset;
    fb->ubo_offset += aligned_size;

    /* Allocate descriptor set */
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &dev->desc_set_layout,
    };
    VkDescriptorSet ds;
    VkResult r = vkAllocateDescriptorSets(dev->device, &ds_ai, &ds);
    if (r != VK_SUCCESS) {
        MOP_WARN("[VK] descriptor set alloc failed: %d", r);
        return;
    }

    /* Update descriptor set */
    VkDescriptorBufferInfo buf_info = {
        .buffer = fb->ubo_buf,
        .offset = 0,
        .range  = sizeof(MopVkFragUniforms),
    };

    VkDescriptorImageInfo img_info = {
        .sampler     = dev->default_sampler,
        .imageView   = call->texture ? call->texture->view : dev->white_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet writes[2] = {
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = ds,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .pBufferInfo     = &buf_info,
        },
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = ds,
            .dstBinding      = 1,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &img_info,
        },
    };
    vkUpdateDescriptorSets(dev->device, 2, writes, 0, NULL);

    /* Bind descriptor set with dynamic offset */
    vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->pipeline_layout, 0, 1, &ds,
                            1, &dynamic_offset);

    /* Bind vertex + index buffers and draw */
    VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(dev->cmd_buf, 0, 1,
                           &call->vertex_buffer->buffer, &vb_offset);
    vkCmdBindIndexBuffer(dev->cmd_buf, call->index_buffer->buffer, 0,
                          VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(dev->cmd_buf, call->index_count, 1, 0, 0, 0);
}

/* =========================================================================
 * 11. draw_instanced
 * ========================================================================= */

static void vk_draw_instanced(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                                const MopRhiDrawCall *call,
                                const MopMat4 *instance_transforms,
                                uint32_t instance_count) {
    if (!call || !instance_transforms || instance_count == 0) return;

    for (uint32_t i = 0; i < instance_count; i++) {
        MopRhiDrawCall inst_call = *call;
        inst_call.model = instance_transforms[i];

        MopMat4 view_model = mop_mat4_multiply(call->view,
                                                instance_transforms[i]);
        inst_call.mvp = mop_mat4_multiply(call->projection, view_model);

        vk_draw(dev, fb, &inst_call);
    }
}

/* =========================================================================
 * 12. frame_end
 * ========================================================================= */

static void vk_frame_end(MopRhiDevice *dev, MopRhiFramebuffer *fb) {
    vkCmdEndRenderPass(dev->cmd_buf);

    /* Copy images to readback staging buffers.
     * Render pass transitions images to TRANSFER_SRC_OPTIMAL. */

    /* Color readback */
    {
        VkBufferImageCopy region = {
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .layerCount = 1,
            },
            .imageExtent = { (uint32_t)fb->width, (uint32_t)fb->height, 1 },
        };
        vkCmdCopyImageToBuffer(dev->cmd_buf, fb->color_image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            fb->readback_color_buf, 1, &region);
    }

    /* Picking readback */
    {
        VkBufferImageCopy region = {
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .layerCount = 1,
            },
            .imageExtent = { (uint32_t)fb->width, (uint32_t)fb->height, 1 },
        };
        vkCmdCopyImageToBuffer(dev->cmd_buf, fb->pick_image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            fb->readback_pick_buf, 1, &region);
    }

    /* Depth readback */
    {
        VkBufferImageCopy region = {
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .layerCount = 1,
            },
            .imageExtent = { (uint32_t)fb->width, (uint32_t)fb->height, 1 },
        };
        vkCmdCopyImageToBuffer(dev->cmd_buf, fb->depth_image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            fb->readback_depth_buf, 1, &region);
    }

    /* Submit and wait */
    vkEndCommandBuffer(dev->cmd_buf);

    VkSubmitInfo submit = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &dev->cmd_buf,
    };
    vkQueueSubmit(dev->queue, 1, &submit, dev->fence);
    vkWaitForFences(dev->device, 1, &dev->fence, VK_TRUE, UINT64_MAX);

    /* Copy from mapped staging to CPU arrays.
     * Vulkan is top-left origin — no Y-flip needed. */
    size_t npixels = (size_t)fb->width * (size_t)fb->height;
    if (fb->readback_color && fb->readback_color_mapped)
        memcpy(fb->readback_color, fb->readback_color_mapped, npixels * 4);
    if (fb->readback_pick && fb->readback_pick_mapped)
        memcpy(fb->readback_pick, fb->readback_pick_mapped,
               npixels * sizeof(uint32_t));
    if (fb->readback_depth && fb->readback_depth_mapped)
        memcpy(fb->readback_depth, fb->readback_depth_mapped,
               npixels * sizeof(float));
}

/* =========================================================================
 * 13. framebuffer_read_color
 * ========================================================================= */

static const uint8_t *vk_framebuffer_read_color(MopRhiDevice *dev,
                                                  MopRhiFramebuffer *fb,
                                                  int *out_width,
                                                  int *out_height) {
    (void)dev;
    if (out_width)  *out_width  = fb->width;
    if (out_height) *out_height = fb->height;
    return fb->readback_color;
}

/* =========================================================================
 * 14. pick_read_id
 * ========================================================================= */

static uint32_t vk_pick_read_id(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                                  int x, int y) {
    (void)dev;
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return 0;
    /* Vulkan is top-left origin — no flip needed */
    return fb->readback_pick[(size_t)y * (size_t)fb->width + (size_t)x];
}

/* =========================================================================
 * 15. pick_read_depth
 * ========================================================================= */

static float vk_pick_read_depth(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                                  int x, int y) {
    (void)dev;
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return 1.0f;
    return fb->readback_depth[(size_t)y * (size_t)fb->width + (size_t)x];
}

/* =========================================================================
 * 16. texture_create
 * ========================================================================= */

static MopRhiTexture *vk_texture_create(MopRhiDevice *dev, int width,
                                          int height,
                                          const uint8_t *rgba_data) {
    MopRhiTexture *tex = calloc(1, sizeof(MopRhiTexture));
    if (!tex) return NULL;

    tex->width  = width;
    tex->height = height;

    VkResult r = mop_vk_create_image(dev->device, &dev->mem_props,
        (uint32_t)width, (uint32_t)height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        &tex->image, &tex->memory);
    if (r != VK_SUCCESS) {
        MOP_ERROR("[VK] texture image create failed: %d", r);
        free(tex);
        return NULL;
    }

    r = mop_vk_create_image_view(dev->device, tex->image,
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &tex->view);
    if (r != VK_SUCCESS) {
        MOP_ERROR("[VK] texture view create failed: %d", r);
        vkDestroyImage(dev->device, tex->image, NULL);
        vkFreeMemory(dev->device, tex->memory, NULL);
        free(tex);
        return NULL;
    }

    staging_upload_image(dev, tex->image, (uint32_t)width, (uint32_t)height,
                          rgba_data);

    return tex;
}

/* =========================================================================
 * 17. texture_destroy
 * ========================================================================= */

static void vk_texture_destroy(MopRhiDevice *dev, MopRhiTexture *tex) {
    if (!tex) return;
    if (dev && dev->device) {
        vkDestroyImageView(dev->device, tex->view, NULL);
        vkDestroyImage(dev->device, tex->image, NULL);
        vkFreeMemory(dev->device, tex->memory, NULL);
    }
    free(tex);
}

/* =========================================================================
 * 18. buffer_read (overlay safety)
 * ========================================================================= */

static const void *vk_buffer_read(MopRhiBuffer *buf) {
    return buf ? buf->shadow : NULL;
}

/* =========================================================================
 * Backend function table
 * ========================================================================= */

static const MopRhiBackend VK_BACKEND = {
    .name                 = "vulkan",
    .device_create        = vk_device_create,
    .device_destroy       = vk_device_destroy,
    .buffer_create        = vk_buffer_create,
    .buffer_destroy       = vk_buffer_destroy,
    .framebuffer_create   = vk_framebuffer_create,
    .framebuffer_destroy  = vk_framebuffer_destroy,
    .framebuffer_resize   = vk_framebuffer_resize,
    .frame_begin          = vk_frame_begin,
    .frame_end            = vk_frame_end,
    .draw                 = vk_draw,
    .pick_read_id         = vk_pick_read_id,
    .pick_read_depth      = vk_pick_read_depth,
    .framebuffer_read_color = vk_framebuffer_read_color,
    .texture_create         = vk_texture_create,
    .texture_destroy        = vk_texture_destroy,
    .draw_instanced         = vk_draw_instanced,
    .buffer_update          = vk_buffer_update,
    .buffer_read            = vk_buffer_read,
};

const MopRhiBackend *mop_rhi_backend_vulkan(void) {
    return &VK_BACKEND;
}

#endif /* MOP_HAS_VULKAN */
