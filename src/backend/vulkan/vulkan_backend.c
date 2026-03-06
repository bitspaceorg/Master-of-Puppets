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

#include "core/viewport_internal.h"

#include <math.h>
#include <stdio.h>

/* =========================================================================
 * Validation callback globals (set by conformance runner)
 * ========================================================================= */

MopVkValidationCallback mop_vk_on_validation_error = NULL;
MopVkValidationCallback mop_vk_on_sync_hazard = NULL;

/* =========================================================================
 * VK_EXT_debug_utils callback
 * ========================================================================= */

static VKAPI_ATTR VkBool32 VKAPI_CALL mop_vk_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user_data) {
  (void)user_data;

  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    MOP_ERROR("[VK validation] %s", data->pMessage);
    if (mop_vk_on_validation_error)
      mop_vk_on_validation_error();
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    MOP_WARN("[VK validation] %s", data->pMessage);
    /* Sync hazards come through as warnings with VALIDATION type */
    if ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) &&
        data->pMessage && strstr(data->pMessage, "SYNC-HAZARD")) {
      if (mop_vk_on_sync_hazard)
        mop_vk_on_sync_hazard();
    } else {
      if (mop_vk_on_validation_error)
        mop_vk_on_validation_error();
    }
  }

  return VK_FALSE;
}

/* =========================================================================
 * Helper: create VkShaderModule from SPIR-V
 * ========================================================================= */

static VkShaderModule create_shader_module(VkDevice device,
                                           const uint32_t *code, size_t size) {
  VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size,
      .pCode = code,
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

static void staging_upload(MopRhiDevice *dev, VkBuffer dst, const void *data,
                           size_t size) {
  if (size > MOP_VK_STAGING_SIZE) {
    MOP_ERROR("[VK] staging upload too large: %zu > %d", size,
              MOP_VK_STAGING_SIZE);
    return;
  }

  memcpy(dev->staging_mapped, data, size);

  VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);

  VkBufferCopy region = {.size = size};
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
  mop_vk_transition_image(
      cb, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkBufferImageCopy region = {
      .imageSubresource =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .layerCount = 1,
          },
      .imageExtent = {width, height, 1},
  };
  vkCmdCopyBufferToImage(cb, dev->staging_buf, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  /* Transition to SHADER_READ_ONLY */
  mop_vk_transition_image(
      cb, image, VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  mop_vk_end_oneshot(dev->device, dev->queue, dev->cmd_pool, cb);
}

/* =========================================================================
 * 1. device_create
 * ========================================================================= */

static MopRhiDevice *vk_device_create(void) {
  MopRhiDevice *dev = calloc(1, sizeof(MopRhiDevice));
  if (!dev)
    return NULL;

  /* ---- Instance ---- */
  VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "Master of Puppets",
      .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
      .pEngineName = "MOP",
      .engineVersion = VK_MAKE_VERSION(0, 1, 0),
      .apiVersion = VK_API_VERSION_1_2,
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
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
      .enabledLayerCount = layer_count,
      .ppEnabledLayerNames = layer_names,
#if defined(__APPLE__)
      .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
#endif
  };

  /* Instance extensions */
  const char *inst_exts[4];
  uint32_t inst_ext_count = 0;
#if defined(__APPLE__)
  inst_exts[inst_ext_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
#endif
  if (layer_count > 0) {
    inst_exts[inst_ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  }
  inst_ci.enabledExtensionCount = inst_ext_count;
  inst_ci.ppEnabledExtensionNames = inst_ext_count > 0 ? inst_exts : NULL;

  VkResult r = vkCreateInstance(&inst_ci, NULL, &dev->instance);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkCreateInstance failed: %d", r);
    free(dev);
    return NULL;
  }

  /* ---- Debug messenger (when validation layers are enabled) ---- */
  if (layer_count > 0) {
    PFN_vkCreateDebugUtilsMessengerEXT create_fn =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            dev->instance, "vkCreateDebugUtilsMessengerEXT");
    if (create_fn) {
      VkDebugUtilsMessengerCreateInfoEXT dbg_ci = {
          .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
          .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
          .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
          .pfnUserCallback = mop_vk_debug_callback,
      };
      r = create_fn(dev->instance, &dbg_ci, NULL, &dev->debug_messenger);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] debug messenger setup failed: %d", r);
        dev->debug_messenger = VK_NULL_HANDLE;
      } else {
        MOP_INFO("[VK] debug messenger enabled (validation errors → counters)");
      }
    }
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
  dev->min_ubo_alignment =
      dev->dev_props.limits.minUniformBufferOffsetAlignment;
  if (dev->min_ubo_alignment == 0)
    dev->min_ubo_alignment = 256;

  MOP_INFO("[VK] GPU: %s", dev->dev_props.deviceName);

  /* Check for fillModeNonSolid (wireframe support) */
  VkPhysicalDeviceFeatures features;
  vkGetPhysicalDeviceFeatures(dev->physical_device, &features);
  dev->has_fill_mode_non_solid = features.fillModeNonSolid;
  dev->reverse_z = true;

  /* Query MSAA support (intersection of color + depth sample counts) */
  {
    VkSampleCountFlags supported =
        dev->dev_props.limits.framebufferColorSampleCounts &
        dev->dev_props.limits.framebufferDepthSampleCounts;
    dev->msaa_samples = (supported & VK_SAMPLE_COUNT_4_BIT)
                            ? VK_SAMPLE_COUNT_4_BIT
                            : VK_SAMPLE_COUNT_1_BIT;
    MOP_INFO("[VK] MSAA: %dx", (int)dev->msaa_samples);
  }

  /* ---- Queue family ---- */
  uint32_t qf_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(dev->physical_device, &qf_count,
                                           NULL);
  VkQueueFamilyProperties *qf_props = malloc(qf_count * sizeof(*qf_props));
  vkGetPhysicalDeviceQueueFamilyProperties(dev->physical_device, &qf_count,
                                           qf_props);

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
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = dev->queue_family,
      .queueCount = 1,
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
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_ci,
      .pEnabledFeatures = &enabled_features,
      .enabledExtensionCount = dev_ext_count,
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
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = dev->queue_family,
  };
  r = vkCreateCommandPool(dev->device, &pool_ci, NULL, &dev->cmd_pool);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkCreateCommandPool failed: %d", r);
    goto fail;
  }

  VkCommandBufferAllocateInfo cb_ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = dev->cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  r = vkAllocateCommandBuffers(dev->device, &cb_ai, &dev->cmd_buf);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkAllocateCommandBuffers failed: %d", r);
    goto fail;
  }

  VkFenceCreateInfo fence_ci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
  };
  r = vkCreateFence(dev->device, &fence_ci, NULL, &dev->fence);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkCreateFence failed: %d", r);
    goto fail;
  }

  /* ---- Shader modules ---- */
  dev->solid_vert = create_shader_module(dev->device, mop_solid_vert_spv,
                                         mop_solid_vert_spv_size);
  dev->instanced_vert = create_shader_module(
      dev->device, mop_instanced_vert_spv, mop_instanced_vert_spv_size);
  dev->solid_frag = create_shader_module(dev->device, mop_solid_frag_spv,
                                         mop_solid_frag_spv_size);
  dev->wireframe_frag = create_shader_module(
      dev->device, mop_wireframe_frag_spv, mop_wireframe_frag_spv_size);

  if (!dev->solid_vert || !dev->instanced_vert || !dev->solid_frag ||
      !dev->wireframe_frag) {
    MOP_ERROR("[VK] shader module creation failed");
    goto fail;
  }

  /* ---- Render pass, layouts ---- */
  r = mop_vk_create_render_pass(dev->device, dev->msaa_samples,
                                &dev->render_pass);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] render pass: %d", r);
    goto fail;
  }

  r = mop_vk_create_desc_set_layout(dev->device, &dev->desc_set_layout);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] desc layout: %d", r);
    goto fail;
  }

  r = mop_vk_create_pipeline_layout(dev->device, dev->desc_set_layout,
                                    &dev->pipeline_layout);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] pipeline layout: %d", r);
    goto fail;
  }

  /* ---- Descriptor pool ---- */
  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
       MOP_VK_MAX_DRAWS_PER_FRAME + 2}, /* +2 for skybox + tonemap */
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       MOP_VK_MAX_DRAWS_PER_FRAME * 5 + 4}, /* texture + shadow + IBL×3 + skybox
                                               + tonemap + grid + overlay */
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, /* SDF overlay SSBO */
  };
  VkDescriptorPoolCreateInfo dp_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets =
          MOP_VK_MAX_DRAWS_PER_FRAME + 4, /* +4: skybox+tonemap+grid+overlay */
      .poolSizeCount = 3,
      .pPoolSizes = pool_sizes,
  };
  r = vkCreateDescriptorPool(dev->device, &dp_ci, NULL, &dev->desc_pool);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] desc pool: %d", r);
    goto fail;
  }

  /* ---- Default sampler (linear, repeat) ---- */
  VkSamplerCreateInfo samp_ci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .maxLod = 1.0f,
  };
  r = vkCreateSampler(dev->device, &samp_ci, NULL, &dev->default_sampler);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkCreateSampler failed: %d", r);
    goto fail;
  }

  /* ---- Staging buffer (4 MB host-visible) ---- */
  r = mop_vk_create_buffer(dev->device, &dev->mem_props, MOP_VK_STAGING_SIZE,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &dev->staging_buf, &dev->staging_mem);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] staging buffer: %d", r);
    goto fail;
  }
  r = vkMapMemory(dev->device, dev->staging_mem, 0, MOP_VK_STAGING_SIZE, 0,
                  &dev->staging_mapped);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkMapMemory staging failed: %d", r);
    goto fail;
  }

  /* ---- 1x1 white fallback texture ---- */
  r = mop_vk_create_image(dev->device, &dev->mem_props, 1, 1,
                          VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          &dev->white_image, &dev->white_memory);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] white image: %d", r);
    goto fail;
  }

  r = mop_vk_create_image_view(dev->device, dev->white_image,
                               VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_ASPECT_COLOR_BIT, &dev->white_view);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] white view: %d", r);
    goto fail;
  }

  /* Upload white pixel */
  {
    uint8_t white[4] = {255, 255, 255, 255};
    staging_upload_image(dev, dev->white_image, 1, 1, white);
  }

  /* ---- 1x1 black fallback texture (for IBL when no env map loaded) ---- */
  r = mop_vk_create_image(dev->device, &dev->mem_props, 1, 1,
                          VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          &dev->black_image, &dev->black_memory);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] black image: %d", r);
    goto fail;
  }

  r = mop_vk_create_image_view(dev->device, dev->black_image,
                               VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_ASPECT_COLOR_BIT, &dev->black_view);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] black view: %d", r);
    goto fail;
  }

  /* Upload black pixel */
  {
    uint8_t black[4] = {0, 0, 0, 255};
    staging_upload_image(dev, dev->black_image, 1, 1, black);
  }

  /* ---- GPU timestamp query pool ---- */
  dev->timestamp_period_ns = dev->dev_props.limits.timestampPeriod;
  dev->has_timestamp_queries = (dev->timestamp_period_ns > 0.0f);
  if (dev->has_timestamp_queries) {
    VkQueryPoolCreateInfo qp_ci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = 2, /* begin + end */
    };
    r = vkCreateQueryPool(dev->device, &qp_ci, NULL, &dev->timestamp_pool);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] timestamp query pool failed: %d (timing disabled)", r);
      dev->has_timestamp_queries = false;
    }
  }

  /* ---- Shadow mapping resources ---- */
  {
    /* Shadow render pass */
    r = mop_vk_create_shadow_render_pass(dev->device, &dev->shadow_render_pass);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] shadow render pass failed: %d (shadows disabled)", r);
    } else {
      /* Shadow image: D32_SFLOAT, 2048x2048, 4 array layers */
      VkImageCreateInfo shadow_ci = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
          .imageType = VK_IMAGE_TYPE_2D,
          .format = VK_FORMAT_D32_SFLOAT,
          .extent = {MOP_VK_SHADOW_MAP_SIZE, MOP_VK_SHADOW_MAP_SIZE, 1},
          .mipLevels = 1,
          .arrayLayers = MOP_VK_CASCADE_COUNT,
          .samples = VK_SAMPLE_COUNT_1_BIT,
          .tiling = VK_IMAGE_TILING_OPTIMAL,
          .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT,
          .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      r = vkCreateImage(dev->device, &shadow_ci, NULL, &dev->shadow_image);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] shadow image failed: %d (shadows disabled)", r);
        goto shadow_done;
      }

      VkMemoryRequirements shadow_req;
      vkGetImageMemoryRequirements(dev->device, dev->shadow_image, &shadow_req);
      int shadow_mem_idx =
          mop_vk_find_memory_type(&dev->mem_props, shadow_req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (shadow_mem_idx < 0) {
        MOP_WARN("[VK] shadow memory type not found (shadows disabled)");
        vkDestroyImage(dev->device, dev->shadow_image, NULL);
        dev->shadow_image = VK_NULL_HANDLE;
        goto shadow_done;
      }

      VkMemoryAllocateInfo shadow_ai = {
          .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
          .allocationSize = shadow_req.size,
          .memoryTypeIndex = (uint32_t)shadow_mem_idx,
      };
      r = vkAllocateMemory(dev->device, &shadow_ai, NULL, &dev->shadow_memory);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] shadow memory alloc failed: %d", r);
        vkDestroyImage(dev->device, dev->shadow_image, NULL);
        dev->shadow_image = VK_NULL_HANDLE;
        goto shadow_done;
      }
      vkBindImageMemory(dev->device, dev->shadow_image, dev->shadow_memory, 0);

      /* Per-layer image views (for rendering into individual cascades) */
      for (int i = 0; i < MOP_VK_CASCADE_COUNT; i++) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = dev->shadow_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = (uint32_t)i,
                    .layerCount = 1,
                },
        };
        r = vkCreateImageView(dev->device, &vci, NULL, &dev->shadow_views[i]);
        if (r != VK_SUCCESS) {
          MOP_WARN("[VK] shadow view[%d] failed: %d", i, r);
          goto shadow_done;
        }
      }

      /* Array image view (for sampling all cascades) */
      VkImageViewCreateInfo arr_vci = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image = dev->shadow_image,
          .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
          .format = VK_FORMAT_D32_SFLOAT,
          .subresourceRange =
              {
                  .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                  .baseMipLevel = 0,
                  .levelCount = 1,
                  .baseArrayLayer = 0,
                  .layerCount = MOP_VK_CASCADE_COUNT,
              },
      };
      r = vkCreateImageView(dev->device, &arr_vci, NULL,
                            &dev->shadow_array_view);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] shadow array view failed: %d", r);
        goto shadow_done;
      }

      /* Shadow framebuffers (one per cascade) */
      for (int i = 0; i < MOP_VK_CASCADE_COUNT; i++) {
        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = dev->shadow_render_pass,
            .attachmentCount = 1,
            .pAttachments = &dev->shadow_views[i],
            .width = MOP_VK_SHADOW_MAP_SIZE,
            .height = MOP_VK_SHADOW_MAP_SIZE,
            .layers = 1,
        };
        r = vkCreateFramebuffer(dev->device, &fci, NULL, &dev->shadow_fbs[i]);
        if (r != VK_SUCCESS) {
          MOP_WARN("[VK] shadow fb[%d] failed: %d", i, r);
          goto shadow_done;
        }
      }

      /* Comparison sampler for PCF (linear filtering for hardware PCF) */
      VkSamplerCreateInfo shadow_samp_ci = {
          .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
          .magFilter = VK_FILTER_LINEAR,
          .minFilter = VK_FILTER_LINEAR,
          .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
          .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
          .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
          .compareEnable = VK_TRUE,
          .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
          .maxLod = 1.0f,
          .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
      };
      r = vkCreateSampler(dev->device, &shadow_samp_ci, NULL,
                          &dev->shadow_sampler);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] shadow sampler failed: %d", r);
        goto shadow_done;
      }

      /* Shadow pipeline layout: push constant = mat4 (64 bytes) */
      VkPushConstantRange shadow_push = {
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
          .offset = 0,
          .size = 64, /* mat4 light_vp */
      };
      VkPipelineLayoutCreateInfo shadow_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .pushConstantRangeCount = 1,
          .pPushConstantRanges = &shadow_push,
      };
      r = vkCreatePipelineLayout(dev->device, &shadow_layout_ci, NULL,
                                 &dev->shadow_pipeline_layout);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] shadow pipeline layout failed: %d", r);
        goto shadow_done;
      }

      /* Load shadow shaders and create pipeline */
#if defined(MOP_VK_HAS_SHADOW_SHADERS)
      dev->shadow_vert = create_shader_module(dev->device, mop_shadow_vert_spv,
                                              mop_shadow_vert_spv_size);
      dev->shadow_frag = create_shader_module(dev->device, mop_shadow_frag_spv,
                                              mop_shadow_frag_spv_size);
      if (dev->shadow_vert && dev->shadow_frag) {
        dev->shadow_pipeline = mop_vk_create_shadow_pipeline(dev);
        if (dev->shadow_pipeline) {
          dev->shadows_enabled = true;
          MOP_INFO("[VK] cascade shadow mapping enabled (%dx%d, %d cascades)",
                   MOP_VK_SHADOW_MAP_SIZE, MOP_VK_SHADOW_MAP_SIZE,
                   MOP_VK_CASCADE_COUNT);
        }
      }
#else
      /* Shadow shaders not compiled yet — resources allocated but pipeline
       * deferred until SPIR-V is available */
      MOP_INFO("[VK] shadow map resources allocated (pipeline pending shader "
               "compilation)");
#endif
    }
  }
shadow_done:

  /* ---- Post-processing resources ---- */
  {
    r = mop_vk_create_postprocess_render_pass(dev->device,
                                              &dev->postprocess_render_pass);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] postprocess render pass failed: %d", r);
    } else {
      /* Descriptor set layout: single combined image sampler */
      VkDescriptorSetLayoutBinding pp_binding = {
          .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      };
      VkDescriptorSetLayoutCreateInfo pp_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 1,
          .pBindings = &pp_binding,
      };
      r = vkCreateDescriptorSetLayout(dev->device, &pp_layout_ci, NULL,
                                      &dev->postprocess_desc_layout);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] postprocess desc layout failed: %d", r);
        goto postprocess_done;
      }

      /* Pipeline layout: desc set + push constant (vec2 inv_resolution) */
      VkPushConstantRange pp_push = {
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          .offset = 0,
          .size = 8, /* vec2 */
      };
      VkPipelineLayoutCreateInfo pp_pl_ci = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = 1,
          .pSetLayouts = &dev->postprocess_desc_layout,
          .pushConstantRangeCount = 1,
          .pPushConstantRanges = &pp_push,
      };
      r = vkCreatePipelineLayout(dev->device, &pp_pl_ci, NULL,
                                 &dev->postprocess_pipeline_layout);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] postprocess pipeline layout failed: %d", r);
        goto postprocess_done;
      }

      /* Load post-process shaders and create pipeline */
#if defined(MOP_VK_HAS_POSTPROCESS_SHADERS)
      dev->fullscreen_vert = create_shader_module(
          dev->device, mop_fullscreen_vert_spv, mop_fullscreen_vert_spv_size);
      dev->fxaa_frag = create_shader_module(dev->device, mop_fxaa_frag_spv,
                                            mop_fxaa_frag_spv_size);
      if (dev->fullscreen_vert && dev->fxaa_frag) {
        dev->postprocess_pipeline = mop_vk_create_postprocess_pipeline(dev);
        if (dev->postprocess_pipeline) {
          dev->postprocess_enabled = true;
          MOP_INFO("[VK] FXAA post-processing enabled");
        }
      }
#else
      MOP_INFO("[VK] postprocess resources allocated (pipeline pending shader "
               "compilation)");
#endif
    }
  }
postprocess_done:

  /* ---- HDR Tonemap resources ---- */
  {
    VkResult tr = mop_vk_create_tonemap_render_pass(dev->device,
                                                    &dev->tonemap_render_pass);
    if (tr != VK_SUCCESS) {
      MOP_WARN("[VK] tonemap render pass failed: %d", tr);
    } else {
      /* Descriptor set layout: single combined image sampler */
      VkDescriptorSetLayoutBinding tm_binding = {
          .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      };
      VkDescriptorSetLayoutCreateInfo tm_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 1,
          .pBindings = &tm_binding,
      };
      tr = vkCreateDescriptorSetLayout(dev->device, &tm_layout_ci, NULL,
                                       &dev->tonemap_desc_layout);
      if (tr != VK_SUCCESS) {
        MOP_WARN("[VK] tonemap desc layout failed: %d", tr);
        goto tonemap_done;
      }

      /* Pipeline layout: desc set + push constant (float exposure) */
      VkPushConstantRange tm_push = {
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          .offset = 0,
          .size = 4, /* float */
      };
      VkPipelineLayoutCreateInfo tm_pl_ci = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = 1,
          .pSetLayouts = &dev->tonemap_desc_layout,
          .pushConstantRangeCount = 1,
          .pPushConstantRanges = &tm_push,
      };
      tr = vkCreatePipelineLayout(dev->device, &tm_pl_ci, NULL,
                                  &dev->tonemap_pipeline_layout);
      if (tr != VK_SUCCESS) {
        MOP_WARN("[VK] tonemap pipeline layout failed: %d", tr);
        goto tonemap_done;
      }

#if defined(MOP_VK_HAS_TONEMAP_SHADERS)
      dev->tonemap_frag = create_shader_module(
          dev->device, mop_tonemap_frag_spv, mop_tonemap_frag_spv_size);
      if (dev->fullscreen_vert && dev->tonemap_frag) {
        dev->tonemap_pipeline = mop_vk_create_tonemap_pipeline(dev);
        if (dev->tonemap_pipeline) {
          dev->tonemap_enabled = true;
          dev->hdr_exposure = 1.0f;
          MOP_INFO("[VK] HDR tonemap pipeline enabled");
        }
      }
#else
      MOP_INFO("[VK] tonemap resources allocated (pipeline pending shader "
               "compilation)");
#endif
    }
  }
tonemap_done:

  /* ---- Skybox pipeline (equirectangular env map) ---- */
#if defined(MOP_VK_HAS_SKYBOX_SHADERS)
  if (dev->fullscreen_vert) {
    dev->skybox_frag = create_shader_module(dev->device, mop_skybox_frag_spv,
                                            mop_skybox_frag_spv_size);
    if (dev->skybox_frag) {
      dev->skybox_pipeline = mop_vk_create_skybox_pipeline(dev);
      if (dev->skybox_pipeline) {
        MOP_INFO("[VK] skybox pipeline created");
      }
    }
  }
#endif

  /* ---- SDF overlay + grid pipeline ---- */
  {
    VkResult ov_r = mop_vk_create_overlay_render_pass(
        dev->device, &dev->overlay_render_pass);
    if (ov_r != VK_SUCCESS) {
      MOP_WARN("[VK] overlay render pass failed: %d", ov_r);
    } else {
      /* Overlay descriptor set layout: binding 0 = depth sampler,
       * binding 1 = SSBO (prim data) */
      VkDescriptorSetLayoutBinding ov_bindings[2] = {
          {
              .binding = 0,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          },
          {
              .binding = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          },
      };
      VkDescriptorSetLayoutCreateInfo ov_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 2,
          .pBindings = ov_bindings,
      };
      ov_r = vkCreateDescriptorSetLayout(dev->device, &ov_layout_ci, NULL,
                                         &dev->overlay_desc_layout);
      if (ov_r == VK_SUCCESS) {
        /* Push constants: prim_count(uint) + fb_width(float) + fb_height(float)
         * + reverse_z(uint) = 16 bytes */
        VkPushConstantRange ov_push = {
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = 16,
        };
        VkPipelineLayoutCreateInfo ov_pl_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &dev->overlay_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &ov_push,
        };
        ov_r = vkCreatePipelineLayout(dev->device, &ov_pl_ci, NULL,
                                      &dev->overlay_pipeline_layout);
      }

      /* Grid descriptor set layout: binding 0 = depth sampler */
      VkDescriptorSetLayoutBinding grid_binding = {
          .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      };
      VkDescriptorSetLayoutCreateInfo grid_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 1,
          .pBindings = &grid_binding,
      };
      ov_r = vkCreateDescriptorSetLayout(dev->device, &grid_layout_ci, NULL,
                                         &dev->grid_desc_layout);
      if (ov_r == VK_SUCCESS) {
        /* Grid push constants: Hi[9] + vp rows (6) + grid_half + reverse_z +
         * axis_half_width + pad + 4 vec4 colors = 144 bytes.
         * MoltenVK/Apple supports 4096, most GPUs support ≥256. */
        VkPushConstantRange grid_push = {
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = 144,
        };
        VkPipelineLayoutCreateInfo grid_pl_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &dev->grid_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &grid_push,
        };
        ov_r = vkCreatePipelineLayout(dev->device, &grid_pl_ci, NULL,
                                      &dev->grid_pipeline_layout);
      }

#if defined(MOP_VK_HAS_OVERLAY_SHADERS)
      if (dev->fullscreen_vert) {
        dev->overlay_frag = create_shader_module(
            dev->device, mop_overlay_frag_spv, mop_overlay_frag_spv_size);
        dev->grid_frag = create_shader_module(dev->device, mop_grid_frag_spv,
                                              mop_grid_frag_spv_size);

        if (dev->overlay_frag && dev->overlay_pipeline_layout) {
          dev->overlay_pipeline = mop_vk_create_overlay_pipeline(dev);
          if (dev->overlay_pipeline) {
            dev->overlay_enabled = true;
            MOP_INFO("[VK] SDF overlay pipeline enabled");
          }
        }
        if (dev->grid_frag && dev->grid_pipeline_layout) {
          dev->grid_pipeline = mop_vk_create_grid_pipeline(dev);
          if (dev->grid_pipeline) {
            dev->grid_enabled = true;
            MOP_INFO("[VK] analytical grid pipeline enabled");
          }
        }
      }
#endif
    }
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

      if (dev->black_view)
        vkDestroyImageView(d, dev->black_view, NULL);
      if (dev->black_image)
        vkDestroyImage(d, dev->black_image, NULL);
      if (dev->black_memory)
        vkFreeMemory(d, dev->black_memory, NULL);

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

      if (dev->timestamp_pool)
        vkDestroyQueryPool(d, dev->timestamp_pool, NULL);

      if (dev->solid_vert)
        vkDestroyShaderModule(d, dev->solid_vert, NULL);
      if (dev->instanced_vert)
        vkDestroyShaderModule(d, dev->instanced_vert, NULL);
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
  if (!dev)
    return;

  VkDevice d = dev->device;
  if (d) {
    vkDeviceWaitIdle(d);

    /* Shadow mapping cleanup */
    if (dev->shadow_pipeline)
      vkDestroyPipeline(d, dev->shadow_pipeline, NULL);
    if (dev->shadow_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->shadow_pipeline_layout, NULL);
    if (dev->shadow_render_pass)
      vkDestroyRenderPass(d, dev->shadow_render_pass, NULL);
    for (int i = 0; i < MOP_VK_CASCADE_COUNT; i++) {
      if (dev->shadow_fbs[i])
        vkDestroyFramebuffer(d, dev->shadow_fbs[i], NULL);
      if (dev->shadow_views[i])
        vkDestroyImageView(d, dev->shadow_views[i], NULL);
    }
    if (dev->shadow_array_view)
      vkDestroyImageView(d, dev->shadow_array_view, NULL);
    if (dev->shadow_sampler)
      vkDestroySampler(d, dev->shadow_sampler, NULL);
    if (dev->shadow_image)
      vkDestroyImage(d, dev->shadow_image, NULL);
    if (dev->shadow_memory)
      vkFreeMemory(d, dev->shadow_memory, NULL);
    if (dev->shadow_vert)
      vkDestroyShaderModule(d, dev->shadow_vert, NULL);
    if (dev->shadow_frag)
      vkDestroyShaderModule(d, dev->shadow_frag, NULL);

    /* Post-processing cleanup */
    if (dev->postprocess_pipeline)
      vkDestroyPipeline(d, dev->postprocess_pipeline, NULL);
    if (dev->postprocess_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->postprocess_pipeline_layout, NULL);
    if (dev->postprocess_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->postprocess_desc_layout, NULL);
    if (dev->postprocess_render_pass)
      vkDestroyRenderPass(d, dev->postprocess_render_pass, NULL);
    if (dev->fullscreen_vert)
      vkDestroyShaderModule(d, dev->fullscreen_vert, NULL);
    if (dev->fxaa_frag)
      vkDestroyShaderModule(d, dev->fxaa_frag, NULL);

    /* Tonemap cleanup */
    if (dev->tonemap_pipeline)
      vkDestroyPipeline(d, dev->tonemap_pipeline, NULL);
    if (dev->tonemap_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->tonemap_pipeline_layout, NULL);
    if (dev->tonemap_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->tonemap_desc_layout, NULL);
    if (dev->tonemap_render_pass)
      vkDestroyRenderPass(d, dev->tonemap_render_pass, NULL);
    if (dev->tonemap_frag)
      vkDestroyShaderModule(d, dev->tonemap_frag, NULL);

    /* Skybox cleanup */
    if (dev->skybox_pipeline)
      vkDestroyPipeline(d, dev->skybox_pipeline, NULL);
    if (dev->skybox_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->skybox_pipeline_layout, NULL);
    if (dev->skybox_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->skybox_desc_layout, NULL);
    if (dev->skybox_frag)
      vkDestroyShaderModule(d, dev->skybox_frag, NULL);

    /* Overlay cleanup */
    if (dev->overlay_pipeline)
      vkDestroyPipeline(d, dev->overlay_pipeline, NULL);
    if (dev->overlay_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->overlay_pipeline_layout, NULL);
    if (dev->overlay_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->overlay_desc_layout, NULL);
    if (dev->overlay_render_pass)
      vkDestroyRenderPass(d, dev->overlay_render_pass, NULL);
    if (dev->overlay_frag)
      vkDestroyShaderModule(d, dev->overlay_frag, NULL);

    /* Grid cleanup */
    if (dev->grid_pipeline)
      vkDestroyPipeline(d, dev->grid_pipeline, NULL);
    if (dev->grid_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->grid_pipeline_layout, NULL);
    if (dev->grid_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->grid_desc_layout, NULL);
    if (dev->grid_frag)
      vkDestroyShaderModule(d, dev->grid_frag, NULL);

    /* Pipelines */
    for (int i = 0; i < MOP_VK_MAX_PIPELINES; i++) {
      if (dev->pipelines[i])
        vkDestroyPipeline(d, dev->pipelines[i], NULL);
    }
    if (dev->instanced_pipeline)
      vkDestroyPipeline(d, dev->instanced_pipeline, NULL);

    /* Timestamp queries */
    if (dev->timestamp_pool)
      vkDestroyQueryPool(d, dev->timestamp_pool, NULL);

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

    /* Black texture */
    if (dev->black_view)
      vkDestroyImageView(d, dev->black_view, NULL);
    if (dev->black_image)
      vkDestroyImage(d, dev->black_image, NULL);
    if (dev->black_memory)
      vkFreeMemory(d, dev->black_memory, NULL);

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
    if (dev->instanced_vert)
      vkDestroyShaderModule(d, dev->instanced_vert, NULL);
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

  if (dev->debug_messenger && dev->instance) {
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_fn =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            dev->instance, "vkDestroyDebugUtilsMessengerEXT");
    if (destroy_fn)
      destroy_fn(dev->instance, dev->debug_messenger, NULL);
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
  if (!buf)
    return NULL;

  buf->size = desc->size;

  /* Shadow copy */
  buf->shadow = malloc(desc->size);
  if (!buf->shadow) {
    free(buf);
    return NULL;
  }
  memcpy(buf->shadow, desc->data, desc->size);

  /* Device-local buffer */
  VkResult r = mop_vk_create_buffer(
      dev->device, &dev->mem_props, desc->size,
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &buf->buffer, &buf->memory);
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
  if (!buf)
    return;
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
  if (!buf || !data)
    return;

  /* Update shadow */
  memcpy((uint8_t *)buf->shadow + offset, data, size);

  /* Upload via staging — for partial updates, copy to staging with offset */
  if (size > MOP_VK_STAGING_SIZE) {
    MOP_ERROR("[VK] buffer_update too large: %zu", size);
    return;
  }

  memcpy(dev->staging_mapped, data, size);

  VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);
  VkBufferCopy region = {.srcOffset = 0, .dstOffset = offset, .size = size};
  vkCmdCopyBuffer(cb, dev->staging_buf, buf->buffer, 1, &region);

  /* Memory barrier: ensure the transfer write is visible to subsequent
   * vertex attribute reads.  Required on MoltenVK where implicit
   * synchronization between submissions may not flush GPU caches. */
  VkBufferMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask =
          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = buf->buffer,
      .offset = offset,
      .size = size,
  };
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                       &barrier, 0, NULL);

  mop_vk_end_oneshot(dev->device, dev->queue, dev->cmd_pool, cb);
}

/* =========================================================================
 * Framebuffer helpers
 * ========================================================================= */

static void vk_fb_create_attachments(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                                     int width, int height) {
  fb->width = width;
  fb->height = height;
  size_t npixels = (size_t)width * (size_t)height;

  VkResult r;

  /* ---- Color (R16G16B16A16_SFLOAT — HDR accumulation) ---- */
  r = mop_vk_create_image(
      dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
      VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      &fb->color_image, &fb->color_memory);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb color image: %d", r);
    return;
  }

  r = mop_vk_create_image_view(dev->device, fb->color_image,
                               VK_FORMAT_R16G16B16A16_SFLOAT,
                               VK_IMAGE_ASPECT_COLOR_BIT, &fb->color_view);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb color view: %d", r);
    return;
  }

  /* ---- LDR color (R8G8B8A8_SRGB — tonemap resolve target) ---- */
  r = mop_vk_create_image(
      dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
      VK_FORMAT_R8G8B8A8_SRGB, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      &fb->ldr_color_image, &fb->ldr_color_memory);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb ldr color image: %d", r);
    return;
  }

  r = mop_vk_create_image_view(dev->device, fb->ldr_color_image,
                               VK_FORMAT_R8G8B8A8_SRGB,
                               VK_IMAGE_ASPECT_COLOR_BIT, &fb->ldr_color_view);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb ldr color view: %d", r);
    return;
  }

  /* ---- Picking (R32_UINT) — TRANSFER_DST for MSAA manual resolve ---- */
  r = mop_vk_create_image(
      dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
      VK_FORMAT_R32_UINT, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      &fb->pick_image, &fb->pick_memory);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb pick image: %d", r);
    return;
  }

  r = mop_vk_create_image_view(dev->device, fb->pick_image, VK_FORMAT_R32_UINT,
                               VK_IMAGE_ASPECT_COLOR_BIT, &fb->pick_view);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb pick view: %d", r);
    return;
  }

  /* ---- Depth (D32_SFLOAT) — TRANSFER_DST for MSAA manual resolve ---- */
  r = mop_vk_create_image(
      dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
      VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
          VK_IMAGE_USAGE_SAMPLED_BIT,
      &fb->depth_image, &fb->depth_memory);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb depth image: %d", r);
    return;
  }

  r = mop_vk_create_image_view(dev->device, fb->depth_image,
                               VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT,
                               &fb->depth_view);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb depth view: %d", r);
    return;
  }

  /* ---- MSAA render targets (when msaa_samples > 1) ---- */
  if (dev->msaa_samples > VK_SAMPLE_COUNT_1_BIT) {
    /* MSAA color (HDR) — TRANSFER_SRC for manual resolve */
    r = mop_vk_create_image(
        dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
        VK_FORMAT_R16G16B16A16_SFLOAT, dev->msaa_samples,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        &fb->msaa_color_image, &fb->msaa_color_memory);
    if (r != VK_SUCCESS) {
      MOP_ERROR("[VK] fb msaa color image: %d", r);
      return;
    }
    r = mop_vk_create_image_view(
        dev->device, fb->msaa_color_image, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_ASPECT_COLOR_BIT, &fb->msaa_color_view);
    if (r != VK_SUCCESS) {
      MOP_ERROR("[VK] fb msaa color view: %d", r);
      return;
    }

    /* MSAA picking — needs TRANSFER_SRC for manual vkCmdResolveImage */
    r = mop_vk_create_image(
        dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
        VK_FORMAT_R32_UINT, dev->msaa_samples,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        &fb->msaa_pick_image, &fb->msaa_pick_memory);
    if (r != VK_SUCCESS) {
      MOP_ERROR("[VK] fb msaa pick image: %d", r);
      return;
    }
    r = mop_vk_create_image_view(dev->device, fb->msaa_pick_image,
                                 VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT,
                                 &fb->msaa_pick_view);
    if (r != VK_SUCCESS) {
      MOP_ERROR("[VK] fb msaa pick view: %d", r);
      return;
    }

    /* MSAA depth — TRANSFER_SRC for manual resolve */
    r = mop_vk_create_image(dev->device, &dev->mem_props, (uint32_t)width,
                            (uint32_t)height, VK_FORMAT_D32_SFLOAT,
                            dev->msaa_samples,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            &fb->msaa_depth_image, &fb->msaa_depth_memory);
    if (r != VK_SUCCESS) {
      MOP_ERROR("[VK] fb msaa depth image: %d", r);
      return;
    }
    r = mop_vk_create_image_view(
        dev->device, fb->msaa_depth_image, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_ASPECT_DEPTH_BIT, &fb->msaa_depth_view);
    if (r != VK_SUCCESS) {
      MOP_ERROR("[VK] fb msaa depth view: %d", r);
      return;
    }
  }

  /* ---- VkFramebuffer ---- */
  uint32_t att_count;
  VkImageView views[6];
  if (dev->msaa_samples > VK_SAMPLE_COUNT_1_BIT) {
    /* MSAA: 6 attachments — 3 MSAA + 3 resolve (all resolved in-pass) */
    views[0] = fb->msaa_color_view;
    views[1] = fb->msaa_pick_view;
    views[2] = fb->msaa_depth_view;
    views[3] = fb->color_view;
    views[4] = fb->pick_view;
    views[5] = fb->depth_view;
    att_count = 6;
  } else {
    views[0] = fb->color_view;
    views[1] = fb->pick_view;
    views[2] = fb->depth_view;
    att_count = 3;
  }
  VkFramebufferCreateInfo fb_ci = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = dev->render_pass,
      .attachmentCount = att_count,
      .pAttachments = views,
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .layers = 1,
  };
  r = vkCreateFramebuffer(dev->device, &fb_ci, NULL, &fb->framebuffer);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkCreateFramebuffer failed: %d", r);
    return;
  }

  /* ---- Tonemap framebuffer (LDR output) ---- */
  if (dev->tonemap_render_pass && fb->ldr_color_view) {
    VkImageView tm_views[1] = {fb->ldr_color_view};
    VkFramebufferCreateInfo tm_fb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = dev->tonemap_render_pass,
        .attachmentCount = 1,
        .pAttachments = tm_views,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .layers = 1,
    };
    r = vkCreateFramebuffer(dev->device, &tm_fb_ci, NULL,
                            &fb->tonemap_framebuffer);
    if (r != VK_SUCCESS)
      MOP_WARN("[VK] tonemap framebuffer failed: %d", r);
  }

  /* ---- Overlay framebuffer (renders on LDR color image) ---- */
  if (dev->overlay_render_pass && fb->ldr_color_view) {
    VkImageView ov_views[1] = {fb->ldr_color_view};
    VkFramebufferCreateInfo ov_fb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = dev->overlay_render_pass,
        .attachmentCount = 1,
        .pAttachments = ov_views,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .layers = 1,
    };
    r = vkCreateFramebuffer(dev->device, &ov_fb_ci, NULL,
                            &fb->overlay_framebuffer);
    if (r != VK_SUCCESS)
      MOP_WARN("[VK] overlay framebuffer failed: %d", r);
  }

  /* ---- Readback staging buffers (host-visible, persistently mapped) ---- */
  size_t color_size = npixels * 4;
  size_t pick_size = npixels * sizeof(uint32_t);
  size_t depth_size = npixels * sizeof(float);

  r = mop_vk_create_buffer(dev->device, &dev->mem_props, color_size,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &fb->readback_color_buf, &fb->readback_color_mem);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb readback color buffer: %d", r);
    return;
  }
  r = vkMapMemory(dev->device, fb->readback_color_mem, 0, color_size, 0,
                  &fb->readback_color_mapped);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkMapMemory readback color failed: %d", r);
    return;
  }

  r = mop_vk_create_buffer(dev->device, &dev->mem_props, pick_size,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &fb->readback_pick_buf, &fb->readback_pick_mem);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb readback pick buffer: %d", r);
    return;
  }
  r = vkMapMemory(dev->device, fb->readback_pick_mem, 0, pick_size, 0,
                  &fb->readback_pick_mapped);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkMapMemory readback pick failed: %d", r);
    return;
  }

  r = mop_vk_create_buffer(dev->device, &dev->mem_props, depth_size,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &fb->readback_depth_buf, &fb->readback_depth_mem);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb readback depth buffer: %d", r);
    return;
  }
  r = vkMapMemory(dev->device, fb->readback_depth_mem, 0, depth_size, 0,
                  &fb->readback_depth_mapped);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkMapMemory readback depth failed: %d", r);
    return;
  }

  /* ---- CPU readback arrays ---- */
  fb->readback_color = malloc(color_size);
  if (!fb->readback_color) {
    MOP_ERROR("[VK] malloc readback_color failed");
    return;
  }
  fb->readback_pick = malloc(pick_size);
  if (!fb->readback_pick) {
    MOP_ERROR("[VK] malloc readback_pick failed");
    return;
  }
  fb->readback_depth = malloc(depth_size);
  if (!fb->readback_depth) {
    MOP_ERROR("[VK] malloc readback_depth failed");
    return;
  }

  /* ---- Per-frame dynamic UBO ---- */
  r = mop_vk_create_buffer(dev->device, &dev->mem_props, MOP_VK_UBO_SIZE,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &fb->ubo_buf, &fb->ubo_mem);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb UBO buffer: %d", r);
    return;
  }
  r = vkMapMemory(dev->device, fb->ubo_mem, 0, MOP_VK_UBO_SIZE, 0,
                  &fb->ubo_mapped);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkMapMemory UBO failed: %d", r);
    return;
  }
  fb->ubo_offset = 0;
}

static void vk_fb_destroy_attachments(MopRhiDevice *dev,
                                      MopRhiFramebuffer *fb) {
  VkDevice d = dev->device;

  if (fb->framebuffer)
    vkDestroyFramebuffer(d, fb->framebuffer, NULL);

  /* Tonemap framebuffer + LDR color */
  if (fb->overlay_framebuffer)
    vkDestroyFramebuffer(d, fb->overlay_framebuffer, NULL);
  if (fb->tonemap_framebuffer)
    vkDestroyFramebuffer(d, fb->tonemap_framebuffer, NULL);
  if (fb->ldr_color_view)
    vkDestroyImageView(d, fb->ldr_color_view, NULL);
  if (fb->ldr_color_image)
    vkDestroyImage(d, fb->ldr_color_image, NULL);
  if (fb->ldr_color_memory)
    vkFreeMemory(d, fb->ldr_color_memory, NULL);

  /* Color */
  if (fb->color_view)
    vkDestroyImageView(d, fb->color_view, NULL);
  if (fb->color_image)
    vkDestroyImage(d, fb->color_image, NULL);
  if (fb->color_memory)
    vkFreeMemory(d, fb->color_memory, NULL);

  /* Pick */
  if (fb->pick_view)
    vkDestroyImageView(d, fb->pick_view, NULL);
  if (fb->pick_image)
    vkDestroyImage(d, fb->pick_image, NULL);
  if (fb->pick_memory)
    vkFreeMemory(d, fb->pick_memory, NULL);

  /* Depth */
  if (fb->depth_view)
    vkDestroyImageView(d, fb->depth_view, NULL);
  if (fb->depth_image)
    vkDestroyImage(d, fb->depth_image, NULL);
  if (fb->depth_memory)
    vkFreeMemory(d, fb->depth_memory, NULL);

  /* MSAA render targets */
  if (fb->msaa_color_view)
    vkDestroyImageView(d, fb->msaa_color_view, NULL);
  if (fb->msaa_color_image)
    vkDestroyImage(d, fb->msaa_color_image, NULL);
  if (fb->msaa_color_memory)
    vkFreeMemory(d, fb->msaa_color_memory, NULL);

  if (fb->msaa_pick_view)
    vkDestroyImageView(d, fb->msaa_pick_view, NULL);
  if (fb->msaa_pick_image)
    vkDestroyImage(d, fb->msaa_pick_image, NULL);
  if (fb->msaa_pick_memory)
    vkFreeMemory(d, fb->msaa_pick_memory, NULL);

  if (fb->msaa_depth_view)
    vkDestroyImageView(d, fb->msaa_depth_view, NULL);
  if (fb->msaa_depth_image)
    vkDestroyImage(d, fb->msaa_depth_image, NULL);
  if (fb->msaa_depth_memory)
    vkFreeMemory(d, fb->msaa_depth_memory, NULL);

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
  if (fb->ubo_mapped)
    vkUnmapMemory(d, fb->ubo_mem);
  if (fb->ubo_buf)
    vkDestroyBuffer(d, fb->ubo_buf, NULL);
  if (fb->ubo_mem)
    vkFreeMemory(d, fb->ubo_mem, NULL);

  /* Instance buffer */
  if (fb->instance_mapped)
    vkUnmapMemory(d, fb->instance_mem);
  if (fb->instance_buf)
    vkDestroyBuffer(d, fb->instance_buf, NULL);
  if (fb->instance_mem)
    vkFreeMemory(d, fb->instance_mem, NULL);

  memset(fb, 0, sizeof(*fb));
}

/* =========================================================================
 * 6. framebuffer_create
 * ========================================================================= */

static MopRhiFramebuffer *
vk_framebuffer_create(MopRhiDevice *dev, const MopRhiFramebufferDesc *desc) {
  MopRhiFramebuffer *fb = calloc(1, sizeof(MopRhiFramebuffer));
  if (!fb)
    return NULL;

  vk_fb_create_attachments(dev, fb, desc->width, desc->height);
  return fb;
}

/* =========================================================================
 * 7. framebuffer_destroy
 * ========================================================================= */

static void vk_framebuffer_destroy(MopRhiDevice *dev, MopRhiFramebuffer *fb) {
  if (!fb)
    return;
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
  if (!fb)
    return;
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

  /* Reset UBO offset and instance offset */
  fb->ubo_offset = 0;
  fb->instance_offset = 0;

  /* Begin command buffer */
  vkResetCommandBuffer(dev->cmd_buf, 0);
  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer(dev->cmd_buf, &begin_info);

  /* GPU timestamp: top of pipe */
  if (dev->has_timestamp_queries) {
    vkCmdResetQueryPool(dev->cmd_buf, dev->timestamp_pool, 0, 2);
    vkCmdWriteTimestamp(dev->cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        dev->timestamp_pool, 0);
  }

  /* Begin render pass — reversed-Z clears depth to 0.0 */
  float depth_clear = dev->reverse_z ? 0.0f : 1.0f;
  uint32_t clear_count;
  VkClearValue clears[6];
  if (dev->msaa_samples > VK_SAMPLE_COUNT_1_BIT) {
    /* 6 attachments: 3 MSAA (cleared) + 3 resolve (DONT_CARE but must exist) */
    clears[0] = (VkClearValue){.color = {{clear_color.r, clear_color.g,
                                          clear_color.b, clear_color.a}}};
    clears[1] = (VkClearValue){.color = {{0}}}; /* MSAA picking */
    clears[2] =
        (VkClearValue){.depthStencil = {depth_clear, 0}}; /* MSAA depth */
    clears[3] = (VkClearValue){.color = {{0}}};           /* resolve color */
    clears[4] = (VkClearValue){.color = {{0}}};           /* resolve pick */
    clears[5] = (VkClearValue){.depthStencil = {0, 0}};   /* resolve depth */
    clear_count = 6;
  } else {
    clears[0] = (VkClearValue){.color = {{clear_color.r, clear_color.g,
                                          clear_color.b, clear_color.a}}};
    clears[1] = (VkClearValue){.color = {{0}}}; /* picking = 0 */
    clears[2] = (VkClearValue){.depthStencil = {depth_clear, 0}};
    clear_count = 3;
  }

  VkRenderPassBeginInfo rp_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = dev->render_pass,
      .framebuffer = fb->framebuffer,
      .renderArea = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}},
      .clearValueCount = clear_count,
      .pClearValues = clears,
  };
  vkCmdBeginRenderPass(dev->cmd_buf, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

  /* Shadow pass: transition shadow image to attachment layout and render
   * depth for each cascade.  Draw calls are deferred to main pass;
   * the shadow pass will be populated by the viewport core calling
   * vk_shadow_draw() per mesh before frame_begin's main render pass.
   * For now, just transition the shadow map if shadows are enabled. */
  if (dev->shadows_enabled && dev->shadow_image) {
    /* Transition shadow image layers to attachment-optimal before use.
     * Actual shadow draw commands will be recorded by the viewport core
     * through the shadow RHI extension functions. */
    VkImageMemoryBarrier shadow_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = dev->shadow_image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = MOP_VK_CASCADE_COUNT,
            },
    };
    vkCmdPipelineBarrier(dev->cmd_buf, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL,
                         0, NULL, 1, &shadow_barrier);
  }

  /* Set dynamic viewport + scissor */
  /* Negative viewport height flips Y to match OpenGL/CPU clip space
   * conventions.  This is core in Vulkan 1.1 (VK_KHR_maintenance1).
   * Without this, the scene is upside-down and winding order is reversed,
   * breaking backface culling. */
  VkViewport viewport = {
      .x = 0,
      .y = (float)fb->height,
      .width = (float)fb->width,
      .height = -(float)fb->height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  vkCmdSetViewport(dev->cmd_buf, 0, 1, &viewport);

  VkRect2D scissor = {
      .extent = {(uint32_t)fb->width, (uint32_t)fb->height},
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
  uint32_t key =
      mop_vk_pipeline_key(call->wireframe, call->depth_test,
                          call->backface_cull, call->blend_mode, vertex_stride);
  VkPipeline pipeline = mop_vk_get_pipeline(dev, key, vertex_stride);
  if (!pipeline) {
    MOP_ERROR("[VK] draw: pipeline NULL for key=%u stride=%u", key,
              vertex_stride);
    return;
  }

  vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

  /* Push constants: mat4 mvp + mat4 model = 128 bytes */
  float push_data[32];
  memcpy(push_data, call->mvp.d, 64);
  memcpy(push_data + 16, call->model.d, 64);
  vkCmdPushConstants(dev->cmd_buf, dev->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT, 0, 128, push_data);

  /* Write fragment UBO at current offset */
  VkDeviceSize aligned_size =
      mop_vk_align(sizeof(MopVkFragUniforms), dev->min_ubo_alignment);
  if (fb->ubo_offset + aligned_size > MOP_VK_UBO_SIZE) {
    MOP_WARN("[VK] UBO exhausted, skipping draw");
    return;
  }

  MopVkFragUniforms *ubo =
      (MopVkFragUniforms *)((uint8_t *)fb->ubo_mapped + fb->ubo_offset);
  ubo->light_dir[0] = call->light_dir.x;
  ubo->light_dir[1] = call->light_dir.y;
  ubo->light_dir[2] = call->light_dir.z;
  ubo->light_dir[3] = 0.0f;
  ubo->ambient = call->ambient;
  ubo->opacity = call->opacity;
  ubo->object_id = call->object_id;
  ubo->blend_mode = (int32_t)call->blend_mode;
  ubo->has_texture = call->texture ? 1 : 0;
  ubo->metallic = call->metallic;
  ubo->roughness = call->roughness;
  ubo->cam_pos[0] = call->cam_eye.x;
  ubo->cam_pos[1] = call->cam_eye.y;
  ubo->cam_pos[2] = call->cam_eye.z;
  ubo->cam_pos[3] = 0.0f;

  /* Multi-light: populate light array from draw call */
  ubo->num_lights = (int32_t)(call->light_count < MOP_VK_MAX_FRAG_LIGHTS
                                  ? call->light_count
                                  : MOP_VK_MAX_FRAG_LIGHTS);
  for (int i = 0; i < ubo->num_lights; i++) {
    const MopLight *src = &call->lights[i];
    MopVkLight *dst = &ubo->lights[i];
    dst->position[0] = src->position.x;
    dst->position[1] = src->position.y;
    dst->position[2] = src->position.z;
    dst->position[3] = (float)src->type;
    dst->direction[0] = src->direction.x;
    dst->direction[1] = src->direction.y;
    dst->direction[2] = src->direction.z;
    dst->direction[3] = 0.0f;
    dst->color[0] = src->color.r;
    dst->color[1] = src->color.g;
    dst->color[2] = src->color.b;
    dst->color[3] = src->intensity;
    dst->params[0] = src->range;
    dst->params[1] = src->spot_inner_cos;
    dst->params[2] = src->spot_outer_cos;
    dst->params[3] = src->active ? 1.0f : 0.0f;
  }
  /* Zero unused light slots */
  for (int i = ubo->num_lights; i < MOP_VK_MAX_FRAG_LIGHTS; i++) {
    memset(&ubo->lights[i], 0, sizeof(MopVkLight));
  }

  /* Shadow mapping data */
  ubo->shadows_enabled = dev->shadows_enabled ? 1 : 0;
  ubo->cascade_count = dev->shadows_enabled ? MOP_VK_CASCADE_COUNT : 0;
  ubo->_pad_shadow[0] = 0.0f;
  ubo->_pad_shadow[1] = 0.0f;
  if (dev->shadows_enabled) {
    for (int c = 0; c < MOP_VK_CASCADE_COUNT; c++) {
      memcpy(ubo->cascade_vp[c], dev->cascade_vp[c].d, 64);
    }
    for (int c = 0; c < MOP_VK_CASCADE_COUNT; c++) {
      ubo->cascade_splits[c] = dev->cascade_splits[c + 1];
    }
  } else {
    memset(ubo->cascade_vp, 0, sizeof(ubo->cascade_vp));
    memset(ubo->cascade_splits, 0, sizeof(ubo->cascade_splits));
  }

  /* Background (object_id 0) keeps exposure=1 so it doesn't shift with +/- */
  ubo->exposure = (call->object_id == 0) ? 1.0f : dev->hdr_exposure;

  uint32_t dynamic_offset = (uint32_t)fb->ubo_offset;
  fb->ubo_offset += aligned_size;

  /* Allocate descriptor set */
  VkDescriptorSetAllocateInfo ds_ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dev->desc_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dev->desc_set_layout,
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
      .range = sizeof(MopVkFragUniforms),
  };

  VkDescriptorImageInfo img_info = {
      .sampler = dev->default_sampler,
      .imageView = call->texture ? call->texture->view : dev->white_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  /* Shadow map sampler — use shadow array view if available, else white
   * fallback.  The comparison sampler requires a depth-compatible view;
   * when shadows are not enabled, bind the default sampler + white view
   * as a dummy (shader checks shadows_enabled before sampling). */
  VkDescriptorImageInfo shadow_img_info = {
      .sampler =
          dev->shadow_sampler ? dev->shadow_sampler : dev->default_sampler,
      .imageView =
          dev->shadow_array_view ? dev->shadow_array_view : dev->white_view,
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
  };
  /* If shadows not available, use shader-read layout with default sampler */
  if (!dev->shadow_array_view) {
    shadow_img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  /* IBL texture bindings — use real textures if available, black fallback */
  VkDescriptorImageInfo irr_img_info = {
      .sampler = dev->default_sampler,
      .imageView =
          dev->irradiance_view ? dev->irradiance_view : dev->black_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo pf_img_info = {
      .sampler = dev->default_sampler,
      .imageView =
          dev->prefiltered_view ? dev->prefiltered_view : dev->black_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo brdf_img_info = {
      .sampler = dev->default_sampler,
      .imageView = dev->brdf_lut_view ? dev->brdf_lut_view : dev->black_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  VkWriteDescriptorSet writes[6] = {
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo = &buf_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &img_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 2,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &shadow_img_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 3,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &irr_img_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 4,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &pf_img_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 5,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &brdf_img_info,
      },
  };
  vkUpdateDescriptorSets(dev->device, 6, writes, 0, NULL);

  /* Bind descriptor set with dynamic offset */
  vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          dev->pipeline_layout, 0, 1, &ds, 1, &dynamic_offset);

  /* Bind vertex + index buffers and draw */
  VkDeviceSize vb_offset = 0;
  vkCmdBindVertexBuffers(dev->cmd_buf, 0, 1, &call->vertex_buffer->buffer,
                         &vb_offset);
  vkCmdBindIndexBuffer(dev->cmd_buf, call->index_buffer->buffer, 0,
                       VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexed(dev->cmd_buf, call->index_count, 1, 0, 0, 0);
}

/* =========================================================================
 * 11. draw_instanced
 * ========================================================================= */

/* Ensure the per-frame instance buffer is large enough */
static void vk_ensure_instance_buffer(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                                      VkDeviceSize needed) {
  if (fb->instance_buf && fb->instance_buf_size >= needed)
    return;

  /* Destroy old buffer if any */
  if (fb->instance_mapped)
    vkUnmapMemory(dev->device, fb->instance_mem);
  if (fb->instance_buf)
    vkDestroyBuffer(dev->device, fb->instance_buf, NULL);
  if (fb->instance_mem)
    vkFreeMemory(dev->device, fb->instance_mem, NULL);

  /* Round up to next power of 2 (min 64 KB) */
  VkDeviceSize sz = 64 * 1024;
  while (sz < needed)
    sz *= 2;

  VkResult r = mop_vk_create_buffer(dev->device, &dev->mem_props, sz,
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    &fb->instance_buf, &fb->instance_mem);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] instance buffer alloc failed: %d", r);
    fb->instance_buf = VK_NULL_HANDLE;
    fb->instance_buf_size = 0;
    return;
  }

  r = vkMapMemory(dev->device, fb->instance_mem, 0, sz, 0,
                  &fb->instance_mapped);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] instance buffer map failed: %d", r);
    vkDestroyBuffer(dev->device, fb->instance_buf, NULL);
    vkFreeMemory(dev->device, fb->instance_mem, NULL);
    fb->instance_buf = VK_NULL_HANDLE;
    fb->instance_buf_size = 0;
    return;
  }
  fb->instance_buf_size = sz;
}

static void vk_draw_instanced(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                              const MopRhiDrawCall *call,
                              const MopMat4 *instance_transforms,
                              uint32_t instance_count) {
  if (!call || !instance_transforms || instance_count == 0)
    return;

  uint32_t vertex_stride = call->vertex_format
                               ? (uint32_t)call->vertex_format->stride
                               : (uint32_t)sizeof(MopVertex);

  /* Get instanced pipeline */
  VkPipeline pipeline = mop_vk_get_instanced_pipeline(dev, vertex_stride);
  if (!pipeline) {
    /* Fallback to N sequential draw calls */
    for (uint32_t i = 0; i < instance_count; i++) {
      MopRhiDrawCall inst_call = *call;
      inst_call.model = instance_transforms[i];
      MopMat4 vm = mop_mat4_multiply(call->view, instance_transforms[i]);
      inst_call.mvp = mop_mat4_multiply(call->projection, vm);
      vk_draw(dev, fb, &inst_call);
    }
    return;
  }

  /* Ensure instance buffer has room */
  VkDeviceSize inst_data_size = (VkDeviceSize)instance_count * sizeof(MopMat4);
  VkDeviceSize total_needed = fb->instance_offset + inst_data_size;
  vk_ensure_instance_buffer(dev, fb, total_needed);
  if (!fb->instance_buf)
    return;

  /* Write instance transforms to buffer */
  memcpy((uint8_t *)fb->instance_mapped + fb->instance_offset,
         instance_transforms, inst_data_size);
  VkDeviceSize inst_buf_offset = fb->instance_offset;
  fb->instance_offset += inst_data_size;

  /* Bind instanced pipeline */
  vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

  /* Push constants: VP matrix (view * projection) */
  MopMat4 vp = mop_mat4_multiply(call->projection, call->view);
  float push_data[32];
  memcpy(push_data, vp.d, 64);
  memset(push_data + 16, 0, 64); /* unused second mat4 */
  vkCmdPushConstants(dev->cmd_buf, dev->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT, 0, 128, push_data);

  /* Write fragment UBO (shared across all instances) */
  VkDeviceSize aligned_size =
      mop_vk_align(sizeof(MopVkFragUniforms), dev->min_ubo_alignment);
  if (fb->ubo_offset + aligned_size > MOP_VK_UBO_SIZE) {
    MOP_WARN("[VK] UBO exhausted in instanced draw");
    return;
  }

  MopVkFragUniforms *ubo =
      (MopVkFragUniforms *)((uint8_t *)fb->ubo_mapped + fb->ubo_offset);
  ubo->light_dir[0] = call->light_dir.x;
  ubo->light_dir[1] = call->light_dir.y;
  ubo->light_dir[2] = call->light_dir.z;
  ubo->light_dir[3] = 0.0f;
  ubo->ambient = call->ambient;
  ubo->opacity = call->opacity;
  ubo->object_id = call->object_id;
  ubo->blend_mode = (int32_t)call->blend_mode;
  ubo->has_texture = call->texture ? 1 : 0;
  ubo->metallic = call->metallic;
  ubo->roughness = call->roughness;
  ubo->cam_pos[0] = call->cam_eye.x;
  ubo->cam_pos[1] = call->cam_eye.y;
  ubo->cam_pos[2] = call->cam_eye.z;
  ubo->cam_pos[3] = 0.0f;

  ubo->num_lights = (int32_t)(call->light_count < MOP_VK_MAX_FRAG_LIGHTS
                                  ? call->light_count
                                  : MOP_VK_MAX_FRAG_LIGHTS);
  for (int i = 0; i < ubo->num_lights; i++) {
    const MopLight *src = &call->lights[i];
    MopVkLight *dst = &ubo->lights[i];
    dst->position[0] = src->position.x;
    dst->position[1] = src->position.y;
    dst->position[2] = src->position.z;
    dst->position[3] = (float)src->type;
    dst->direction[0] = src->direction.x;
    dst->direction[1] = src->direction.y;
    dst->direction[2] = src->direction.z;
    dst->direction[3] = 0.0f;
    dst->color[0] = src->color.r;
    dst->color[1] = src->color.g;
    dst->color[2] = src->color.b;
    dst->color[3] = src->intensity;
    dst->params[0] = src->range;
    dst->params[1] = src->spot_inner_cos;
    dst->params[2] = src->spot_outer_cos;
    dst->params[3] = src->active ? 1.0f : 0.0f;
  }
  for (int i = ubo->num_lights; i < MOP_VK_MAX_FRAG_LIGHTS; i++) {
    memset(&ubo->lights[i], 0, sizeof(MopVkLight));
  }

  /* Shadow mapping data (instanced path) */
  ubo->shadows_enabled = dev->shadows_enabled ? 1 : 0;
  ubo->cascade_count = dev->shadows_enabled ? MOP_VK_CASCADE_COUNT : 0;
  ubo->_pad_shadow[0] = 0.0f;
  ubo->_pad_shadow[1] = 0.0f;
  if (dev->shadows_enabled) {
    for (int c = 0; c < MOP_VK_CASCADE_COUNT; c++) {
      memcpy(ubo->cascade_vp[c], dev->cascade_vp[c].d, 64);
    }
    for (int c = 0; c < MOP_VK_CASCADE_COUNT; c++) {
      ubo->cascade_splits[c] = dev->cascade_splits[c + 1];
    }
  } else {
    memset(ubo->cascade_vp, 0, sizeof(ubo->cascade_vp));
    memset(ubo->cascade_splits, 0, sizeof(ubo->cascade_splits));
  }

  ubo->exposure = (call->object_id == 0) ? 1.0f : dev->hdr_exposure;

  uint32_t dynamic_offset = (uint32_t)fb->ubo_offset;
  fb->ubo_offset += aligned_size;

  /* Allocate + update descriptor set */
  VkDescriptorSetAllocateInfo ds_ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dev->desc_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dev->desc_set_layout,
  };
  VkDescriptorSet ds;
  VkResult r = vkAllocateDescriptorSets(dev->device, &ds_ai, &ds);
  if (r != VK_SUCCESS) {
    MOP_WARN("[VK] instanced descriptor alloc failed: %d", r);
    return;
  }

  VkDescriptorBufferInfo buf_info = {
      .buffer = fb->ubo_buf,
      .offset = 0,
      .range = sizeof(MopVkFragUniforms),
  };
  VkDescriptorImageInfo img_info = {
      .sampler = dev->default_sampler,
      .imageView = call->texture ? call->texture->view : dev->white_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo shadow_img_info = {
      .sampler =
          dev->shadow_sampler ? dev->shadow_sampler : dev->default_sampler,
      .imageView =
          dev->shadow_array_view ? dev->shadow_array_view : dev->white_view,
      .imageLayout = dev->shadow_array_view
                         ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  /* IBL texture bindings — same as regular draw path */
  VkDescriptorImageInfo irr_img_info = {
      .sampler = dev->default_sampler,
      .imageView =
          dev->irradiance_view ? dev->irradiance_view : dev->black_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo pf_img_info = {
      .sampler = dev->default_sampler,
      .imageView =
          dev->prefiltered_view ? dev->prefiltered_view : dev->black_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo brdf_img_info = {
      .sampler = dev->default_sampler,
      .imageView = dev->brdf_lut_view ? dev->brdf_lut_view : dev->black_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  VkWriteDescriptorSet writes[6] = {
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo = &buf_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &img_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 2,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &shadow_img_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 3,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &irr_img_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 4,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &pf_img_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 5,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &brdf_img_info,
      },
  };
  vkUpdateDescriptorSets(dev->device, 6, writes, 0, NULL);

  vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          dev->pipeline_layout, 0, 1, &ds, 1, &dynamic_offset);

  /* Bind vertex + instance + index buffers */
  VkBuffer vbufs[2] = {call->vertex_buffer->buffer, fb->instance_buf};
  VkDeviceSize offsets[2] = {0, inst_buf_offset};
  vkCmdBindVertexBuffers(dev->cmd_buf, 0, 2, vbufs, offsets);
  vkCmdBindIndexBuffer(dev->cmd_buf, call->index_buffer->buffer, 0,
                       VK_INDEX_TYPE_UINT32);

  /* Single instanced draw call */
  vkCmdDrawIndexed(dev->cmd_buf, call->index_count, instance_count, 0, 0, 0);
}

/* =========================================================================
 * 12. frame_end
 * ========================================================================= */

static void vk_frame_end(MopRhiDevice *dev, MopRhiFramebuffer *fb) {
  /* GPU timestamp: bottom of pipe (before ending render pass for correct
   * timing — we want rendering time, not including readback copies) */
  if (dev->has_timestamp_queries) {
    vkCmdWriteTimestamp(dev->cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        dev->timestamp_pool, 1);
  }

  vkCmdEndRenderPass(dev->cmd_buf);

  /* MSAA: all resolves happen in-pass via pResolveAttachments and
   * VkSubpassDescriptionDepthStencilResolve.  Render pass transitions
   * the 1x resolve targets to TRANSFER_SRC_OPTIMAL automatically. */

  /* ---- HDR Tonemap pass: color_image (HDR) → ldr_color_image (LDR) ---- */
  if (dev->tonemap_enabled && fb->tonemap_framebuffer) {
    /* Transition HDR color from TRANSFER_SRC → SHADER_READ_ONLY */
    mop_vk_transition_image(
        dev->cmd_buf, fb->color_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* Allocate descriptor set for HDR sampler */
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->tonemap_desc_layout,
    };
    VkDescriptorSet tm_ds;
    VkResult dr = vkAllocateDescriptorSets(dev->device, &ds_ai, &tm_ds);
    if (dr == VK_SUCCESS) {
      /* Update descriptor with HDR color image */
      VkDescriptorImageInfo img_info = {
          .sampler = dev->default_sampler,
          .imageView = fb->color_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      VkWriteDescriptorSet write = {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = tm_ds,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &img_info,
      };
      vkUpdateDescriptorSets(dev->device, 1, &write, 0, NULL);

      /* Begin tonemap render pass */
      VkClearValue clear = {.color = {{0, 0, 0, 1}}};
      VkRenderPassBeginInfo rp_bi = {
          .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
          .renderPass = dev->tonemap_render_pass,
          .framebuffer = fb->tonemap_framebuffer,
          .renderArea = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}},
          .clearValueCount = 1,
          .pClearValues = &clear,
      };
      vkCmdBeginRenderPass(dev->cmd_buf, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

      /* Set dynamic viewport/scissor — match scene's Y-flip viewport so the
       * fullscreen shader's UV flip produces correct orientation */
      VkViewport vp = {
          .y = (float)fb->height,
          .width = (float)fb->width,
          .height = -(float)fb->height,
          .maxDepth = 1.0f,
      };
      VkRect2D sc = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}};
      vkCmdSetViewport(dev->cmd_buf, 0, 1, &vp);
      vkCmdSetScissor(dev->cmd_buf, 0, 1, &sc);

      /* Bind tonemap pipeline and push exposure */
      vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dev->tonemap_pipeline);
      vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              dev->tonemap_pipeline_layout, 0, 1, &tm_ds, 0,
                              NULL);
      float exposure =
          1.0f; /* scene objects already have exposure in frag UBO */
      vkCmdPushConstants(dev->cmd_buf, dev->tonemap_pipeline_layout,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float),
                         &exposure);
      vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0); /* fullscreen triangle */

      vkCmdEndRenderPass(dev->cmd_buf);
      /* tonemap render pass transitions ldr_color_image to
       * TRANSFER_SRC_OPTIMAL */
    }
  }

  /* Copy images to readback staging buffers.
   * Render pass transitions images to TRANSFER_SRC_OPTIMAL. */

  /* Color readback — from LDR image (tonemapped) if available,
   * otherwise fall back to HDR color image */
  {
    VkImage readback_src = (dev->tonemap_enabled && fb->ldr_color_image)
                               ? fb->ldr_color_image
                               : fb->color_image;
    VkBufferImageCopy region = {
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .layerCount = 1,
            },
        .imageExtent = {(uint32_t)fb->width, (uint32_t)fb->height, 1},
    };
    vkCmdCopyImageToBuffer(dev->cmd_buf, readback_src,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           fb->readback_color_buf, 1, &region);
  }

  /* Picking readback */
  {
    VkBufferImageCopy region = {
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .layerCount = 1,
            },
        .imageExtent = {(uint32_t)fb->width, (uint32_t)fb->height, 1},
    };
    vkCmdCopyImageToBuffer(dev->cmd_buf, fb->pick_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           fb->readback_pick_buf, 1, &region);
  }

  /* Depth readback */
  {
    VkBufferImageCopy region = {
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .layerCount = 1,
            },
        .imageExtent = {(uint32_t)fb->width, (uint32_t)fb->height, 1},
    };
    vkCmdCopyImageToBuffer(dev->cmd_buf, fb->depth_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           fb->readback_depth_buf, 1, &region);
  }

  /* Submit and wait */
  vkEndCommandBuffer(dev->cmd_buf);

  VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &dev->cmd_buf,
  };
  vkQueueSubmit(dev->queue, 1, &submit, dev->fence);
  vkWaitForFences(dev->device, 1, &dev->fence, VK_TRUE, UINT64_MAX);

  /* Read GPU timestamps */
  if (dev->has_timestamp_queries) {
    uint64_t timestamps[2] = {0, 0};
    VkResult tr = vkGetQueryPoolResults(
        dev->device, dev->timestamp_pool, 0, 2, sizeof(timestamps), timestamps,
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (tr == VK_SUCCESS && timestamps[1] >= timestamps[0]) {
      double elapsed_ns =
          (double)(timestamps[1] - timestamps[0]) * dev->timestamp_period_ns;
      dev->last_timing.gpu_frame_ms = elapsed_ns / 1e6;
    }
  }

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
  if (out_width)
    *out_width = fb->width;
  if (out_height)
    *out_height = fb->height;
  return fb->readback_color;
}

/* =========================================================================
 * 13b. framebuffer_read_object_id
 * ========================================================================= */

static const uint32_t *vk_framebuffer_read_object_id(MopRhiDevice *dev,
                                                     MopRhiFramebuffer *fb,
                                                     int *out_width,
                                                     int *out_height) {
  (void)dev;
  if (out_width)
    *out_width = fb->width;
  if (out_height)
    *out_height = fb->height;
  return fb->readback_pick;
}

static const float *vk_framebuffer_read_depth(MopRhiDevice *dev,
                                              MopRhiFramebuffer *fb,
                                              int *out_width, int *out_height) {
  (void)dev;
  if (out_width)
    *out_width = fb->width;
  if (out_height)
    *out_height = fb->height;
  return fb->readback_depth;
}

/* =========================================================================
 * 14. pick_read_id
 * ========================================================================= */

static uint32_t vk_pick_read_id(MopRhiDevice *dev, MopRhiFramebuffer *fb, int x,
                                int y) {
  (void)dev;
  if (x < 0 || x >= fb->width || y < 0 || y >= fb->height)
    return 0;
  /* Vulkan is top-left origin — no flip needed */
  return fb->readback_pick[(size_t)y * (size_t)fb->width + (size_t)x];
}

/* =========================================================================
 * 15. pick_read_depth
 * ========================================================================= */

static float vk_pick_read_depth(MopRhiDevice *dev, MopRhiFramebuffer *fb, int x,
                                int y) {
  (void)dev;
  if (x < 0 || x >= fb->width || y < 0 || y >= fb->height)
    return 1.0f;
  return fb->readback_depth[(size_t)y * (size_t)fb->width + (size_t)x];
}

/* =========================================================================
 * 16. texture_create
 * ========================================================================= */

static MopRhiTexture *vk_texture_create(MopRhiDevice *dev, int width,
                                        int height, const uint8_t *rgba_data) {
  MopRhiTexture *tex = calloc(1, sizeof(MopRhiTexture));
  if (!tex)
    return NULL;

  tex->width = width;
  tex->height = height;

  VkResult r = mop_vk_create_image(
      dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
      VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &tex->image,
      &tex->memory);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] texture image create failed: %d", r);
    free(tex);
    return NULL;
  }

  r = mop_vk_create_image_view(dev->device, tex->image,
                               VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_ASPECT_COLOR_BIT, &tex->view);
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
 * 16b. texture_create_hdr (R32G32B32A32_SFLOAT)
 * ========================================================================= */

static void staging_upload_image_float(MopRhiDevice *dev, VkImage image,
                                       uint32_t width, uint32_t height,
                                       const float *rgba_data) {
  size_t row_bytes = (size_t)width * 4 * sizeof(float);
  uint32_t rows_per_batch = (uint32_t)(MOP_VK_STAGING_SIZE / row_bytes);
  if (rows_per_batch == 0) {
    MOP_ERROR("[VK] HDR image row too large for staging: %zu", row_bytes);
    return;
  }
  if (rows_per_batch > height)
    rows_per_batch = height;

  /* Transition to TRANSFER_DST once */
  {
    VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);
    mop_vk_transition_image(
        cb, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    mop_vk_end_oneshot(dev->device, dev->queue, dev->cmd_pool, cb);
  }

  /* Upload in row batches */
  for (uint32_t y = 0; y < height; y += rows_per_batch) {
    uint32_t batch_rows =
        (y + rows_per_batch > height) ? (height - y) : rows_per_batch;
    size_t batch_bytes = (size_t)batch_rows * row_bytes;

    memcpy(dev->staging_mapped, rgba_data + (size_t)y * width * 4, batch_bytes);

    VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .layerCount = 1,
            },
        .imageOffset = {0, (int32_t)y, 0},
        .imageExtent = {width, batch_rows, 1},
    };
    vkCmdCopyBufferToImage(cb, dev->staging_buf, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    mop_vk_end_oneshot(dev->device, dev->queue, dev->cmd_pool, cb);
  }

  /* Transition to SHADER_READ_ONLY */
  {
    VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);
    mop_vk_transition_image(
        cb, image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    mop_vk_end_oneshot(dev->device, dev->queue, dev->cmd_pool, cb);
  }
}

static MopRhiTexture *vk_texture_create_hdr(MopRhiDevice *dev, int width,
                                            int height,
                                            const float *rgba_float_data) {
  MopRhiTexture *tex = calloc(1, sizeof(MopRhiTexture));
  if (!tex)
    return NULL;

  tex->width = width;
  tex->height = height;
  tex->is_hdr = true;

  VkResult r = mop_vk_create_image(
      dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
      VK_FORMAT_R32G32B32A32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &tex->image,
      &tex->memory);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] HDR texture image create failed: %d", r);
    free(tex);
    return NULL;
  }

  r = mop_vk_create_image_view(dev->device, tex->image,
                               VK_FORMAT_R32G32B32A32_SFLOAT,
                               VK_IMAGE_ASPECT_COLOR_BIT, &tex->view);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] HDR texture view create failed: %d", r);
    vkDestroyImage(dev->device, tex->image, NULL);
    vkFreeMemory(dev->device, tex->memory, NULL);
    free(tex);
    return NULL;
  }

  staging_upload_image_float(dev, tex->image, (uint32_t)width, (uint32_t)height,
                             rgba_float_data);

  return tex;
}

/* =========================================================================
 * 17. texture_destroy
 * ========================================================================= */

static void vk_texture_destroy(MopRhiDevice *dev, MopRhiTexture *tex) {
  if (!tex)
    return;
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
 * 19. frame_gpu_time_ms
 * ========================================================================= */

static float vk_frame_gpu_time_ms(MopRhiDevice *dev) {
  return (float)dev->last_timing.gpu_frame_ms;
}

static void vk_set_exposure(MopRhiDevice *dev, float exposure) {
  dev->hdr_exposure = exposure > 0.0f ? exposure : 0.001f;
}

static void vk_set_ibl_textures(MopRhiDevice *dev, MopRhiTexture *irradiance,
                                MopRhiTexture *prefiltered,
                                MopRhiTexture *brdf_lut) {
  dev->irradiance_view = irradiance ? irradiance->view : VK_NULL_HANDLE;
  dev->prefiltered_view = prefiltered ? prefiltered->view : VK_NULL_HANDLE;
  dev->brdf_lut_view = brdf_lut ? brdf_lut->view : VK_NULL_HANDLE;
  dev->env_map_view = VK_NULL_HANDLE; /* set separately for skybox */
}

/* =========================================================================
 * Shadow mapping: cascade computation
 * ========================================================================= */

/* Compute cascade split distances and light-space VP matrices.
 * Called from the viewport core before shadow rendering. */
void vk_compute_cascades(MopRhiDevice *dev, const MopMat4 *view,
                         const MopMat4 *proj, MopVec3 light_dir) {
  /* Compute cascade split distances using logarithmic scheme */
  float near_clip = 0.1f;
  float far_clip = 100.0f;

  /* Extract near/far from projection matrix (standard perspective) */
  /* M(2,3) = -2fn/(f-n), M(2,2) = -(f+n)/(f-n) */
  float m22 = proj->d[2 * 4 + 2]; /* col-major: d[10] */
  float m23 = proj->d[3 * 4 + 2]; /* col-major: d[14] */

  if (dev->reverse_z) {
    /* reverse_z: near_plane = M(2,3) = proj->d[14] */
    near_clip = m23;
    far_clip = 200.0f; /* infinite far, use practical value */
  } else {
    /* Standard perspective: near = m23 / (m22 - 1), far = m23 / (m22 + 1) */
    if (fabsf(m22 - 1.0f) > 1e-6f)
      near_clip = m23 / (m22 - 1.0f);
    if (fabsf(m22 + 1.0f) > 1e-6f)
      far_clip = m23 / (m22 + 1.0f);
  }

  if (near_clip <= 0.0f)
    near_clip = 0.1f;
  if (far_clip <= near_clip)
    far_clip = near_clip + 100.0f;

  dev->cascade_splits[0] = near_clip;
  for (int i = 1; i <= MOP_VK_CASCADE_COUNT; i++) {
    float p = (float)i / (float)MOP_VK_CASCADE_COUNT;
    float log_split = near_clip * powf(far_clip / near_clip, p);
    float lin_split = near_clip + (far_clip - near_clip) * p;
    dev->cascade_splits[i] = 0.95f * log_split + 0.05f * lin_split;
  }

  /* Normalize light direction */
  float llen = sqrtf(light_dir.x * light_dir.x + light_dir.y * light_dir.y +
                     light_dir.z * light_dir.z);
  if (llen > 1e-6f) {
    light_dir.x /= llen;
    light_dir.y /= llen;
    light_dir.z /= llen;
  }

  /* Compute inverse view-projection */
  MopMat4 vp = mop_mat4_multiply(*proj, *view);
  MopMat4 inv_vp = mop_mat4_inverse(vp);

  /* For each cascade, compute the light VP matrix */
  for (int c = 0; c < MOP_VK_CASCADE_COUNT; c++) {
    float split_near = dev->cascade_splits[c];
    float split_far = dev->cascade_splits[c + 1];

    /* Remap split distances to NDC Z range [0,1] */
    float ndc_near, ndc_far;
    if (dev->reverse_z) {
      ndc_near = near_clip / split_near; /* reverse_z: z = near/z_eye */
      ndc_far = near_clip / split_far;
    } else {
      /* Standard: z_ndc = (A*z_eye + B) / (-z_eye), A = -(f+n)/(f-n), B =
       * -2fn/(f-n) */
      ndc_near = (-m22 * (-split_near) + m23) / split_near;
      ndc_far = (-m22 * (-split_far) + m23) / split_far;
    }

    /* Frustum corners in NDC space */
    MopVec4 frustum_corners[8] = {
        {-1, -1, ndc_near, 1}, {1, -1, ndc_near, 1}, {1, 1, ndc_near, 1},
        {-1, 1, ndc_near, 1},  {-1, -1, ndc_far, 1}, {1, -1, ndc_far, 1},
        {1, 1, ndc_far, 1},    {-1, 1, ndc_far, 1},
    };

    /* Transform frustum corners to world space */
    MopVec3 world_corners[8];
    MopVec3 center = {0, 0, 0};
    for (int i = 0; i < 8; i++) {
      MopVec4 ws = mop_mat4_mul_vec4(inv_vp, frustum_corners[i]);
      if (fabsf(ws.w) > 1e-6f) {
        ws.x /= ws.w;
        ws.y /= ws.w;
        ws.z /= ws.w;
      }
      world_corners[i] = (MopVec3){ws.x, ws.y, ws.z};
      center.x += ws.x;
      center.y += ws.y;
      center.z += ws.z;
    }
    center.x /= 8.0f;
    center.y /= 8.0f;
    center.z /= 8.0f;

    /* Light view matrix: look from center along light direction */
    MopVec3 light_eye = {center.x - light_dir.x * 50.0f,
                         center.y - light_dir.y * 50.0f,
                         center.z - light_dir.z * 50.0f};
    MopMat4 light_view =
        mop_mat4_look_at(light_eye, center, (MopVec3){0, 1, 0});

    /* Find AABB of frustum corners in light space */
    float min_x = 1e30f, max_x = -1e30f;
    float min_y = 1e30f, max_y = -1e30f;
    float min_z = 1e30f, max_z = -1e30f;

    for (int i = 0; i < 8; i++) {
      MopVec4 ls = mop_mat4_mul_vec4(
          light_view, (MopVec4){world_corners[i].x, world_corners[i].y,
                                world_corners[i].z, 1.0f});
      if (ls.x < min_x)
        min_x = ls.x;
      if (ls.x > max_x)
        max_x = ls.x;
      if (ls.y < min_y)
        min_y = ls.y;
      if (ls.y > max_y)
        max_y = ls.y;
      if (ls.z < min_z)
        min_z = ls.z;
      if (ls.z > max_z)
        max_z = ls.z;
    }

    /* Add margin for shadow casters behind the frustum */
    float z_margin = 50.0f;
    min_z -= z_margin;

    MopMat4 light_proj =
        mop_mat4_ortho(min_x, max_x, min_y, max_y, min_z, max_z);
    dev->cascade_vp[c] = mop_mat4_multiply(light_proj, light_view);
  }
}

/* =========================================================================
 * Backend function table
 * ========================================================================= */

/* Skybox UBO — must match SkyboxUBO in mop_skybox.frag */
typedef struct MopVkSkyboxUBO {
  float inv_view_proj[16]; /* mat4 */
  float cam_pos[4];        /* vec4 (xyz + pad) */
  float rotation;
  float intensity;
  float _pad[2];
} MopVkSkyboxUBO; /* 96 bytes */

static void vk_draw_skybox(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                           MopRhiTexture *env_map, const float *inv_vp,
                           const float *cam_pos, float rotation,
                           float intensity) {
  if (!dev->skybox_pipeline || !env_map)
    return;

  /* Write UBO data into the shared UBO buffer */
  size_t aligned_size =
      mop_vk_align(sizeof(MopVkSkyboxUBO), dev->min_ubo_alignment);
  if (fb->ubo_offset + aligned_size > MOP_VK_UBO_SIZE)
    return;

  MopVkSkyboxUBO *ubo =
      (MopVkSkyboxUBO *)((uint8_t *)fb->ubo_mapped + fb->ubo_offset);
  memcpy(ubo->inv_view_proj, inv_vp, 64);
  ubo->cam_pos[0] = cam_pos[0];
  ubo->cam_pos[1] = cam_pos[1];
  ubo->cam_pos[2] = cam_pos[2];
  ubo->cam_pos[3] = 0.0f;
  ubo->rotation = rotation;
  ubo->intensity = intensity;

  uint32_t dynamic_offset = (uint32_t)fb->ubo_offset;
  fb->ubo_offset += aligned_size;

  /* Allocate descriptor set */
  VkDescriptorSetAllocateInfo ds_ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dev->desc_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dev->skybox_desc_layout,
  };
  VkDescriptorSet ds;
  VkResult r = vkAllocateDescriptorSets(dev->device, &ds_ai, &ds);
  if (r != VK_SUCCESS)
    return;

  /* Update descriptors: UBO + env map sampler */
  VkDescriptorBufferInfo buf_info = {
      .buffer = fb->ubo_buf,
      .offset = 0,
      .range = sizeof(MopVkSkyboxUBO),
  };
  VkDescriptorImageInfo img_info = {
      .sampler = dev->default_sampler,
      .imageView = env_map->view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkWriteDescriptorSet writes[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo = &buf_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &img_info,
      },
  };
  vkUpdateDescriptorSets(dev->device, 2, writes, 0, NULL);

  /* Bind skybox pipeline and draw fullscreen triangle */
  vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    dev->skybox_pipeline);
  vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          dev->skybox_pipeline_layout, 0, 1, &ds, 1,
                          &dynamic_offset);
  vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0);
}

/* =========================================================================
 * SDF overlay draw — uploads prim SSBO, runs fullscreen overlay shader
 * ========================================================================= */

static void vk_draw_overlays(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                             const void *prims, uint32_t prim_count,
                             const void *grid_params, int fb_width,
                             int fb_height) {
  if (!fb->overlay_framebuffer)
    return;
  bool has_prims = dev->overlay_enabled && prim_count > 0;
  bool has_grid = dev->grid_enabled && grid_params != NULL;
  if (!has_prims && !has_grid)
    return;

  /* Create temporary SSBO for prim data (only if we have prims) */
  VkBuffer ssbo_buf = VK_NULL_HANDLE;
  VkDeviceMemory ssbo_mem = VK_NULL_HANDLE;
  size_t ssbo_size = 0;
  if (has_prims) {
    ssbo_size = (size_t)prim_count * 3 * sizeof(float) * 4;
    if (ssbo_size > MOP_VK_STAGING_SIZE)
      has_prims = false;
    else {
      VkResult r = mop_vk_create_buffer(
          dev->device, &dev->mem_props, ssbo_size,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
          &ssbo_buf, &ssbo_mem);
      if (r != VK_SUCCESS)
        has_prims = false;
    }
  }

  if (!has_prims && !has_grid)
    return;

  /* Map and fill SSBO if we have prims */
  if (has_prims) {
    void *ssbo_mapped = NULL;
    VkResult r =
        vkMapMemory(dev->device, ssbo_mem, 0, ssbo_size, 0, &ssbo_mapped);
    if (r != VK_SUCCESS) {
      vkDestroyBuffer(dev->device, ssbo_buf, NULL);
      vkFreeMemory(dev->device, ssbo_mem, NULL);
      has_prims = false;
    } else {
      const uint8_t *src = (const uint8_t *)prims;
      float *dst = (float *)ssbo_mapped;
      for (uint32_t i = 0; i < prim_count; i++) {
        const float *pf = (const float *)(src + i * 48);
        dst[i * 12 + 0] = pf[0];
        dst[i * 12 + 1] = pf[1];
        dst[i * 12 + 2] = pf[2];
        dst[i * 12 + 3] = pf[3];
        dst[i * 12 + 4] = pf[4];
        dst[i * 12 + 5] = pf[5];
        dst[i * 12 + 6] = pf[6];
        dst[i * 12 + 7] = pf[7];
        dst[i * 12 + 8] = pf[8];
        dst[i * 12 + 9] = pf[9];
        int32_t type_val;
        memcpy(&type_val, &pf[10], sizeof(int32_t));
        dst[i * 12 + 10] = (float)type_val;
        dst[i * 12 + 11] = pf[11];
      }
      vkUnmapMemory(dev->device, ssbo_mem);
    }
  }

  if (!has_prims && !has_grid)
    return;

  VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);

  /* Upload readback buffer → LDR image so CPU-drawn overlays (outline)
   * are baked into the LDR before we render grid + gizmos on top. */
  if (fb->readback_color && fb->readback_color_mapped) {
    size_t npx = (size_t)fb_width * (size_t)fb_height;
    memcpy(fb->readback_color_mapped, fb->readback_color, npx * 4);
  }
  {
    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = fb->ldr_color_image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1,
                             .layerCount = 1},
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &to_dst);
    VkBufferImageCopy upload_region = {
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1},
        .imageExtent = {(uint32_t)fb_width, (uint32_t)fb_height, 1},
    };
    vkCmdCopyBufferToImage(cb, fb->readback_color_buf, fb->ldr_color_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &upload_region);
    VkImageMemoryBarrier to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = fb->ldr_color_image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1,
                             .layerCount = 1},
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 1, &to_src);
  }

  /* Transition resolved depth: TRANSFER_SRC → SHADER_READ_ONLY for sampling */
  {
    VkImageMemoryBarrier depth_to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image = fb->depth_image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                             .levelCount = 1,
                             .layerCount = 1},
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &depth_to_read);
  }

  /* Begin overlay render pass */
  VkRenderPassBeginInfo rp_bi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = dev->overlay_render_pass,
      .framebuffer = fb->overlay_framebuffer,
      .renderArea = {.extent = {(uint32_t)fb_width, (uint32_t)fb_height}},
      .clearValueCount = 0,
  };
  vkCmdBeginRenderPass(cb, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp = {
      .y = (float)fb_height,
      .width = (float)fb_width,
      .height = -(float)fb_height,
      .maxDepth = 1.0f,
  };
  VkRect2D sc = {.extent = {(uint32_t)fb_width, (uint32_t)fb_height}};
  vkCmdSetViewport(cb, 0, 1, &vp);
  vkCmdSetScissor(cb, 0, 1, &sc);

  /* --- GPU Grid draw (rendered first, underneath gizmos) --- */
  if (has_grid) {
    VkDescriptorSetAllocateInfo grid_ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->grid_desc_layout,
    };
    VkDescriptorSet grid_ds;
    VkResult gr = vkAllocateDescriptorSets(dev->device, &grid_ds_ai, &grid_ds);
    if (gr == VK_SUCCESS) {
      VkDescriptorImageInfo grid_depth = {
          .sampler = dev->default_sampler,
          .imageView = fb->depth_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      VkWriteDescriptorSet grid_write = {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = grid_ds,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &grid_depth,
      };
      vkUpdateDescriptorSets(dev->device, 1, &grid_write, 0, NULL);

      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dev->grid_pipeline);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              dev->grid_pipeline_layout, 0, 1, &grid_ds, 0,
                              NULL);

      /* Pack grid push constants to match shader std430 layout.
       * vec4 members align to 16 bytes. After 19 scalars (76 bytes),
       * the first vec4 starts at offset 80 → 1 float pad needed. */
      const MopGridParams *gp = (const MopGridParams *)grid_params;
      struct {
        float Hi[9];               /* 0..35 */
        float vp_z0, vp_z2, vp_z3; /* 36..47 */
        float vp_w0, vp_w2, vp_w3; /* 48..59 */
        float grid_half;           /* 60 */
        uint32_t reverse_z;        /* 64 */
        float axis_half_width;     /* 68 */
        float _pad0;               /* 72 */
        float _pad1;               /* 76 — aligns vec4 to offset 80 */
        float minor_color[4];      /* 80..95 */
        float major_color[4];      /* 96..111 */
        float axis_x_color[4];     /* 112..127 */
        float axis_z_color[4];     /* 128..143 */
      } grid_push;
      _Static_assert(sizeof(grid_push) == 144, "grid push constant size");
      for (int i = 0; i < 9; i++)
        grid_push.Hi[i] = gp->Hi[i];
      grid_push.vp_z0 = gp->vp_z0;
      grid_push.vp_z2 = gp->vp_z2;
      grid_push.vp_z3 = gp->vp_z3;
      grid_push.vp_w0 = gp->vp_w0;
      grid_push.vp_w2 = gp->vp_w2;
      grid_push.vp_w3 = gp->vp_w3;
      grid_push.grid_half = gp->grid_half;
      grid_push.reverse_z = gp->reverse_z ? 1u : 0u;
      grid_push.axis_half_width = gp->axis_half_width;
      grid_push._pad0 = 0.0f;
      grid_push._pad1 = 0.0f;
      grid_push.minor_color[0] = gp->minor_color.r;
      grid_push.minor_color[1] = gp->minor_color.g;
      grid_push.minor_color[2] = gp->minor_color.b;
      grid_push.minor_color[3] = gp->minor_color.a;
      grid_push.major_color[0] = gp->major_color.r;
      grid_push.major_color[1] = gp->major_color.g;
      grid_push.major_color[2] = gp->major_color.b;
      grid_push.major_color[3] = gp->major_color.a;
      grid_push.axis_x_color[0] = gp->axis_x_color.r;
      grid_push.axis_x_color[1] = gp->axis_x_color.g;
      grid_push.axis_x_color[2] = gp->axis_x_color.b;
      grid_push.axis_x_color[3] = gp->axis_x_color.a;
      grid_push.axis_z_color[0] = gp->axis_z_color.r;
      grid_push.axis_z_color[1] = gp->axis_z_color.g;
      grid_push.axis_z_color[2] = gp->axis_z_color.b;
      grid_push.axis_z_color[3] = gp->axis_z_color.a;

      vkCmdPushConstants(cb, dev->grid_pipeline_layout,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(grid_push),
                         &grid_push);
      vkCmdDraw(cb, 3, 1, 0, 0);
    }
  }

  /* --- SDF overlay primitives (gizmos, lights, etc.) --- */
  if (has_prims) {
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->overlay_desc_layout,
    };
    VkDescriptorSet ds;
    VkResult dr = vkAllocateDescriptorSets(dev->device, &ds_ai, &ds);
    if (dr == VK_SUCCESS) {
      VkDescriptorImageInfo depth_info = {
          .sampler = dev->default_sampler,
          .imageView = fb->depth_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      VkDescriptorBufferInfo ssbo_info = {
          .buffer = ssbo_buf,
          .offset = 0,
          .range = ssbo_size,
      };
      VkWriteDescriptorSet writes[2] = {
          {
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet = ds,
              .dstBinding = 0,
              .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .pImageInfo = &depth_info,
          },
          {
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet = ds,
              .dstBinding = 1,
              .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              .pBufferInfo = &ssbo_info,
          },
      };
      vkUpdateDescriptorSets(dev->device, 2, writes, 0, NULL);

      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dev->overlay_pipeline);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              dev->overlay_pipeline_layout, 0, 1, &ds, 0, NULL);

      struct {
        uint32_t prim_count;
        float fb_width;
        float fb_height;
        uint32_t reverse_z;
      } push = {
          .prim_count = prim_count,
          .fb_width = (float)fb_width,
          .fb_height = (float)fb_height,
          .reverse_z = dev->reverse_z ? 1u : 0u,
      };
      vkCmdPushConstants(cb, dev->overlay_pipeline_layout,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
      vkCmdDraw(cb, 3, 1, 0, 0);
    }
  }

  vkCmdEndRenderPass(cb);

  /* Transition depth back to TRANSFER_SRC (original layout after render pass)
   */
  {
    VkImageMemoryBarrier depth_to_xfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = fb->depth_image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                             .levelCount = 1,
                             .layerCount = 1},
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &depth_to_xfer);
  }

  /* Copy LDR back to readback */
  {
    VkBufferImageCopy region = {
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1},
        .imageExtent = {(uint32_t)fb_width, (uint32_t)fb_height, 1},
    };
    vkCmdCopyImageToBuffer(cb, fb->ldr_color_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           fb->readback_color_buf, 1, &region);
  }

  mop_vk_end_oneshot(dev->device, dev->queue, dev->cmd_pool, cb);

  size_t npixels = (size_t)fb_width * (size_t)fb_height;
  if (fb->readback_color && fb->readback_color_mapped)
    memcpy(fb->readback_color, fb->readback_color_mapped, npixels * 4);

  if (ssbo_buf) {
    vkDestroyBuffer(dev->device, ssbo_buf, NULL);
    vkFreeMemory(dev->device, ssbo_mem, NULL);
  }
}

static const MopRhiBackend VK_BACKEND = {
    .name = "vulkan",
    .device_create = vk_device_create,
    .device_destroy = vk_device_destroy,
    .buffer_create = vk_buffer_create,
    .buffer_destroy = vk_buffer_destroy,
    .framebuffer_create = vk_framebuffer_create,
    .framebuffer_destroy = vk_framebuffer_destroy,
    .framebuffer_resize = vk_framebuffer_resize,
    .frame_begin = vk_frame_begin,
    .frame_end = vk_frame_end,
    .draw = vk_draw,
    .pick_read_id = vk_pick_read_id,
    .pick_read_depth = vk_pick_read_depth,
    .framebuffer_read_color = vk_framebuffer_read_color,
    .framebuffer_read_object_id = vk_framebuffer_read_object_id,
    .framebuffer_read_depth = vk_framebuffer_read_depth,
    .texture_create = vk_texture_create,
    .texture_create_hdr = vk_texture_create_hdr,
    .texture_destroy = vk_texture_destroy,
    .draw_instanced = vk_draw_instanced,
    .buffer_update = vk_buffer_update,
    .buffer_read = vk_buffer_read,
    .frame_gpu_time_ms = vk_frame_gpu_time_ms,
    .set_exposure = vk_set_exposure,
    .set_ibl_textures = vk_set_ibl_textures,
    .draw_skybox = vk_draw_skybox,
    .draw_overlays = vk_draw_overlays,
};

const MopRhiBackend *mop_rhi_backend_vulkan(void) { return &VK_BACKEND; }

#endif /* MOP_HAS_VULKAN */
