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
#include <sys/stat.h>
#include <unistd.h>

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

/* Submit a staging command buffer and wait via the staging fence.
 * Replaces vkQueueWaitIdle with fence tracking for correctness and
 * to allow future async staging improvements. */
static void staging_submit_and_wait(MopRhiDevice *dev, VkCommandBuffer cb) {
  vkEndCommandBuffer(cb);
  VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cb,
  };
  vkQueueSubmit(dev->queue, 1, &si, dev->staging_fence);
  vkWaitForFences(dev->device, 1, &dev->staging_fence, VK_TRUE, UINT64_MAX);
  vkResetFences(dev->device, 1, &dev->staging_fence);
  vkFreeCommandBuffers(dev->device, dev->cmd_pool, 1, &cb);
}

static void staging_upload(MopRhiDevice *dev, VkBuffer dst, const void *data,
                           size_t size) {
  if (size > MOP_VK_STAGING_SIZE) {
    MOP_ERROR("[VK] staging upload too large: %zu > %zu", size,
              (size_t)MOP_VK_STAGING_SIZE);
    return;
  }

  memcpy(dev->staging_mapped, data, size);

  VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);

  VkBufferCopy region = {.size = size};
  vkCmdCopyBuffer(cb, dev->staging_buf, dst, 1, &region);

  staging_submit_and_wait(dev, cb);
}

/* =========================================================================
 * Helper: upload image data through staging buffer
 * ========================================================================= */

static void staging_upload_image(MopRhiDevice *dev, VkImage image,
                                 uint32_t width, uint32_t height,
                                 const uint8_t *rgba_data) {
  size_t size = (size_t)width * height * 4;

  /* Fit in persistent staging → use it directly. Oversize → allocate
   * a transient host-visible buffer for this one upload. Keeps the
   * idle staging footprint small (16 MB) while supporting 4K and up. */
  VkBuffer upl_buf = VK_NULL_HANDLE;
  VkDeviceMemory upl_mem = VK_NULL_HANDLE;
  void *upl_mapped = NULL;
  bool transient = false;

  if (size <= MOP_VK_STAGING_SIZE) {
    upl_buf = dev->staging_buf;
    upl_mapped = dev->staging_mapped;
  } else {
    VkResult r = mop_vk_create_buffer(dev->device, &dev->mem_props, size,
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      &upl_buf, &upl_mem);
    if (r != VK_SUCCESS) {
      MOP_ERROR("[VK] image upload: transient buffer create failed: %d", r);
      return;
    }
    r = vkMapMemory(dev->device, upl_mem, 0, size, 0, &upl_mapped);
    if (r != VK_SUCCESS) {
      vkDestroyBuffer(dev->device, upl_buf, NULL);
      vkFreeMemory(dev->device, upl_mem, NULL);
      MOP_ERROR("[VK] image upload: map failed: %d", r);
      return;
    }
    transient = true;
  }

  memcpy(upl_mapped, rgba_data, size);

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
  vkCmdCopyBufferToImage(cb, upl_buf, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  /* Transition to SHADER_READ_ONLY */
  mop_vk_transition_image(
      cb, image, VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  staging_submit_and_wait(dev, cb);

  if (transient) {
    vkUnmapMemory(dev->device, upl_mem);
    vkDestroyBuffer(dev->device, upl_buf, NULL);
    vkFreeMemory(dev->device, upl_mem, NULL);
  }
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
    if (supported & VK_SAMPLE_COUNT_4_BIT)
      dev->msaa_samples = VK_SAMPLE_COUNT_4_BIT;
    else if (supported & VK_SAMPLE_COUNT_2_BIT)
      dev->msaa_samples = VK_SAMPLE_COUNT_2_BIT;
    else
      dev->msaa_samples = VK_SAMPLE_COUNT_1_BIT;
    /* MSAA: use detected sample count */
    MOP_INFO("[VK] MSAA: %dx", (int)dev->msaa_samples);
  }

  /* ---- Queue families (graphics + optional async compute) ---- */
  uint32_t qf_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(dev->physical_device, &qf_count,
                                           NULL);
  VkQueueFamilyProperties *qf_props = malloc(qf_count * sizeof(*qf_props));
  vkGetPhysicalDeviceQueueFamilyProperties(dev->physical_device, &qf_count,
                                           qf_props);

  dev->queue_family = UINT32_MAX;
  dev->compute_queue_family = UINT32_MAX;
  dev->has_async_compute = false;

  /* First pass: find graphics queue family */
  for (uint32_t i = 0; i < qf_count; i++) {
    if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      dev->queue_family = i;
      break;
    }
  }

  /* Second pass: find a dedicated compute queue family (compute but NOT
   * graphics).  This is ideal for async overlap on AMD/NVIDIA. */
  for (uint32_t i = 0; i < qf_count; i++) {
    if ((qf_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
        !(qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
      dev->compute_queue_family = i;
      break;
    }
  }

  /* Fallback: any compute-capable family that differs from graphics.
   * On MoltenVK/Apple this typically doesn't exist (single family). */
  if (dev->compute_queue_family == UINT32_MAX) {
    for (uint32_t i = 0; i < qf_count; i++) {
      if ((qf_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
          i != dev->queue_family) {
        dev->compute_queue_family = i;
        break;
      }
    }
  }

  /* Last resort: use a second queue from the graphics family (if available)
   * for async dispatch.  Many GPUs expose 2+ queues per family. */
  if (dev->compute_queue_family == UINT32_MAX &&
      dev->queue_family != UINT32_MAX &&
      qf_props[dev->queue_family].queueCount >= 2) {
    /* Same family, different queue index — still allows some overlap */
    dev->compute_queue_family = dev->queue_family;
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
  float priorities2[2] = {1.0f, 0.5f}; /* gfx=high, compute=medium */

  VkDeviceQueueCreateInfo queue_cis[2];
  uint32_t queue_ci_count = 1;
  queue_cis[0] = (VkDeviceQueueCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = dev->queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
  };

  VkPhysicalDeviceFeatures enabled_features = {0};
  if (dev->has_fill_mode_non_solid) {
    enabled_features.fillModeNonSolid = VK_TRUE;
  }

  /* Probe descriptor indexing features (Vulkan 1.2 core) */
  VkPhysicalDeviceDescriptorIndexingFeatures di_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
  };
  VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &di_features,
  };
  vkGetPhysicalDeviceFeatures2(dev->physical_device, &features2);

  dev->has_descriptor_indexing =
      di_features.descriptorBindingPartiallyBound &&
      di_features.descriptorBindingVariableDescriptorCount &&
      di_features.runtimeDescriptorArray &&
      di_features.shaderSampledImageArrayNonUniformIndexing;

  /* Enable descriptor indexing if supported */
  VkPhysicalDeviceDescriptorIndexingFeatures di_enable = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
  };
  if (dev->has_descriptor_indexing) {
    di_enable.descriptorBindingPartiallyBound = VK_TRUE;
    di_enable.descriptorBindingVariableDescriptorCount = VK_TRUE;
    di_enable.runtimeDescriptorArray = VK_TRUE;
    di_enable.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    di_enable.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    MOP_INFO("[VK] descriptor indexing enabled (bindless path available)");
  } else {
    MOP_INFO("[VK] descriptor indexing not available (using per-draw path)");
  }

  /* Device extensions for MoltenVK portability */
  const char *dev_exts[] = {
#if defined(__APPLE__)
      "VK_KHR_portability_subset",
#endif
  };
  uint32_t dev_ext_count = sizeof(dev_exts) / sizeof(dev_exts[0]);

  /* Populate compute queue create info if we found a separate family */
  if (dev->compute_queue_family != UINT32_MAX &&
      dev->compute_queue_family != dev->queue_family) {
    queue_cis[1] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = dev->compute_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    queue_ci_count = 2;
  } else if (dev->compute_queue_family == dev->queue_family) {
    /* Same family — request 2 queues from it */
    queue_cis[0].queueCount = 2;
    queue_cis[0].pQueuePriorities = priorities2;
  }

  VkDeviceCreateInfo dev_ci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = dev->has_descriptor_indexing ? &di_enable : NULL,
      .queueCreateInfoCount = queue_ci_count,
      .pQueueCreateInfos = queue_cis,
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

  /* ---- Async compute queue + resources ---- */
  dev->has_async_compute = false;
  if (dev->compute_queue_family != UINT32_MAX) {
    uint32_t cq_index =
        (dev->compute_queue_family == dev->queue_family) ? 1 : 0;
    vkGetDeviceQueue(dev->device, dev->compute_queue_family, cq_index,
                     &dev->compute_queue);

    /* Compute command pool */
    VkCommandPoolCreateInfo cpool_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = dev->compute_queue_family,
    };
    r = vkCreateCommandPool(dev->device, &cpool_ci, NULL,
                            &dev->compute_cmd_pool);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] async compute command pool creation failed: %d — "
               "disabling async compute",
               r);
      dev->compute_queue_family = UINT32_MAX;
      dev->compute_queue = VK_NULL_HANDLE;
      goto skip_async_compute;
    }

    /* Compute command buffer */
    VkCommandBufferAllocateInfo ccb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev->compute_cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    r = vkAllocateCommandBuffers(dev->device, &ccb_ai, &dev->compute_cmd_buf);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] async compute CB alloc failed: %d", r);
      vkDestroyCommandPool(dev->device, dev->compute_cmd_pool, NULL);
      dev->compute_cmd_pool = VK_NULL_HANDLE;
      dev->compute_queue_family = UINT32_MAX;
      dev->compute_queue = VK_NULL_HANDLE;
      goto skip_async_compute;
    }

    /* Compute fence (unsignaled — signaled when compute work completes) */
    VkFenceCreateInfo cfence_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    r = vkCreateFence(dev->device, &cfence_ci, NULL, &dev->compute_fence);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] async compute fence creation failed: %d", r);
      vkDestroyCommandPool(dev->device, dev->compute_cmd_pool, NULL);
      dev->compute_cmd_pool = VK_NULL_HANDLE;
      dev->compute_cmd_buf = VK_NULL_HANDLE;
      dev->compute_queue_family = UINT32_MAX;
      dev->compute_queue = VK_NULL_HANDLE;
      goto skip_async_compute;
    }

    /* Compute semaphore (signals graphics queue to wait) */
    VkSemaphoreCreateInfo csem_ci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    r = vkCreateSemaphore(dev->device, &csem_ci, NULL, &dev->compute_semaphore);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] async compute semaphore creation failed: %d", r);
      vkDestroyFence(dev->device, dev->compute_fence, NULL);
      vkDestroyCommandPool(dev->device, dev->compute_cmd_pool, NULL);
      dev->compute_cmd_pool = VK_NULL_HANDLE;
      dev->compute_cmd_buf = VK_NULL_HANDLE;
      dev->compute_fence = VK_NULL_HANDLE;
      dev->compute_queue_family = UINT32_MAX;
      dev->compute_queue = VK_NULL_HANDLE;
      goto skip_async_compute;
    }

    dev->has_async_compute = true;
    MOP_INFO("[VK] async compute enabled (family %u, queue index %u)",
             dev->compute_queue_family, cq_index);
  }
skip_async_compute:;

  /* ---- Command pool ---- */
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

  /* ---- Per-frame resources (ring-buffered CBs, fences, descriptor pools) ----
   */
  {
    VkCommandBufferAllocateInfo cb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MOP_VK_FRAMES_IN_FLIGHT,
    };
    VkCommandBuffer cbs[MOP_VK_FRAMES_IN_FLIGHT];
    r = vkAllocateCommandBuffers(dev->device, &cb_ai, cbs);
    if (r != VK_SUCCESS) {
      MOP_ERROR("[VK] vkAllocateCommandBuffers failed: %d", r);
      goto fail;
    }

    VkFenceCreateInfo fence_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    for (int i = 0; i < MOP_VK_FRAMES_IN_FLIGHT; i++) {
      dev->frames[i].cmd_buf = cbs[i];
      r = vkCreateFence(dev->device, &fence_ci, NULL, &dev->frames[i].fence);
      if (r != VK_SUCCESS) {
        MOP_ERROR("[VK] vkCreateFence (frame %d) failed: %d", i, r);
        goto fail;
      }
    }

    /* Set initial active-frame aliases */
    dev->frame_index = 0;
    dev->cmd_buf = dev->frames[0].cmd_buf;
    dev->fence = dev->frames[0].fence;
  }

  /* ---- Vulkan pipeline cache (Phase 3 — disk persistence) ---- */
  {
    VkPipelineCacheCreateInfo pc_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
    };

    /* Try to load cached data from disk */
    const char *home = getenv("HOME");
    if (home) {
      char cache_path[512];
      snprintf(cache_path, sizeof(cache_path),
               "%s/.cache/mop/pipeline_cache.bin", home);
      FILE *f = fopen(cache_path, "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) {
          void *data = malloc((size_t)size);
          if (data && fread(data, 1, (size_t)size, f) == (size_t)size) {
            pc_ci.initialDataSize = (size_t)size;
            pc_ci.pInitialData = data;
            MOP_INFO("[VK] loaded pipeline cache (%ld bytes)", size);
          }
          /* data freed after vkCreatePipelineCache */
          fclose(f);
          r = vkCreatePipelineCache(dev->device, &pc_ci, NULL,
                                    &dev->vk_pipeline_cache);
          free(data);
        } else {
          fclose(f);
          r = vkCreatePipelineCache(dev->device, &pc_ci, NULL,
                                    &dev->vk_pipeline_cache);
        }
      } else {
        r = vkCreatePipelineCache(dev->device, &pc_ci, NULL,
                                  &dev->vk_pipeline_cache);
      }
    } else {
      r = vkCreatePipelineCache(dev->device, &pc_ci, NULL,
                                &dev->vk_pipeline_cache);
    }
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] pipeline cache creation failed: %d", r);
      dev->vk_pipeline_cache = VK_NULL_HANDLE;
    }
  }

  /* ---- Memory suballocator (Phase 5) ---- */
  dev->suballocator = mop_suballoc_create(
      dev->device, &dev->mem_props,
      (VkDeviceSize)dev->dev_props.limits.nonCoherentAtomSize);
  if (dev->suballocator) {
    MOP_INFO("[VK] memory suballocator initialized");
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

  /* ---- Per-frame descriptor pools ---- */
  {
    /* Bindless path adds: 1024 combined-image-sampler (texture array) + 4
     * (shadow+IBL), storage buffers for bindless + compute cull, UBOs.
     * GPU culling (if enabled) adds: 4 storage buffers + 1 UBO per frame.
     * We reserve the extra capacity when descriptor_indexing is available,
     * since gpu_culling_enabled is set later in the init sequence. */
    uint32_t extra_samplers =
        dev->has_descriptor_indexing ? MOP_VK_MAX_BINDLESS_TEXTURES + 4 : 0;
    uint32_t extra_ssbos = dev->has_descriptor_indexing ? 4u : 0u;
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         MOP_VK_MAX_DRAWS_PER_FRAME + 2}, /* +2 for skybox + tonemap */
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         MOP_VK_MAX_DRAWS_PER_FRAME * 8 + 16 +
             extra_samplers}, /* legacy per-draw + bindless array */
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         MOP_VK_MAX_DRAWS_PER_FRAME + 4 +
             extra_ssbos}, /* legacy + bindless + cull SSBOs */
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         2 + (dev->has_descriptor_indexing ? 2u : 0u)}, /* SSAO+globals+cull */
    };
    VkDescriptorPoolCreateInfo dp_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = dev->has_descriptor_indexing
                     ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
                     : 0,
        .maxSets = MOP_VK_MAX_DRAWS_PER_FRAME + 16,
        .poolSizeCount = 4,
        .pPoolSizes = pool_sizes,
    };

    for (int i = 0; i < MOP_VK_FRAMES_IN_FLIGHT; i++) {
      r = vkCreateDescriptorPool(dev->device, &dp_ci, NULL,
                                 &dev->frames[i].desc_pool);
      if (r != VK_SUCCESS) {
        MOP_ERROR("[VK] desc pool (frame %d): %d", i, r);
        goto fail;
      }
    }
    dev->desc_pool = dev->frames[0].desc_pool;
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
    MOP_ERROR("[VK] vkCreateSampler (default) failed: %d", r);
    goto fail;
  }

  /* Clamp-to-edge sampler for screen-space post-processing */
  VkSamplerCreateInfo clamp_ci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 1.0f,
  };
  r = vkCreateSampler(dev->device, &clamp_ci, NULL, &dev->clamp_sampler);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkCreateSampler (clamp) failed: %d", r);
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

  /* Staging fence (tracks upload completion — replaces vkQueueWaitIdle) */
  {
    VkFenceCreateInfo sf_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    r = vkCreateFence(dev->device, &sf_ci, NULL, &dev->staging_fence);
    if (r != VK_SUCCESS) {
      MOP_ERROR("[VK] staging fence: %d", r);
      goto fail;
    }
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

  /* ---- Bindless resources (Phase 2A) ---- */
#if defined(MOP_VK_HAS_BINDLESS_SHADERS)
  if (dev->has_descriptor_indexing) {
    /* Bindless shader modules */
    dev->bindless_solid_vert =
        create_shader_module(dev->device, mop_solid_bindless_vert_spv,
                             mop_solid_bindless_vert_spv_size);
    dev->bindless_solid_frag =
        create_shader_module(dev->device, mop_solid_bindless_frag_spv,
                             mop_solid_bindless_frag_spv_size);
    dev->bindless_wireframe_frag =
        create_shader_module(dev->device, mop_wireframe_bindless_frag_spv,
                             mop_wireframe_bindless_frag_spv_size);

    if (!dev->bindless_solid_vert || !dev->bindless_solid_frag ||
        !dev->bindless_wireframe_frag) {
      MOP_WARN("[VK] bindless shader module creation failed, falling back");
      dev->has_descriptor_indexing = false;
    }
  }
#else
  dev->has_descriptor_indexing = false;
#endif

  if (dev->has_descriptor_indexing) {
    /* Bindless descriptor set layout */
    r = mop_vk_create_bindless_desc_layout(
        dev->device, MOP_VK_MAX_BINDLESS_TEXTURES, &dev->bindless_desc_layout);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] bindless desc layout failed: %d, falling back", r);
      dev->has_descriptor_indexing = false;
    }
  }

  if (dev->has_descriptor_indexing) {
    /* Bindless pipeline layout */
    r = mop_vk_create_bindless_pipeline_layout(
        dev->device, dev->bindless_desc_layout, &dev->bindless_pipeline_layout);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] bindless pipeline layout failed: %d, falling back", r);
      dev->has_descriptor_indexing = false;
    }
  }

  if (dev->has_descriptor_indexing) {
    /* Initialize texture registry: slot 0 = white, slot 1 = black */
    dev->texture_registry[0] = dev->white_view;
    dev->texture_registry[1] = dev->black_view;
    dev->texture_registry_count = 2;
    MOP_INFO("[VK] bindless texture registry initialized (2 fallback slots)");
  }

  /* ---- GPU culling compute pipeline (Phase 2B) ---- */
  dev->gpu_culling_enabled = false;
  dev->indirect_draw_enabled = false;
  dev->indirect_draw_frame_count = 0;
#if defined(MOP_VK_HAS_CULL_SHADER)
  if (dev->has_descriptor_indexing) {
    dev->cull_comp = create_shader_module(dev->device, mop_cull_comp_spv,
                                          mop_cull_comp_spv_size);
    if (dev->cull_comp) {
      r = mop_vk_create_cull_desc_layout(dev->device, &dev->cull_desc_layout);
      if (r == VK_SUCCESS) {
        r = mop_vk_create_cull_pipeline(dev);
        if (r == VK_SUCCESS) {
          dev->gpu_culling_enabled = true;
          MOP_INFO("[VK] GPU frustum culling pipeline created");
        } else {
          MOP_WARN("[VK] cull compute pipeline failed: %d", r);
        }
      } else {
        MOP_WARN("[VK] cull desc layout failed: %d", r);
      }
    } else {
      MOP_WARN("[VK] cull compute shader module creation failed");
    }
  }
#endif

  /* ---- Hi-Z occlusion culling pipeline (Phase 2C) ---- */
  dev->hiz_enabled = false;
#if defined(MOP_VK_HAS_HIZ_SHADER)
  if (dev->gpu_culling_enabled) {
    dev->hiz_downsample_comp =
        create_shader_module(dev->device, mop_hiz_downsample_comp_spv,
                             mop_hiz_downsample_comp_spv_size);
    if (dev->hiz_downsample_comp) {
      r = mop_vk_create_hiz_desc_layout(dev->device, &dev->hiz_desc_layout);
      if (r == VK_SUCCESS) {
        /* Create nearest-neighbor sampler for Hi-Z reads */
        VkSamplerCreateInfo samp_ci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        };
        r = vkCreateSampler(dev->device, &samp_ci, NULL, &dev->hiz_sampler);
        if (r == VK_SUCCESS) {
          r = mop_vk_create_hiz_pipeline(dev);
          if (r == VK_SUCCESS) {
            dev->hiz_enabled = true;
            MOP_INFO("[VK] Hi-Z occlusion culling pipeline created");
          } else {
            MOP_WARN("[VK] Hi-Z compute pipeline failed: %d", r);
          }
        }
      }
    }
  }
#endif

  /* ---- Mesh shading pipeline (Phase 10) ---- */
  dev->has_mesh_shader = false;
  dev->pfn_draw_mesh_tasks = NULL;
#if defined(MOP_VK_HAS_MESH_SHADERS)
  /* Only enable if the device actually supports VK_EXT_mesh_shader.
   * For now, create shader modules and pipeline unconditionally —
   * runtime detection of VK_EXT_mesh_shader support is done at
   * draw time via dev->has_mesh_shader. */
  {
    dev->meshlet_task = create_shader_module(dev->device, mop_meshlet_task_spv,
                                             mop_meshlet_task_spv_size);
    dev->meshlet_mesh = create_shader_module(dev->device, mop_meshlet_mesh_spv,
                                             mop_meshlet_mesh_spv_size);
    if (dev->meshlet_task && dev->meshlet_mesh) {
      r = mop_vk_create_meshlet_desc_layout(dev->device,
                                            &dev->meshlet_desc_layout);
      if (r == VK_SUCCESS) {
        r = mop_vk_create_meshlet_pipeline(dev);
        if (r == VK_SUCCESS) {
          /* Phase 8: cache vkCmdDrawMeshTasksEXT so we skip
           * vkGetDeviceProcAddr on every draw call. */
          dev->pfn_draw_mesh_tasks =
              vkGetDeviceProcAddr(dev->device, "vkCmdDrawMeshTasksEXT");
          if (dev->pfn_draw_mesh_tasks) {
            dev->has_mesh_shader = true;
            MOP_INFO("[VK] mesh shading active — "
                     "vkCmdDrawMeshTasksEXT loaded");
          } else {
            MOP_INFO("[VK] mesh shading pipeline created but "
                     "vkCmdDrawMeshTasksEXT unavailable at runtime");
          }
        } else {
          MOP_WARN("[VK] meshlet pipeline creation failed: %d", r);
        }
      } else {
        MOP_WARN("[VK] meshlet desc layout failed: %d", r);
      }
    } else {
      MOP_WARN("[VK] meshlet shader module creation failed");
    }
  }
#endif

  /* ---- RTX readiness (Phase 9 — capability probe only) ---- */
  dev->has_raytracing = false;
  dev->has_ray_query = false;
  dev->has_ray_pipeline = false;
  {
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(dev->physical_device, NULL, &ext_count,
                                         NULL);
    if (ext_count > 0) {
      VkExtensionProperties *exts = malloc(ext_count * sizeof(*exts));
      if (exts) {
        vkEnumerateDeviceExtensionProperties(dev->physical_device, NULL,
                                             &ext_count, exts);
        for (uint32_t i = 0; i < ext_count; i++) {
          if (strcmp(exts[i].extensionName, "VK_KHR_acceleration_structure") ==
              0)
            dev->has_raytracing = true;
          else if (strcmp(exts[i].extensionName, "VK_KHR_ray_query") == 0)
            dev->has_ray_query = true;
          else if (strcmp(exts[i].extensionName,
                          "VK_KHR_ray_tracing_pipeline") == 0)
            dev->has_ray_pipeline = true;
        }
        free(exts);
      }
    }
    if (dev->has_raytracing)
      MOP_INFO("[VK] RTX: acceleration_structure available");
    if (dev->has_ray_query)
      MOP_INFO("[VK] RTX: ray_query available");
    if (dev->has_ray_pipeline)
      MOP_INFO("[VK] RTX: ray_tracing_pipeline available");
    if (!dev->has_raytracing)
      MOP_INFO("[VK] RTX: not available on this device");
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

    /* Per-pass timestamp query pool (Phase 9A) */
    VkQueryPoolCreateInfo pass_qp_ci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = MOP_VK_MAX_PASS_TIMESTAMPS,
    };
    r = vkCreateQueryPool(dev->device, &pass_qp_ci, NULL,
                          &dev->pass_timestamp_pool);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] per-pass timestamp pool failed: %d", r);
      dev->pass_timestamp_pool = VK_NULL_HANDLE;
    }
    dev->pass_query_count = 0;
    dev->pass_timing_count = 0;
    dev->pass_timing_result_count = 0;
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
          /* Shadow pipeline + rendering pass fully implemented.
           * Shadow maps are rendered in frame_end (temporal: 1 frame behind).
           * shadows_enabled starts false; set true after first shadow render.
           */
          dev->shadows_enabled = false;
          dev->shadow_draws = NULL;
          dev->shadow_draw_count = 0;
          dev->shadow_draw_capacity = 0;
          dev->shadow_data_valid = false;
          MOP_INFO("[VK] shadow pipeline ready (%dx%d, %d cascades)",
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

  /* Transition shadow image to a valid layout so descriptor bindings don't
   * reference UNDEFINED-layout memory (causes corruption on MoltenVK). */
  if (dev->shadow_image && !dev->shadows_enabled) {
    VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);
    mop_vk_transition_image(cb, dev->shadow_image, VK_IMAGE_ASPECT_DEPTH_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 0,
                            VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    staging_submit_and_wait(dev, cb);
  }

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
    }
    VkResult tr2 = mop_vk_create_tonemap_render_pass_taa(
        dev->device, &dev->tonemap_render_pass_taa);
    if (tr2 != VK_SUCCESS) {
      MOP_WARN("[VK] tonemap render pass (TAA variant) failed: %d", tr2);
    }
    if (tr == VK_SUCCESS) {
      /* Descriptor set layout: HDR + bloom[0..4] + SSAO + SSR (8 samplers).
       * Multi-level bloom is combined directly in the tonemap shader to
       * avoid a separate upsample chain (TBDR issues on MoltenVK). */
      VkDescriptorSetLayoutBinding tm_bindings[8];
      for (int b = 0; b < 8; b++) {
        tm_bindings[b] = (VkDescriptorSetLayoutBinding){
            .binding = (uint32_t)b,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
      }
      VkDescriptorSetLayoutCreateInfo tm_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 8,
          .pBindings = tm_bindings,
      };
      tr = vkCreateDescriptorSetLayout(dev->device, &tm_layout_ci, NULL,
                                       &dev->tonemap_desc_layout);
      if (tr != VK_SUCCESS) {
        MOP_WARN("[VK] tonemap desc layout failed: %d", tr);
        goto tonemap_done;
      }

      /* Pipeline layout: desc set + push constants (exposure + bloom_intensity)
       */
      VkPushConstantRange tm_push = {
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          .offset = 0,
          .size = 8, /* float exposure + float bloom_intensity */
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

  /* ---- Bloom resources ---- */
  {
    VkResult br =
        mop_vk_create_bloom_render_pass(dev->device, &dev->bloom_render_pass);
    if (br != VK_SUCCESS) {
      MOP_WARN("[VK] bloom render pass failed: %d", br);
    } else {
      /* Descriptor set layout: single input sampler */
      VkDescriptorSetLayoutBinding bloom_binding = {
          .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      };
      VkDescriptorSetLayoutCreateInfo bloom_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 1,
          .pBindings = &bloom_binding,
      };
      br = vkCreateDescriptorSetLayout(dev->device, &bloom_layout_ci, NULL,
                                       &dev->bloom_desc_layout);
      if (br != VK_SUCCESS) {
        MOP_WARN("[VK] bloom desc layout failed: %d", br);
        goto bloom_done;
      }

      /* Pipeline layout: desc set + 16 bytes push constants (max of
       * extract {threshold, soft_knee} = 8 and blur {texel_size, dir} = 16) */
      VkPushConstantRange bloom_push = {
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          .offset = 0,
          .size = 16,
      };
      VkPipelineLayoutCreateInfo bloom_pl_ci = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = 1,
          .pSetLayouts = &dev->bloom_desc_layout,
          .pushConstantRangeCount = 1,
          .pPushConstantRanges = &bloom_push,
      };
      br = vkCreatePipelineLayout(dev->device, &bloom_pl_ci, NULL,
                                  &dev->bloom_pipeline_layout);
      if (br != VK_SUCCESS) {
        MOP_WARN("[VK] bloom pipeline layout failed: %d", br);
        goto bloom_done;
      }

#if defined(MOP_VK_HAS_BLOOM_SHADERS)
      dev->bloom_extract_frag =
          create_shader_module(dev->device, mop_bloom_extract_frag_spv,
                               mop_bloom_extract_frag_spv_size);
      dev->bloom_blur_frag = create_shader_module(
          dev->device, mop_bloom_blur_frag_spv, mop_bloom_blur_frag_spv_size);

      if (dev->fullscreen_vert && dev->bloom_extract_frag &&
          dev->bloom_blur_frag) {
        dev->bloom_extract_pipeline = mop_vk_create_bloom_extract_pipeline(dev);
        dev->bloom_blur_pipeline = mop_vk_create_bloom_blur_pipeline(dev);

        /* Two-texture upsample: descriptor layout with 2 samplers */
        VkDescriptorSetLayoutBinding up_bindings[2] = {
            {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
            {
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
        };
        VkDescriptorSetLayoutCreateInfo up_layout_ci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2,
            .pBindings = up_bindings,
        };
        vkCreateDescriptorSetLayout(dev->device, &up_layout_ci, NULL,
                                    &dev->bloom_upsample_desc_layout);

        VkPushConstantRange up_push = {
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = 16,
        };
        VkPipelineLayoutCreateInfo up_pl_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &dev->bloom_upsample_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &up_push,
        };
        vkCreatePipelineLayout(dev->device, &up_pl_ci, NULL,
                               &dev->bloom_upsample_pl_layout);

        dev->bloom_upsample_frag =
            create_shader_module(dev->device, mop_bloom_upsample_frag_spv,
                                 mop_bloom_upsample_frag_spv_size);
        if (dev->bloom_upsample_frag && dev->bloom_upsample_pl_layout)
          dev->bloom_upsample_pipeline =
              mop_vk_create_bloom_upsample_pipeline(dev);

        if (dev->bloom_extract_pipeline && dev->bloom_blur_pipeline) {
          dev->bloom_threshold = 1.0f;
          dev->bloom_intensity = 0.5f;
          MOP_INFO("[VK] bloom pipelines created");
        }
      }
#else
      MOP_INFO("[VK] bloom resources allocated (pipeline pending shader "
               "compilation)");
#endif
    }
  }
bloom_done:

  /* ---- SSAO resources ---- */
  {
    VkResult sr =
        mop_vk_create_ssao_render_pass(dev->device, &dev->ssao_render_pass);
    if (sr != VK_SUCCESS) {
      MOP_WARN("[VK] SSAO render pass failed: %d", sr);
    } else {
      /* Descriptor set layout: depth sampler + noise texture + kernel UBO */
      VkDescriptorSetLayoutBinding ssao_bindings[3] = {
          {
              .binding = 0,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          },
          {
              .binding = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          },
          {
              .binding = 2,
              .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          },
      };
      VkDescriptorSetLayoutCreateInfo ssao_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 3,
          .pBindings = ssao_bindings,
      };
      sr = vkCreateDescriptorSetLayout(dev->device, &ssao_layout_ci, NULL,
                                       &dev->ssao_desc_layout);
      if (sr != VK_SUCCESS) {
        MOP_WARN("[VK] SSAO desc layout failed: %d", sr);
        goto ssao_done;
      }

      /* Pipeline layout: desc set + push constants
       * SSAO push: mat4 projection(64) + radius(4) + bias(4) + kernel_size(4)
       *            + reverse_z(4) + noise_scale(8) + pad(8) = 96 bytes
       * SSAO blur push: texel_size(8) = 8 bytes
       * Max = 96 bytes */
      VkPushConstantRange ssao_push = {
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          .offset = 0,
          .size = 96,
      };
      VkPipelineLayoutCreateInfo ssao_pl_ci = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = 1,
          .pSetLayouts = &dev->ssao_desc_layout,
          .pushConstantRangeCount = 1,
          .pPushConstantRanges = &ssao_push,
      };
      sr = vkCreatePipelineLayout(dev->device, &ssao_pl_ci, NULL,
                                  &dev->ssao_pipeline_layout);
      if (sr != VK_SUCCESS) {
        MOP_WARN("[VK] SSAO pipeline layout failed: %d", sr);
        goto ssao_done;
      }

      /* Create 4x4 noise texture (RG16_SFLOAT — random rotation vectors) */
      {
        sr = mop_vk_create_image(
            dev->device, &dev->mem_props, 4, 4, VK_FORMAT_R16G16_SFLOAT,
            VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            &dev->ssao_noise_image, &dev->ssao_noise_memory);
        if (sr != VK_SUCCESS) {
          MOP_WARN("[VK] SSAO noise image failed: %d", sr);
          goto ssao_done;
        }
        sr = mop_vk_create_image_view(
            dev->device, dev->ssao_noise_image, VK_FORMAT_R16G16_SFLOAT,
            VK_IMAGE_ASPECT_COLOR_BIT, &dev->ssao_noise_view);
        if (sr != VK_SUCCESS) {
          MOP_WARN("[VK] SSAO noise view failed: %d", sr);
          goto ssao_done;
        }

        /* Generate random noise data and upload via staging buffer */
        uint16_t noise_data[4 * 4 * 2]; /* 4x4 texels, RG half-float */
        srand(42); /* deterministic seed for reproducible AO */
        for (int i = 0; i < 16; i++) {
          float rx = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
          float ry = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
          /* Convert to half-float (simple approximation) */
          /* IEEE 754 half: sign(1) exp(5) mant(10) */
          union {
            float f;
            uint32_t u;
          } fx = {.f = rx};
          union {
            float f;
            uint32_t u;
          } fy = {.f = ry};
          uint32_t sx = (fx.u >> 16) & 0x8000;
          int32_t ex = ((fx.u >> 23) & 0xFF) - 127 + 15;
          if (ex <= 0)
            ex = 0;
          if (ex > 30)
            ex = 30;
          uint32_t mx = (fx.u >> 13) & 0x3FF;
          noise_data[i * 2 + 0] = (uint16_t)(sx | ((uint32_t)ex << 10) | mx);

          uint32_t sy = (fy.u >> 16) & 0x8000;
          int32_t ey = ((fy.u >> 23) & 0xFF) - 127 + 15;
          if (ey <= 0)
            ey = 0;
          if (ey > 30)
            ey = 30;
          uint32_t my = (fy.u >> 13) & 0x3FF;
          noise_data[i * 2 + 1] = (uint16_t)(sy | ((uint32_t)ey << 10) | my);
        }

        /* Upload noise via staging buffer */
        size_t noise_size = sizeof(noise_data);
        memcpy(dev->staging_mapped, noise_data, noise_size);
        VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);
        mop_vk_transition_image(
            cb, dev->ssao_noise_image, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .layerCount = 1},
            .imageExtent = {4, 4, 1},
        };
        vkCmdCopyBufferToImage(cb, dev->staging_buf, dev->ssao_noise_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        mop_vk_transition_image(
            cb, dev->ssao_noise_image, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        staging_submit_and_wait(dev, cb);
      }

      /* Create kernel UBO (64 hemisphere sample positions) */
      {
        float kernel_data[64 * 4]; /* vec4 per sample */
        srand(42);
        for (int i = 0; i < 64; i++) {
          float x = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
          float y = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
          float z = (float)rand() / (float)RAND_MAX; /* hemisphere: z > 0 */
          float len = sqrtf(x * x + y * y + z * z);
          if (len > 0.0f) {
            x /= len;
            y /= len;
            z /= len;
          }

          /* Scale: distribute more samples closer to origin */
          float scale = (float)i / 64.0f;
          scale = 0.1f + scale * scale * 0.9f; /* lerp(0.1, 1.0, scale^2) */
          kernel_data[i * 4 + 0] = x * scale;
          kernel_data[i * 4 + 1] = y * scale;
          kernel_data[i * 4 + 2] = z * scale;
          kernel_data[i * 4 + 3] = 0.0f; /* padding */
        }

        sr = mop_vk_create_buffer(dev->device, &dev->mem_props,
                                  sizeof(kernel_data),
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &dev->ssao_kernel_ubo, &dev->ssao_kernel_mem);
        if (sr != VK_SUCCESS) {
          MOP_WARN("[VK] SSAO kernel UBO failed: %d", sr);
          goto ssao_done;
        }
        void *mapped;
        vkMapMemory(dev->device, dev->ssao_kernel_mem, 0, sizeof(kernel_data),
                    0, &mapped);
        memcpy(mapped, kernel_data, sizeof(kernel_data));
        vkUnmapMemory(dev->device, dev->ssao_kernel_mem);
      }

#if defined(MOP_VK_HAS_SSAO_SHADERS)
      dev->ssao_frag = create_shader_module(dev->device, mop_ssao_frag_spv,
                                            mop_ssao_frag_spv_size);
      dev->ssao_blur_frag = create_shader_module(
          dev->device, mop_ssao_blur_frag_spv, mop_ssao_blur_frag_spv_size);

      if (dev->fullscreen_vert && dev->ssao_frag && dev->ssao_blur_frag) {
        dev->ssao_pipeline = mop_vk_create_ssao_pipeline(dev);
        dev->ssao_blur_pipeline = mop_vk_create_ssao_blur_pipeline(dev);

        if (dev->ssao_pipeline && dev->ssao_blur_pipeline) {
          MOP_INFO("[VK] SSAO pipelines created");
        }
      }
#else
      MOP_INFO("[VK] SSAO resources allocated (pipeline pending shader "
               "compilation)");
#endif

      /* GTAO — upgraded AO, shares render pass + layout with SSAO */
      dev->gtao_available = false;
#if defined(MOP_VK_HAS_GTAO_SHADERS)
      dev->gtao_frag = create_shader_module(dev->device, mop_gtao_frag_spv,
                                            mop_gtao_frag_spv_size);
      dev->gtao_blur_frag = create_shader_module(
          dev->device, mop_gtao_blur_frag_spv, mop_gtao_blur_frag_spv_size);

      if (dev->fullscreen_vert && dev->gtao_frag && dev->gtao_blur_frag) {
        dev->gtao_pipeline = mop_vk_create_gtao_pipeline(dev);
        dev->gtao_blur_pipeline = mop_vk_create_gtao_blur_pipeline(dev);

        if (dev->gtao_pipeline && dev->gtao_blur_pipeline) {
          dev->gtao_available = true;
          MOP_INFO("[VK] GTAO pipelines created (upgraded AO)");
        }
      }
#endif
    }
  }
ssao_done:

  /* Merged SSAO+blur render pass (Phase 6 — pass merging) */
  {
    VkResult mr = mop_vk_create_ssao_merged_render_pass(
        dev->device, &dev->ssao_merged_render_pass);
    if (mr != VK_SUCCESS) {
      MOP_WARN("[VK] merged SSAO render pass failed: %d", mr);
      dev->ssao_merged_render_pass = VK_NULL_HANDLE;
    }
  }

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

  /* ---- GPU skinning compute pipeline (scaffolding) ----
   * Creates a compute pipeline + descriptor layout for mop_skin.comp.
   * Only activates if the shader was compiled into vulkan_shaders.h.
   * The dispatch helper is in vulkan_pipeline.c; actual wiring into
   * mop_skin_apply() is pending (see docs/TODO.md). */
#if defined(MOP_VK_HAS_SKIN_SHADER)
  {
    dev->skin_comp = create_shader_module(dev->device, mop_skin_comp_spv,
                                          mop_skin_comp_spv_size);
    if (dev->skin_comp) {
      /* 3 SSBO bindings: bind-pose, output, bone matrices */
      VkDescriptorSetLayoutBinding skin_b[3] = {
          {.binding = 0,
           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
           .descriptorCount = 1,
           .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
          {.binding = 1,
           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
           .descriptorCount = 1,
           .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
          {.binding = 2,
           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
           .descriptorCount = 1,
           .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
      };
      VkDescriptorSetLayoutCreateInfo dsl_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 3,
          .pBindings = skin_b,
      };
      vkCreateDescriptorSetLayout(dev->device, &dsl_ci, NULL,
                                  &dev->skin_desc_layout);

      VkPushConstantRange skin_pc = {
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
          .offset = 0,
          .size = 32, /* 8 × u32 */
      };
      VkPipelineLayoutCreateInfo pl_ci = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = 1,
          .pSetLayouts = &dev->skin_desc_layout,
          .pushConstantRangeCount = 1,
          .pPushConstantRanges = &skin_pc,
      };
      vkCreatePipelineLayout(dev->device, &pl_ci, NULL,
                             &dev->skin_pipeline_layout);

      if (dev->skin_desc_layout && dev->skin_pipeline_layout) {
        VkComputePipelineCreateInfo cp_ci = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType =
                          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = dev->skin_comp,
                      .pName = "main"},
            .layout = dev->skin_pipeline_layout,
        };
        VkResult sr =
            vkCreateComputePipelines(dev->device, dev->vk_pipeline_cache, 1,
                                     &cp_ci, NULL, &dev->skin_pipeline);
        if (sr == VK_SUCCESS) {
          dev->skin_enabled = true;
          MOP_INFO("[VK] skin compute pipeline created");
        } else {
          MOP_WARN("[VK] skin compute pipeline creation failed: %d", sr);
        }
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

  /* ---- SSR (Screen-Space Reflections) pipeline ---- */
  dev->ssr_enabled = false;
  dev->ssr_intensity = 0.5f;
  {
    VkResult sr =
        mop_vk_create_ssr_render_pass(dev->device, &dev->ssr_render_pass);
    if (sr != VK_SUCCESS) {
      MOP_WARN("[VK] SSR render pass failed: %d", sr);
    } else {
      /* Descriptor set layout: 2 samplers (depth, HDR color) */
      VkDescriptorSetLayoutBinding ssr_bindings[2];
      for (int b = 0; b < 2; b++) {
        ssr_bindings[b] = (VkDescriptorSetLayoutBinding){
            .binding = (uint32_t)b,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
      }
      VkDescriptorSetLayoutCreateInfo ssr_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 2,
          .pBindings = ssr_bindings,
      };
      sr = vkCreateDescriptorSetLayout(dev->device, &ssr_layout_ci, NULL,
                                       &dev->ssr_desc_layout);
      if (sr != VK_SUCCESS) {
        MOP_WARN("[VK] SSR desc layout failed: %d", sr);
      } else {
        /* Push constants: projection(64) + inv_projection(64) + inv_res(8)
         * + reverse_z(4) + max_distance(4) + thickness(4) + intensity(4)
         * + pad(4) = 156 bytes */
        VkPushConstantRange ssr_push = {
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = 156,
        };
        VkPipelineLayoutCreateInfo ssr_pl_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &dev->ssr_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &ssr_push,
        };
        sr = vkCreatePipelineLayout(dev->device, &ssr_pl_ci, NULL,
                                    &dev->ssr_pipeline_layout);
        if (sr != VK_SUCCESS) {
          MOP_WARN("[VK] SSR pipeline layout failed: %d", sr);
        } else {
#if defined(MOP_VK_HAS_SSR_SHADER)
          dev->ssr_frag = create_shader_module(dev->device, mop_ssr_frag_spv,
                                               mop_ssr_frag_spv_size);
          if (dev->fullscreen_vert && dev->ssr_frag) {
            dev->ssr_pipeline = mop_vk_create_ssr_pipeline(dev);
            if (dev->ssr_pipeline) {
              MOP_INFO("[VK] SSR pipeline enabled");
            }
          }
#else
          MOP_INFO("[VK] SSR resources allocated (pipeline pending shader "
                   "compilation)");
#endif
        }
      }
    }
  }

  /* ---- OIT (Order-Independent Transparency) pipelines ---- */
  dev->oit_enabled = false;
  dev->oit_draws = NULL;
  dev->oit_draw_count = 0;
  dev->oit_draw_capacity = 0;
  {
    VkResult oit_r = mop_vk_create_oit_render_pass(
        dev->device, VK_FORMAT_D32_SFLOAT, &dev->oit_render_pass);
    if (oit_r != VK_SUCCESS) {
      MOP_WARN("[VK] OIT render pass failed: %d", oit_r);
    } else {
      oit_r = mop_vk_create_oit_composite_render_pass(
          dev->device, &dev->oit_composite_render_pass);
      if (oit_r != VK_SUCCESS) {
        MOP_WARN("[VK] OIT composite render pass failed: %d", oit_r);
      } else {
        /* Composite descriptor set layout: 2 samplers (accum, revealage) */
        VkDescriptorSetLayoutBinding oit_bindings[2];
        for (int b = 0; b < 2; b++) {
          oit_bindings[b] = (VkDescriptorSetLayoutBinding){
              .binding = (uint32_t)b,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          };
        }
        VkDescriptorSetLayoutCreateInfo oit_layout_ci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2,
            .pBindings = oit_bindings,
        };
        oit_r = vkCreateDescriptorSetLayout(dev->device, &oit_layout_ci, NULL,
                                            &dev->oit_composite_desc_layout);
        if (oit_r != VK_SUCCESS) {
          MOP_WARN("[VK] OIT composite desc layout failed: %d", oit_r);
        } else {
          VkPipelineLayoutCreateInfo oit_pl_ci = {
              .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
              .setLayoutCount = 1,
              .pSetLayouts = &dev->oit_composite_desc_layout,
          };
          oit_r = vkCreatePipelineLayout(dev->device, &oit_pl_ci, NULL,
                                         &dev->oit_composite_pipeline_layout);
          if (oit_r != VK_SUCCESS) {
            MOP_WARN("[VK] OIT composite pipeline layout failed: %d", oit_r);
          } else {
#if defined(MOP_VK_HAS_OIT_SHADERS)
            dev->oit_accum_frag =
                create_shader_module(dev->device, mop_oit_accum_frag_spv,
                                     mop_oit_accum_frag_spv_size);
            dev->oit_composite_frag =
                create_shader_module(dev->device, mop_oit_composite_frag_spv,
                                     mop_oit_composite_frag_spv_size);
            if (dev->bindless_solid_vert && dev->oit_accum_frag) {
              dev->oit_pipeline = mop_vk_create_oit_pipeline(dev);
            }
            if (dev->fullscreen_vert && dev->oit_composite_frag) {
              dev->oit_composite_pipeline =
                  mop_vk_create_oit_composite_pipeline(dev);
            }
            if (dev->oit_pipeline && dev->oit_composite_pipeline) {
              MOP_INFO("[VK] OIT pipelines enabled");
            }
#else
            MOP_INFO("[VK] OIT resources allocated (pipelines pending shader "
                     "compilation)");
#endif
          }
        }
      }
    }
  }

  /* ---- Deferred decal pipeline ---- */
  dev->decal_count = 0;
  {
    VkResult dr =
        mop_vk_create_decal_render_pass(dev->device, &dev->decal_render_pass);
    if (dr != VK_SUCCESS) {
      MOP_WARN("[VK] Decal render pass failed: %d", dr);
    } else {
      /* Descriptor layout: binding 0 = depth copy (sampler2D),
       * binding 1 = decal texture (sampler2D), binding 2 = UBO */
      VkDescriptorSetLayoutBinding decal_bindings[3] = {
          {
              .binding = 0,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          },
          {
              .binding = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          },
          {
              .binding = 2,
              .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          },
      };
      VkDescriptorSetLayoutCreateInfo decal_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 3,
          .pBindings = decal_bindings,
      };
      dr = vkCreateDescriptorSetLayout(dev->device, &decal_layout_ci, NULL,
                                       &dev->decal_desc_layout);
      if (dr != VK_SUCCESS) {
        MOP_WARN("[VK] Decal desc layout failed: %d", dr);
      } else {
        /* Pipeline layout: 1 desc set + 128 byte push constant (vert+frag) */
        VkPushConstantRange decal_pc = {
            .stageFlags =
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = 128, /* mat4 mvp + mat4 inv_decal */
        };
        VkPipelineLayoutCreateInfo decal_pl_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &dev->decal_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &decal_pc,
        };
        dr = vkCreatePipelineLayout(dev->device, &decal_pl_ci, NULL,
                                    &dev->decal_pipeline_layout);
        if (dr != VK_SUCCESS) {
          MOP_WARN("[VK] Decal pipeline layout failed: %d", dr);
        } else {
#if defined(MOP_VK_HAS_DECAL_SHADERS)
          dev->decal_vert = create_shader_module(
              dev->device, mop_decal_vert_spv, mop_decal_vert_spv_size);
          dev->decal_frag = create_shader_module(
              dev->device, mop_decal_frag_spv, mop_decal_frag_spv_size);
          if (dev->decal_vert && dev->decal_frag) {
            dev->decal_pipeline = mop_vk_create_decal_pipeline(dev);
            if (dev->decal_pipeline) {
              MOP_INFO("[VK] Decal pipeline enabled");
            }
          }
#else
          MOP_INFO("[VK] Decal resources allocated (pipelines pending shader "
                   "compilation)");
#endif
        }
      }
    }
  }

  /* ---- Volumetric fog pipeline ---- */
  dev->volumetric_enabled = false;
  dev->volumetric_density = 0.02f;
  dev->volumetric_color[0] = 1.0f;
  dev->volumetric_color[1] = 1.0f;
  dev->volumetric_color[2] = 1.0f;
  dev->volumetric_anisotropy = 0.3f;
  dev->volumetric_steps = 32;
  {
    VkResult vr = mop_vk_create_volumetric_render_pass(
        dev->device, &dev->volumetric_render_pass);
    if (vr == VK_SUCCESS) {
      /* Descriptor layout: depth(0) + light SSBO(1) + UBO(2) */
      VkDescriptorSetLayoutBinding vol_bindings[3] = {
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
          {
              .binding = 2,
              .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          },
      };
      VkDescriptorSetLayoutCreateInfo vol_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 3,
          .pBindings = vol_bindings,
      };
      vr = vkCreateDescriptorSetLayout(dev->device, &vol_layout_ci, NULL,
                                       &dev->volumetric_desc_layout);
      if (vr == VK_SUCCESS) {
        VkPipelineLayoutCreateInfo vol_pl_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &dev->volumetric_desc_layout,
        };
        vr = vkCreatePipelineLayout(dev->device, &vol_pl_ci, NULL,
                                    &dev->volumetric_pipeline_layout);
        if (vr == VK_SUCCESS) {
#if defined(MOP_VK_HAS_VOLUMETRIC_SHADER)
          dev->volumetric_frag =
              create_shader_module(dev->device, mop_volumetric_frag_spv,
                                   mop_volumetric_frag_spv_size);
          if (dev->volumetric_frag && dev->fullscreen_vert) {
            dev->volumetric_pipeline = mop_vk_create_volumetric_pipeline(dev);
            if (dev->volumetric_pipeline) {
              MOP_INFO("[VK] Volumetric fog pipeline enabled");
            }
          }
#else
          MOP_INFO("[VK] Volumetric resources allocated (shader pending)");
#endif
        }
      }
    }
  }

  /* ---- TAA resolve pipeline ---- */
  dev->taa_enabled = false;
  {
    VkResult tr =
        mop_vk_create_taa_render_pass(dev->device, &dev->taa_render_pass);
    if (tr != VK_SUCCESS) {
      MOP_WARN("[VK] TAA render pass failed: %d", tr);
    } else {
      /* Descriptor set layout: 3 samplers (current, history, depth) */
      VkDescriptorSetLayoutBinding taa_bindings[3];
      for (int b = 0; b < 3; b++) {
        taa_bindings[b] = (VkDescriptorSetLayoutBinding){
            .binding = (uint32_t)b,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
      }
      VkDescriptorSetLayoutCreateInfo taa_layout_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = 3,
          .pBindings = taa_bindings,
      };
      tr = vkCreateDescriptorSetLayout(dev->device, &taa_layout_ci, NULL,
                                       &dev->taa_desc_layout);
      if (tr != VK_SUCCESS) {
        MOP_WARN("[VK] TAA desc layout failed: %d", tr);
      } else {
        /* Push constants: inv_vp_jittered(64) + prev_vp(64) + inv_res(8) +
         * jitter(8) + feedback(4) + first_frame(4) = 152 bytes */
        VkPushConstantRange taa_push = {
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = 152,
        };
        VkPipelineLayoutCreateInfo taa_pl_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &dev->taa_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &taa_push,
        };
        tr = vkCreatePipelineLayout(dev->device, &taa_pl_ci, NULL,
                                    &dev->taa_pipeline_layout);
        if (tr != VK_SUCCESS) {
          MOP_WARN("[VK] TAA pipeline layout failed: %d", tr);
        } else {
#if defined(MOP_VK_HAS_TAA_SHADER)
          dev->taa_frag = create_shader_module(dev->device, mop_taa_frag_spv,
                                               mop_taa_frag_spv_size);
          if (dev->fullscreen_vert && dev->taa_frag) {
            dev->taa_pipeline = mop_vk_create_taa_pipeline(dev);
            if (dev->taa_pipeline) {
              /* TAA pipeline ready but not activated until vk_set_taa() is
               * called with jitter data.  Enabling it here would make the
               * readback path use uninitialized TAA history buffers. */
              dev->taa_enabled = false;
              MOP_INFO("[VK] TAA resolve pipeline enabled");
            }
          }
#else
          MOP_INFO("[VK] TAA resources allocated (pipeline pending shader "
                   "compilation)");
#endif
        }
      }
    }
  }

  /* -----------------------------------------------------------------------
   * Per-thread Vulkan resources for multi-threaded command recording.
   * Each worker thread gets its own command pool and descriptor pool.
   * Non-fatal: falls back to single-threaded if creation fails.
   * ----------------------------------------------------------------------- */
  {
    int num_threads = 4; /* default */
#if defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 1)
      num_threads =
          (int)(n > MOP_VK_MAX_WORKER_THREADS ? MOP_VK_MAX_WORKER_THREADS : n);
#endif
    dev->thread_count = 0;
    for (int t = 0; t < num_threads; t++) {
      VkCommandPoolCreateInfo tpool_ci = {
          .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
          .queueFamilyIndex = dev->queue_family,
      };
      VkResult r = vkCreateCommandPool(dev->device, &tpool_ci, NULL,
                                       &dev->thread_states[t].cmd_pool);
      if (r != VK_SUCCESS)
        break;

      VkCommandBufferAllocateInfo tcb_ai = {
          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
          .commandPool = dev->thread_states[t].cmd_pool,
          .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
          .commandBufferCount = 1,
      };
      r = vkAllocateCommandBuffers(dev->device, &tcb_ai,
                                   &dev->thread_states[t].secondary_cb);
      if (r != VK_SUCCESS) {
        vkDestroyCommandPool(dev->device, dev->thread_states[t].cmd_pool, NULL);
        dev->thread_states[t].cmd_pool = VK_NULL_HANDLE;
        break;
      }

      VkDescriptorPoolSize tpool_sizes[] = {
          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32},
          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16},
      };
      VkDescriptorPoolCreateInfo tdp_ci = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
          .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
          .maxSets = 64,
          .poolSizeCount = 4,
          .pPoolSizes = tpool_sizes,
      };
      r = vkCreateDescriptorPool(dev->device, &tdp_ci, NULL,
                                 &dev->thread_states[t].desc_pool);
      if (r != VK_SUCCESS) {
        vkDestroyCommandPool(dev->device, dev->thread_states[t].cmd_pool, NULL);
        dev->thread_states[t].cmd_pool = VK_NULL_HANDLE;
        dev->thread_states[t].secondary_cb = VK_NULL_HANDLE;
        break;
      }

      dev->thread_states[t].cb_recording = false;
      dev->thread_count++;
    }
    MOP_INFO("[VK] %u worker thread states created for MT command recording",
             dev->thread_count);
  }

  MOP_INFO("[VK] device created successfully");
  return dev;

fail:
  /* Partial cleanup — destroy what was created */
  {
    VkDevice d = dev->device;
    if (d) {
      vkDeviceWaitIdle(d);

      if (dev->staging_fence)
        vkDestroyFence(d, dev->staging_fence, NULL);
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

      if (dev->clamp_sampler)
        vkDestroySampler(d, dev->clamp_sampler, NULL);
      if (dev->default_sampler)
        vkDestroySampler(d, dev->default_sampler, NULL);

      for (int i = 0; i < MOP_VK_FRAMES_IN_FLIGHT; i++) {
        if (dev->frames[i].desc_pool)
          vkDestroyDescriptorPool(d, dev->frames[i].desc_pool, NULL);
      }

      if (dev->pipeline_layout)
        vkDestroyPipelineLayout(d, dev->pipeline_layout, NULL);
      if (dev->desc_set_layout)
        vkDestroyDescriptorSetLayout(d, dev->desc_set_layout, NULL);
      if (dev->render_pass)
        vkDestroyRenderPass(d, dev->render_pass, NULL);

      if (dev->timestamp_pool)
        vkDestroyQueryPool(d, dev->timestamp_pool, NULL);
      if (dev->pass_timestamp_pool)
        vkDestroyQueryPool(d, dev->pass_timestamp_pool, NULL);

      if (dev->solid_vert)
        vkDestroyShaderModule(d, dev->solid_vert, NULL);
      if (dev->instanced_vert)
        vkDestroyShaderModule(d, dev->instanced_vert, NULL);
      if (dev->solid_frag)
        vkDestroyShaderModule(d, dev->solid_frag, NULL);
      if (dev->wireframe_frag)
        vkDestroyShaderModule(d, dev->wireframe_frag, NULL);

      for (int i = 0; i < MOP_VK_FRAMES_IN_FLIGHT; i++) {
        if (dev->frames[i].fence)
          vkDestroyFence(d, dev->frames[i].fence, NULL);
      }
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
    free(dev->shadow_draws);

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
    if (dev->tonemap_render_pass_taa)
      vkDestroyRenderPass(d, dev->tonemap_render_pass_taa, NULL);
    if (dev->tonemap_frag)
      vkDestroyShaderModule(d, dev->tonemap_frag, NULL);

    /* Bloom cleanup */
    if (dev->bloom_extract_pipeline)
      vkDestroyPipeline(d, dev->bloom_extract_pipeline, NULL);
    if (dev->bloom_blur_pipeline)
      vkDestroyPipeline(d, dev->bloom_blur_pipeline, NULL);
    if (dev->bloom_upsample_pipeline)
      vkDestroyPipeline(d, dev->bloom_upsample_pipeline, NULL);
    if (dev->bloom_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->bloom_pipeline_layout, NULL);
    if (dev->bloom_upsample_pl_layout)
      vkDestroyPipelineLayout(d, dev->bloom_upsample_pl_layout, NULL);
    if (dev->bloom_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->bloom_desc_layout, NULL);
    if (dev->bloom_upsample_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->bloom_upsample_desc_layout, NULL);
    if (dev->bloom_render_pass)
      vkDestroyRenderPass(d, dev->bloom_render_pass, NULL);
    if (dev->bloom_extract_frag)
      vkDestroyShaderModule(d, dev->bloom_extract_frag, NULL);
    if (dev->bloom_blur_frag)
      vkDestroyShaderModule(d, dev->bloom_blur_frag, NULL);
    if (dev->bloom_upsample_frag)
      vkDestroyShaderModule(d, dev->bloom_upsample_frag, NULL);

    /* SSAO cleanup */
    if (dev->ssao_pipeline)
      vkDestroyPipeline(d, dev->ssao_pipeline, NULL);
    if (dev->ssao_blur_pipeline)
      vkDestroyPipeline(d, dev->ssao_blur_pipeline, NULL);
    if (dev->ssao_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->ssao_pipeline_layout, NULL);
    if (dev->ssao_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->ssao_desc_layout, NULL);
    if (dev->ssao_render_pass)
      vkDestroyRenderPass(d, dev->ssao_render_pass, NULL);
    if (dev->ssao_merged_render_pass)
      vkDestroyRenderPass(d, dev->ssao_merged_render_pass, NULL);
    if (dev->ssao_frag)
      vkDestroyShaderModule(d, dev->ssao_frag, NULL);
    if (dev->ssao_blur_frag)
      vkDestroyShaderModule(d, dev->ssao_blur_frag, NULL);
    if (dev->ssao_noise_view)
      vkDestroyImageView(d, dev->ssao_noise_view, NULL);
    if (dev->ssao_noise_image)
      vkDestroyImage(d, dev->ssao_noise_image, NULL);
    if (dev->ssao_noise_memory)
      vkFreeMemory(d, dev->ssao_noise_memory, NULL);
    if (dev->ssao_kernel_ubo)
      vkDestroyBuffer(d, dev->ssao_kernel_ubo, NULL);
    if (dev->ssao_kernel_mem)
      vkFreeMemory(d, dev->ssao_kernel_mem, NULL);

    /* GTAO cleanup */
    if (dev->gtao_pipeline)
      vkDestroyPipeline(d, dev->gtao_pipeline, NULL);
    if (dev->gtao_blur_pipeline)
      vkDestroyPipeline(d, dev->gtao_blur_pipeline, NULL);
    if (dev->gtao_frag)
      vkDestroyShaderModule(d, dev->gtao_frag, NULL);
    if (dev->gtao_blur_frag)
      vkDestroyShaderModule(d, dev->gtao_blur_frag, NULL);

    /* Skybox cleanup */
    if (dev->skybox_pipeline)
      vkDestroyPipeline(d, dev->skybox_pipeline, NULL);
    if (dev->skybox_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->skybox_pipeline_layout, NULL);
    if (dev->skybox_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->skybox_desc_layout, NULL);
    if (dev->skybox_frag)
      vkDestroyShaderModule(d, dev->skybox_frag, NULL);

    /* Skin compute cleanup */
    if (dev->skin_pipeline)
      vkDestroyPipeline(d, dev->skin_pipeline, NULL);
    if (dev->skin_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->skin_pipeline_layout, NULL);
    if (dev->skin_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->skin_desc_layout, NULL);
    if (dev->skin_comp)
      vkDestroyShaderModule(d, dev->skin_comp, NULL);

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

    /* Bindless cleanup (Phase 2A) */
    if (dev->bindless_solid_vert)
      vkDestroyShaderModule(d, dev->bindless_solid_vert, NULL);
    if (dev->bindless_solid_frag)
      vkDestroyShaderModule(d, dev->bindless_solid_frag, NULL);
    if (dev->bindless_wireframe_frag)
      vkDestroyShaderModule(d, dev->bindless_wireframe_frag, NULL);
    if (dev->bindless_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->bindless_pipeline_layout, NULL);
    if (dev->bindless_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->bindless_desc_layout, NULL);

    /* GPU culling cleanup (Phase 2B) */
    if (dev->cull_pipeline)
      vkDestroyPipeline(d, dev->cull_pipeline, NULL);
    if (dev->cull_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->cull_pipeline_layout, NULL);
    if (dev->cull_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->cull_desc_layout, NULL);
    if (dev->cull_comp)
      vkDestroyShaderModule(d, dev->cull_comp, NULL);

    /* Mesh shading cleanup (Phase 10) */
    if (dev->meshlet_pipeline)
      vkDestroyPipeline(d, dev->meshlet_pipeline, NULL);
    if (dev->meshlet_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->meshlet_pipeline_layout, NULL);
    if (dev->meshlet_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->meshlet_desc_layout, NULL);
    if (dev->meshlet_task)
      vkDestroyShaderModule(d, dev->meshlet_task, NULL);
    if (dev->meshlet_mesh)
      vkDestroyShaderModule(d, dev->meshlet_mesh, NULL);
    if (dev->meshlet_ssbo)
      vkDestroyBuffer(d, dev->meshlet_ssbo, NULL);
    if (dev->meshlet_ssbo_mem)
      vkFreeMemory(d, dev->meshlet_ssbo_mem, NULL);
    if (dev->meshlet_cone_ssbo)
      vkDestroyBuffer(d, dev->meshlet_cone_ssbo, NULL);
    if (dev->meshlet_cone_ssbo_mem)
      vkFreeMemory(d, dev->meshlet_cone_ssbo_mem, NULL);
    if (dev->meshlet_vert_idx_ssbo)
      vkDestroyBuffer(d, dev->meshlet_vert_idx_ssbo, NULL);
    if (dev->meshlet_vert_idx_ssbo_mem)
      vkFreeMemory(d, dev->meshlet_vert_idx_ssbo_mem, NULL);
    if (dev->meshlet_prim_idx_ssbo)
      vkDestroyBuffer(d, dev->meshlet_prim_idx_ssbo, NULL);
    if (dev->meshlet_prim_idx_ssbo_mem)
      vkFreeMemory(d, dev->meshlet_prim_idx_ssbo_mem, NULL);

    /* Hi-Z occlusion culling cleanup (Phase 2C) */
    if (dev->hiz_pipeline)
      vkDestroyPipeline(d, dev->hiz_pipeline, NULL);
    if (dev->hiz_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->hiz_pipeline_layout, NULL);
    if (dev->hiz_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->hiz_desc_layout, NULL);
    if (dev->hiz_sampler)
      vkDestroySampler(d, dev->hiz_sampler, NULL);
    if (dev->hiz_downsample_comp)
      vkDestroyShaderModule(d, dev->hiz_downsample_comp, NULL);

    /* SSR cleanup */
    if (dev->ssr_pipeline)
      vkDestroyPipeline(d, dev->ssr_pipeline, NULL);
    if (dev->ssr_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->ssr_pipeline_layout, NULL);
    if (dev->ssr_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->ssr_desc_layout, NULL);
    if (dev->ssr_frag)
      vkDestroyShaderModule(d, dev->ssr_frag, NULL);
    if (dev->ssr_render_pass)
      vkDestroyRenderPass(d, dev->ssr_render_pass, NULL);

    /* OIT cleanup */
    if (dev->oit_pipeline)
      vkDestroyPipeline(d, dev->oit_pipeline, NULL);
    if (dev->oit_accum_frag)
      vkDestroyShaderModule(d, dev->oit_accum_frag, NULL);
    if (dev->oit_render_pass)
      vkDestroyRenderPass(d, dev->oit_render_pass, NULL);
    if (dev->oit_composite_pipeline)
      vkDestroyPipeline(d, dev->oit_composite_pipeline, NULL);
    if (dev->oit_composite_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->oit_composite_pipeline_layout, NULL);
    if (dev->oit_composite_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->oit_composite_desc_layout, NULL);
    if (dev->oit_composite_frag)
      vkDestroyShaderModule(d, dev->oit_composite_frag, NULL);
    if (dev->oit_composite_render_pass)
      vkDestroyRenderPass(d, dev->oit_composite_render_pass, NULL);
    free(dev->oit_draws);

    /* Decal cleanup */
    if (dev->decal_pipeline)
      vkDestroyPipeline(d, dev->decal_pipeline, NULL);
    if (dev->decal_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->decal_pipeline_layout, NULL);
    if (dev->decal_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->decal_desc_layout, NULL);
    if (dev->decal_vert)
      vkDestroyShaderModule(d, dev->decal_vert, NULL);
    if (dev->decal_frag)
      vkDestroyShaderModule(d, dev->decal_frag, NULL);
    if (dev->decal_render_pass)
      vkDestroyRenderPass(d, dev->decal_render_pass, NULL);

    /* Volumetric fog cleanup */
    if (dev->volumetric_pipeline)
      vkDestroyPipeline(d, dev->volumetric_pipeline, NULL);
    if (dev->volumetric_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->volumetric_pipeline_layout, NULL);
    if (dev->volumetric_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->volumetric_desc_layout, NULL);
    if (dev->volumetric_frag)
      vkDestroyShaderModule(d, dev->volumetric_frag, NULL);
    if (dev->volumetric_render_pass)
      vkDestroyRenderPass(d, dev->volumetric_render_pass, NULL);

    /* TAA cleanup */
    if (dev->taa_pipeline)
      vkDestroyPipeline(d, dev->taa_pipeline, NULL);
    if (dev->taa_pipeline_layout)
      vkDestroyPipelineLayout(d, dev->taa_pipeline_layout, NULL);
    if (dev->taa_desc_layout)
      vkDestroyDescriptorSetLayout(d, dev->taa_desc_layout, NULL);
    if (dev->taa_frag)
      vkDestroyShaderModule(d, dev->taa_frag, NULL);
    if (dev->taa_render_pass)
      vkDestroyRenderPass(d, dev->taa_render_pass, NULL);

    /* Pipeline cache (hash map) — includes both bindless and non-bindless */
    for (int i = 0; i < MOP_VK_PIPELINE_CACHE_CAPACITY; i++) {
      if (dev->pipeline_cache[i].pipeline)
        vkDestroyPipeline(d, dev->pipeline_cache[i].pipeline, NULL);
    }
    if (dev->instanced_pipeline)
      vkDestroyPipeline(d, dev->instanced_pipeline, NULL);

    /* Timestamp queries */
    if (dev->timestamp_pool)
      vkDestroyQueryPool(d, dev->timestamp_pool, NULL);
    if (dev->pass_timestamp_pool)
      vkDestroyQueryPool(d, dev->pass_timestamp_pool, NULL);

    /* Staging */
    if (dev->staging_fence)
      vkDestroyFence(d, dev->staging_fence, NULL);
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

    /* Per-frame descriptor pools */
    for (int i = 0; i < MOP_VK_FRAMES_IN_FLIGHT; i++) {
      if (dev->frames[i].desc_pool)
        vkDestroyDescriptorPool(d, dev->frames[i].desc_pool, NULL);
    }

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

    /* Async compute cleanup (Phase 1C) */
    if (dev->compute_semaphore)
      vkDestroySemaphore(d, dev->compute_semaphore, NULL);
    if (dev->compute_fence)
      vkDestroyFence(d, dev->compute_fence, NULL);
    if (dev->compute_cmd_pool)
      vkDestroyCommandPool(d, dev->compute_cmd_pool, NULL);

    /* Per-thread resources (secondary CB pools + descriptor pools) */
    for (uint32_t t = 0; t < dev->thread_count; t++) {
      if (dev->thread_states[t].desc_pool)
        vkDestroyDescriptorPool(d, dev->thread_states[t].desc_pool, NULL);
      if (dev->thread_states[t].cmd_pool)
        vkDestroyCommandPool(d, dev->thread_states[t].cmd_pool, NULL);
    }

    /* Per-frame fences (CBs freed by vkDestroyCommandPool) */
    for (int i = 0; i < MOP_VK_FRAMES_IN_FLIGHT; i++) {
      if (dev->frames[i].fence)
        vkDestroyFence(d, dev->frames[i].fence, NULL);
    }
    if (dev->cmd_pool)
      vkDestroyCommandPool(d, dev->cmd_pool, NULL);

    /* Save Vulkan pipeline cache to disk (Phase 3) */
    if (dev->vk_pipeline_cache) {
      size_t cache_size = 0;
      VkResult cr =
          vkGetPipelineCacheData(d, dev->vk_pipeline_cache, &cache_size, NULL);
      if (cr == VK_SUCCESS && cache_size > 0) {
        void *cache_data = malloc(cache_size);
        if (cache_data) {
          cr = vkGetPipelineCacheData(d, dev->vk_pipeline_cache, &cache_size,
                                      cache_data);
          if (cr == VK_SUCCESS) {
            const char *home = getenv("HOME");
            if (home) {
              char cache_dir[512], cache_path[512];
              snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/mop", home);
              snprintf(cache_path, sizeof(cache_path), "%s/pipeline_cache.bin",
                       cache_dir);
              /* Create directory (ignore EEXIST) */
              char parent_dir[512];
              snprintf(parent_dir, sizeof(parent_dir), "%s/.cache", home);
              mkdir(parent_dir, 0755);
              mkdir(cache_dir, 0755);
              FILE *f = fopen(cache_path, "wb");
              if (f) {
                fwrite(cache_data, 1, cache_size, f);
                fclose(f);
                MOP_INFO("[VK] saved pipeline cache (%zu bytes)", cache_size);
              }
            }
          }
          free(cache_data);
        }
      }
      vkDestroyPipelineCache(d, dev->vk_pipeline_cache, NULL);
    }

    /* Memory suballocator cleanup (Phase 5) */
    mop_suballoc_destroy(dev->suballocator);

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

  staging_submit_and_wait(dev, cb);
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

  /* R32_SFLOAT copy of depth for overlay pass sampling.
   * MoltenVK can't reliably sample D32_SFLOAT across CB boundaries. */
  r = mop_vk_create_image(
      dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
      VK_FORMAT_R32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      &fb->depth_copy_image, &fb->depth_copy_memory);
  if (r != VK_SUCCESS) {
    MOP_WARN("[VK] fb depth copy image: %d", r);
  } else {
    r = mop_vk_create_image_view(
        dev->device, fb->depth_copy_image, VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_ASPECT_COLOR_BIT, &fb->depth_copy_view);
    if (r != VK_SUCCESS)
      MOP_WARN("[VK] fb depth copy view: %d", r);
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

  /* ---- Bloom mip chain (half-res, R16G16B16A16_SFLOAT) ---- */
  if (dev->bloom_render_pass) {
    uint32_t bw = (uint32_t)width / 2;
    uint32_t bh = (uint32_t)height / 2;
    for (int i = 0; i < MOP_VK_BLOOM_LEVELS; i++) {
      if (bw < 1)
        bw = 1;
      if (bh < 1)
        bh = 1;
      r = mop_vk_create_image(
          dev->device, &dev->mem_props, bw, bh, VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_SAMPLE_COUNT_1_BIT,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          &fb->bloom_images[i], &fb->bloom_memory[i]);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] bloom image level %d failed: %d", i, r);
        break;
      }
      r = mop_vk_create_image_view(
          dev->device, fb->bloom_images[i], VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_IMAGE_ASPECT_COLOR_BIT, &fb->bloom_views[i]);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] bloom view level %d failed: %d", i, r);
        break;
      }
      VkImageView bv = fb->bloom_views[i];
      VkFramebufferCreateInfo bfb_ci = {
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = dev->bloom_render_pass,
          .attachmentCount = 1,
          .pAttachments = &bv,
          .width = bw,
          .height = bh,
          .layers = 1,
      };
      r = vkCreateFramebuffer(dev->device, &bfb_ci, NULL, &fb->bloom_fbs[i]);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] bloom framebuffer level %d failed: %d", i, r);
        break;
      }
      bw /= 2;
      bh /= 2;
    }

    /* Bloom upsample output images (separate targets, avoids LOAD_OP_LOAD) */
    bw = (uint32_t)width / 2;
    bh = (uint32_t)height / 2;
    for (int i = 0; i < MOP_VK_BLOOM_LEVELS; i++) {
      if (bw < 1)
        bw = 1;
      if (bh < 1)
        bh = 1;
      r = mop_vk_create_image(
          dev->device, &dev->mem_props, bw, bh, VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_SAMPLE_COUNT_1_BIT,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          &fb->bloom_up_images[i], &fb->bloom_up_memory[i]);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] bloom upsample image level %d failed: %d", i, r);
        break;
      }
      r = mop_vk_create_image_view(
          dev->device, fb->bloom_up_images[i], VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_IMAGE_ASPECT_COLOR_BIT, &fb->bloom_up_views[i]);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] bloom upsample view level %d failed: %d", i, r);
        break;
      }
      VkImageView buv = fb->bloom_up_views[i];
      VkFramebufferCreateInfo bufb_ci = {
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = dev->bloom_render_pass,
          .attachmentCount = 1,
          .pAttachments = &buv,
          .width = bw,
          .height = bh,
          .layers = 1,
      };
      r = vkCreateFramebuffer(dev->device, &bufb_ci, NULL,
                              &fb->bloom_up_fbs[i]);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] bloom upsample framebuffer level %d failed: %d", i, r);
        break;
      }
      bw /= 2;
      bh /= 2;
    }
  }

  /* ---- SSAO attachments (R8_UNORM, full resolution) ---- */
  if (dev->ssao_render_pass) {
    /* Raw SSAO output */
    r = mop_vk_create_image(
        dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
        VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        &fb->ssao_image, &fb->ssao_memory);
    if (r == VK_SUCCESS) {
      r = mop_vk_create_image_view(dev->device, fb->ssao_image,
                                   VK_FORMAT_R8_UNORM,
                                   VK_IMAGE_ASPECT_COLOR_BIT, &fb->ssao_view);
    }
    if (r == VK_SUCCESS) {
      VkFramebufferCreateInfo sfb_ci = {
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = dev->ssao_render_pass,
          .attachmentCount = 1,
          .pAttachments = &fb->ssao_view,
          .width = (uint32_t)width,
          .height = (uint32_t)height,
          .layers = 1,
      };
      r = vkCreateFramebuffer(dev->device, &sfb_ci, NULL, &fb->ssao_fb);
    }
    if (r != VK_SUCCESS)
      MOP_WARN("[VK] SSAO raw image/fb failed: %d", r);

    /* Blurred SSAO output */
    r = mop_vk_create_image(
        dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
        VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        &fb->ssao_blur_image, &fb->ssao_blur_memory);
    if (r == VK_SUCCESS) {
      r = mop_vk_create_image_view(
          dev->device, fb->ssao_blur_image, VK_FORMAT_R8_UNORM,
          VK_IMAGE_ASPECT_COLOR_BIT, &fb->ssao_blur_view);
    }
    if (r == VK_SUCCESS) {
      VkFramebufferCreateInfo sbfb_ci = {
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = dev->ssao_render_pass,
          .attachmentCount = 1,
          .pAttachments = &fb->ssao_blur_view,
          .width = (uint32_t)width,
          .height = (uint32_t)height,
          .layers = 1,
      };
      r = vkCreateFramebuffer(dev->device, &sbfb_ci, NULL, &fb->ssao_blur_fb);
    }
    if (r != VK_SUCCESS)
      MOP_WARN("[VK] SSAO blur image/fb failed: %d", r);
  }

  /* Merged SSAO+blur framebuffer (Phase 6) */
  if (dev->ssao_merged_render_pass && fb->ssao_image && fb->ssao_blur_image) {
    VkImageView merged_views[2] = {fb->ssao_view, fb->ssao_blur_view};
    VkFramebufferCreateInfo mfb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = dev->ssao_merged_render_pass,
        .attachmentCount = 2,
        .pAttachments = merged_views,
        .width = (uint32_t)fb->width,
        .height = (uint32_t)fb->height,
        .layers = 1,
    };
    vkCreateFramebuffer(dev->device, &mfb_ci, NULL, &fb->ssao_merged_fb);
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
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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

  /* ---- Per-frame light SSBO (all scene lights, shared across draws) ---- */
  fb->light_ssbo_size =
      (VkDeviceSize)MOP_VK_MAX_SSBO_LIGHTS * sizeof(MopVkLight);
  r = mop_vk_create_buffer(dev->device, &dev->mem_props, fb->light_ssbo_size,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &fb->light_ssbo, &fb->light_ssbo_mem);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] fb light SSBO: %d", r);
    return;
  }
  r = vkMapMemory(dev->device, fb->light_ssbo_mem, 0, fb->light_ssbo_size, 0,
                  &fb->light_ssbo_mapped);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] vkMapMemory light SSBO failed: %d", r);
    return;
  }
  fb->light_count_this_frame = 0;

  /* ---- Per-frame object SSBO (bindless: indexed by draw_id) ---- */
  if (dev->has_descriptor_indexing) {
    VkDeviceSize obj_ssbo_size =
        (VkDeviceSize)MOP_VK_MAX_DRAWS_PER_FRAME * sizeof(MopVkObjectData);
    r = mop_vk_create_buffer(dev->device, &dev->mem_props, obj_ssbo_size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &fb->object_ssbo, &fb->object_ssbo_mem);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] object SSBO alloc failed: %d", r);
    } else {
      r = vkMapMemory(dev->device, fb->object_ssbo_mem, 0, obj_ssbo_size, 0,
                      &fb->object_ssbo_mapped);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] object SSBO map failed: %d", r);
        fb->object_ssbo_mapped = NULL;
      }
    }
    fb->draw_count_this_frame = 0;

    /* Per-frame globals UBO (camera, shadow, exposure) */
    VkDeviceSize globals_size = sizeof(MopVkFrameGlobals);
    /* Align to UBO alignment */
    globals_size = mop_vk_align(globals_size, dev->min_ubo_alignment);
    r = mop_vk_create_buffer(dev->device, &dev->mem_props, globals_size,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &fb->globals_ubo, &fb->globals_ubo_mem);
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] globals UBO alloc failed: %d", r);
    } else {
      r = vkMapMemory(dev->device, fb->globals_ubo_mem, 0, globals_size, 0,
                      &fb->globals_ubo_mapped);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] globals UBO map failed: %d", r);
        fb->globals_ubo_mapped = NULL;
      }
    }
  }

  /* ---- Indirect draw buffers (Phase 2B — GPU culling) ---- */
  if (dev->gpu_culling_enabled) {
    /* VkDrawIndexedIndirectCommand = 20 bytes per command */
    VkDeviceSize cmd_size = (VkDeviceSize)MOP_VK_MAX_DRAWS_PER_FRAME * 20;

    /* Input draw commands (CPU-writable, GPU-readable as storage + indirect) */
    r = mop_vk_create_buffer(dev->device, &dev->mem_props, cmd_size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &fb->input_draw_cmds, &fb->input_draw_cmds_mem);
    if (r == VK_SUCCESS) {
      vkMapMemory(dev->device, fb->input_draw_cmds_mem, 0, cmd_size, 0,
                  &fb->input_draw_cmds_mapped);
    }

    /* Output draw commands (GPU-writable as storage, GPU-readable as indirect)
     * Does NOT need to be host-visible. */
    r = mop_vk_create_buffer(dev->device, &dev->mem_props, cmd_size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &fb->output_draw_cmds, &fb->output_draw_cmds_mem);
    /* Fallback: try without DEVICE_LOCAL (some integrated GPUs) */
    if (r != VK_SUCCESS) {
      r = mop_vk_create_buffer(dev->device, &dev->mem_props, cmd_size,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               &fb->output_draw_cmds,
                               &fb->output_draw_cmds_mem);
    }

    /* Draw count buffer (single uint32_t — atomic counter) */
    r = mop_vk_create_buffer(dev->device, &dev->mem_props, sizeof(uint32_t),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &fb->draw_count_buf, &fb->draw_count_buf_mem);
    if (r == VK_SUCCESS) {
      vkMapMemory(dev->device, fb->draw_count_buf_mem, 0, sizeof(uint32_t), 0,
                  &fb->draw_count_buf_mapped);
    }
  }

  /* ---- Hi-Z depth pyramid (Phase 2C — occlusion culling) ---- */
  if (dev->hiz_enabled) {
    uint32_t hw = (uint32_t)width;
    uint32_t hh = (uint32_t)height;
    /* Vulkan mip count = floor(log2(max(w,h))) + 1 */
    uint32_t max_dim = hw > hh ? hw : hh;
    uint32_t levels = 1;
    {
      uint32_t d = max_dim;
      while (d > 1) {
        d >>= 1;
        levels++;
      }
    }
    if (levels > MOP_VK_HIZ_MAX_LEVELS)
      levels = MOP_VK_HIZ_MAX_LEVELS;

    fb->hiz_width = hw;
    fb->hiz_height = hh;
    fb->hiz_levels = levels;

    r = mop_vk_create_image_mipped(
        dev->device, &dev->mem_props, hw, hh, VK_FORMAT_R32_SFLOAT, levels,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        &fb->hiz_image, &fb->hiz_memory);
    if (r == VK_SUCCESS) {
      for (uint32_t m = 0; m < levels; m++) {
        r = mop_vk_create_image_view_mip(
            dev->device, fb->hiz_image, VK_FORMAT_R32_SFLOAT,
            VK_IMAGE_ASPECT_COLOR_BIT, m, &fb->hiz_views[m]);
        if (r != VK_SUCCESS) {
          MOP_WARN("[VK] Hi-Z view for mip %u failed: %d", m, r);
          break;
        }
      }
    } else {
      MOP_WARN("[VK] Hi-Z image creation failed: %d", r);
      fb->hiz_levels = 0;
    }
  }

  /* ---- SSR output texture (R16G16B16A16_SFLOAT — same as HDR color) ---- */
  if (dev->ssr_pipeline && dev->ssr_render_pass) {
    r = mop_vk_create_image(
        dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        &fb->ssr_image, &fb->ssr_memory);
    if (r == VK_SUCCESS) {
      r = mop_vk_create_image_view(dev->device, fb->ssr_image,
                                   VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, &fb->ssr_view);
    }
    if (r == VK_SUCCESS) {
      VkImageView ssr_view = fb->ssr_view;
      VkFramebufferCreateInfo ssr_fb_ci = {
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = dev->ssr_render_pass,
          .attachmentCount = 1,
          .pAttachments = &ssr_view,
          .width = (uint32_t)width,
          .height = (uint32_t)height,
          .layers = 1,
      };
      r = vkCreateFramebuffer(dev->device, &ssr_fb_ci, NULL,
                              &fb->ssr_framebuffer);
      if (r != VK_SUCCESS)
        MOP_WARN("[VK] SSR framebuffer failed: %d", r);
    } else {
      MOP_WARN("[VK] SSR image/view failed: %d", r);
    }
  }

  /* ---- OIT render targets (accum + revealage + framebuffers) ---- */
  if (dev->oit_pipeline && dev->oit_render_pass &&
      dev->oit_composite_pipeline && dev->oit_composite_render_pass) {
    /* Accumulation: R16G16B16A16_SFLOAT */
    r = mop_vk_create_image(
        dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        &fb->oit_accum_image, &fb->oit_accum_mem);
    if (r == VK_SUCCESS) {
      r = mop_vk_create_image_view(
          dev->device, fb->oit_accum_image, VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_IMAGE_ASPECT_COLOR_BIT, &fb->oit_accum_view);
    }
    /* Revealage: R8_UNORM */
    if (r == VK_SUCCESS) {
      r = mop_vk_create_image(
          dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
          VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          &fb->oit_revealage_image, &fb->oit_revealage_mem);
    }
    if (r == VK_SUCCESS) {
      r = mop_vk_create_image_view(
          dev->device, fb->oit_revealage_image, VK_FORMAT_R8_UNORM,
          VK_IMAGE_ASPECT_COLOR_BIT, &fb->oit_revealage_view);
    }
    /* OIT accumulation framebuffer: accum + revealage + depth */
    if (r == VK_SUCCESS) {
      VkImageView oit_views[3] = {fb->oit_accum_view, fb->oit_revealage_view,
                                  fb->depth_view};
      VkFramebufferCreateInfo oit_fb_ci = {
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = dev->oit_render_pass,
          .attachmentCount = 3,
          .pAttachments = oit_views,
          .width = (uint32_t)width,
          .height = (uint32_t)height,
          .layers = 1,
      };
      r = vkCreateFramebuffer(dev->device, &oit_fb_ci, NULL,
                              &fb->oit_framebuffer);
      if (r != VK_SUCCESS)
        MOP_WARN("[VK] OIT framebuffer failed: %d", r);
    }
    /* OIT composite framebuffer: writes into color_image */
    if (r == VK_SUCCESS) {
      VkImageView comp_view = fb->color_view;
      VkFramebufferCreateInfo comp_fb_ci = {
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = dev->oit_composite_render_pass,
          .attachmentCount = 1,
          .pAttachments = &comp_view,
          .width = (uint32_t)width,
          .height = (uint32_t)height,
          .layers = 1,
      };
      r = vkCreateFramebuffer(dev->device, &comp_fb_ci, NULL,
                              &fb->oit_composite_framebuffer);
      if (r != VK_SUCCESS)
        MOP_WARN("[VK] OIT composite framebuffer failed: %d", r);
    }
    if (r != VK_SUCCESS) {
      MOP_WARN("[VK] OIT image/view allocation failed: %d", r);
    }
  }

  /* ---- Decal framebuffer + per-frame UBO ---- */
  if (dev->decal_pipeline && dev->decal_render_pass) {
    /* Framebuffer: color_image only (depth sampled via descriptor) */
    VkImageView decal_view = fb->color_view;
    VkFramebufferCreateInfo decal_fb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = dev->decal_render_pass,
        .attachmentCount = 1,
        .pAttachments = &decal_view,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .layers = 1,
    };
    r = vkCreateFramebuffer(dev->device, &decal_fb_ci, NULL,
                            &fb->decal_framebuffer);
    if (r != VK_SUCCESS)
      MOP_WARN("[VK] Decal framebuffer failed: %d", r);

    /* Per-frame decal UBO: inv_vp(64) + reverse_z(4) + opacity(4) + pad(8) = 80
     * bytes */
    r = mop_vk_create_buffer(dev->device, &dev->mem_props, 80,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &dev->decal_ubo, &dev->decal_ubo_mem);
    if (r == VK_SUCCESS) {
      vkMapMemory(dev->device, dev->decal_ubo_mem, 0, 80, 0,
                  &dev->decal_ubo_mapped);
    } else {
      MOP_WARN("[VK] Decal UBO alloc failed: %d", r);
    }
  }

  /* ---- Volumetric fog framebuffer + per-frame UBO ---- */
  if (dev->volumetric_pipeline && dev->volumetric_render_pass) {
    VkImageView vol_view = fb->color_view;
    VkFramebufferCreateInfo vol_fb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = dev->volumetric_render_pass,
        .attachmentCount = 1,
        .pAttachments = &vol_view,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .layers = 1,
    };
    r = vkCreateFramebuffer(dev->device, &vol_fb_ci, NULL,
                            &fb->volumetric_framebuffer);
    if (r != VK_SUCCESS)
      MOP_WARN("[VK] Volumetric framebuffer failed: %d", r);

    /* Per-frame UBO: inv_vp(64) + cam_pos(16) + fog_params(16) +
     * anisotropy(4) + num_lights(4) + num_steps(4) + reverse_z(4) = 112 */
    r = mop_vk_create_buffer(dev->device, &dev->mem_props, 112,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &fb->volumetric_ubo, &fb->volumetric_ubo_mem);
    if (r == VK_SUCCESS) {
      vkMapMemory(dev->device, fb->volumetric_ubo_mem, 0, 112, 0,
                  &fb->volumetric_ubo_mapped);
    } else {
      MOP_WARN("[VK] Volumetric UBO alloc failed: %d", r);
    }
  }

  /* ---- TAA history textures (ping-pong, same format as LDR color) ---- */
  if (dev->taa_enabled && dev->taa_render_pass) {
    fb->taa_current = 0;
    for (int i = 0; i < 2; i++) {
      r = mop_vk_create_image(
          dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height,
          VK_FORMAT_R8G8B8A8_SRGB, VK_SAMPLE_COUNT_1_BIT,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          &fb->taa_history[i], &fb->taa_history_mem[i]);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] TAA history image %d failed: %d", i, r);
        break;
      }
      r = mop_vk_create_image_view(
          dev->device, fb->taa_history[i], VK_FORMAT_R8G8B8A8_SRGB,
          VK_IMAGE_ASPECT_COLOR_BIT, &fb->taa_history_view[i]);
      if (r != VK_SUCCESS) {
        MOP_WARN("[VK] TAA history view %d failed: %d", i, r);
        break;
      }

      VkImageView taa_view = fb->taa_history_view[i];
      VkFramebufferCreateInfo taa_fb_ci = {
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = dev->taa_render_pass,
          .attachmentCount = 1,
          .pAttachments = &taa_view,
          .width = (uint32_t)width,
          .height = (uint32_t)height,
          .layers = 1,
      };
      r = vkCreateFramebuffer(dev->device, &taa_fb_ci, NULL,
                              &fb->taa_framebuffer[i]);
      if (r != VK_SUCCESS)
        MOP_WARN("[VK] TAA framebuffer %d failed: %d", i, r);
    }
  }
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

  /* Depth copy (R32_SFLOAT for overlay sampling) */
  if (fb->depth_copy_view)
    vkDestroyImageView(d, fb->depth_copy_view, NULL);
  if (fb->depth_copy_image)
    vkDestroyImage(d, fb->depth_copy_image, NULL);
  if (fb->depth_copy_memory)
    vkFreeMemory(d, fb->depth_copy_memory, NULL);

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

  /* Light SSBO */
  if (fb->light_ssbo_mapped)
    vkUnmapMemory(d, fb->light_ssbo_mem);
  if (fb->light_ssbo)
    vkDestroyBuffer(d, fb->light_ssbo, NULL);
  if (fb->light_ssbo_mem)
    vkFreeMemory(d, fb->light_ssbo_mem, NULL);

  /* Object SSBO (bindless) */
  if (fb->object_ssbo_mapped)
    vkUnmapMemory(d, fb->object_ssbo_mem);
  if (fb->object_ssbo)
    vkDestroyBuffer(d, fb->object_ssbo, NULL);
  if (fb->object_ssbo_mem)
    vkFreeMemory(d, fb->object_ssbo_mem, NULL);

  /* Globals UBO (bindless) */
  if (fb->globals_ubo_mapped)
    vkUnmapMemory(d, fb->globals_ubo_mem);
  if (fb->globals_ubo)
    vkDestroyBuffer(d, fb->globals_ubo, NULL);
  if (fb->globals_ubo_mem)
    vkFreeMemory(d, fb->globals_ubo_mem, NULL);

  /* Indirect draw buffers (Phase 2B) */
  if (fb->input_draw_cmds_mapped)
    vkUnmapMemory(d, fb->input_draw_cmds_mem);
  if (fb->input_draw_cmds)
    vkDestroyBuffer(d, fb->input_draw_cmds, NULL);
  if (fb->input_draw_cmds_mem)
    vkFreeMemory(d, fb->input_draw_cmds_mem, NULL);
  if (fb->output_draw_cmds)
    vkDestroyBuffer(d, fb->output_draw_cmds, NULL);
  if (fb->output_draw_cmds_mem)
    vkFreeMemory(d, fb->output_draw_cmds_mem, NULL);
  if (fb->draw_count_buf_mapped)
    vkUnmapMemory(d, fb->draw_count_buf_mem);
  if (fb->draw_count_buf)
    vkDestroyBuffer(d, fb->draw_count_buf, NULL);
  if (fb->draw_count_buf_mem)
    vkFreeMemory(d, fb->draw_count_buf_mem, NULL);

  /* Hi-Z pyramid cleanup */
  for (uint32_t m = 0; m < fb->hiz_levels; m++) {
    if (fb->hiz_views[m])
      vkDestroyImageView(d, fb->hiz_views[m], NULL);
  }
  if (fb->hiz_image)
    vkDestroyImage(d, fb->hiz_image, NULL);
  if (fb->hiz_memory)
    vkFreeMemory(d, fb->hiz_memory, NULL);

  /* Instance buffer */
  if (fb->instance_mapped)
    vkUnmapMemory(d, fb->instance_mem);
  if (fb->instance_buf)
    vkDestroyBuffer(d, fb->instance_buf, NULL);
  if (fb->instance_mem)
    vkFreeMemory(d, fb->instance_mem, NULL);

  /* Overlay SSBO */
  if (fb->overlay_ssbo_mapped)
    vkUnmapMemory(d, fb->overlay_ssbo_mem);
  if (fb->overlay_ssbo)
    vkDestroyBuffer(d, fb->overlay_ssbo, NULL);
  if (fb->overlay_ssbo_mem)
    vkFreeMemory(d, fb->overlay_ssbo_mem, NULL);

  /* Bloom mip chain */
  for (int i = 0; i < MOP_VK_BLOOM_LEVELS; i++) {
    if (fb->bloom_fbs[i])
      vkDestroyFramebuffer(d, fb->bloom_fbs[i], NULL);
    if (fb->bloom_views[i])
      vkDestroyImageView(d, fb->bloom_views[i], NULL);
    if (fb->bloom_images[i])
      vkDestroyImage(d, fb->bloom_images[i], NULL);
    if (fb->bloom_memory[i])
      vkFreeMemory(d, fb->bloom_memory[i], NULL);
    /* Bloom upsample output images */
    if (fb->bloom_up_fbs[i])
      vkDestroyFramebuffer(d, fb->bloom_up_fbs[i], NULL);
    if (fb->bloom_up_views[i])
      vkDestroyImageView(d, fb->bloom_up_views[i], NULL);
    if (fb->bloom_up_images[i])
      vkDestroyImage(d, fb->bloom_up_images[i], NULL);
    if (fb->bloom_up_memory[i])
      vkFreeMemory(d, fb->bloom_up_memory[i], NULL);
  }

  /* SSAO attachments */
  if (fb->ssao_merged_fb)
    vkDestroyFramebuffer(d, fb->ssao_merged_fb, NULL);
  if (fb->ssao_blur_fb)
    vkDestroyFramebuffer(d, fb->ssao_blur_fb, NULL);
  if (fb->ssao_blur_view)
    vkDestroyImageView(d, fb->ssao_blur_view, NULL);
  if (fb->ssao_blur_image)
    vkDestroyImage(d, fb->ssao_blur_image, NULL);
  if (fb->ssao_blur_memory)
    vkFreeMemory(d, fb->ssao_blur_memory, NULL);
  if (fb->ssao_fb)
    vkDestroyFramebuffer(d, fb->ssao_fb, NULL);
  if (fb->ssao_view)
    vkDestroyImageView(d, fb->ssao_view, NULL);
  if (fb->ssao_image)
    vkDestroyImage(d, fb->ssao_image, NULL);
  if (fb->ssao_memory)
    vkFreeMemory(d, fb->ssao_memory, NULL);

  /* SSR */
  if (fb->ssr_framebuffer)
    vkDestroyFramebuffer(d, fb->ssr_framebuffer, NULL);
  if (fb->ssr_view)
    vkDestroyImageView(d, fb->ssr_view, NULL);
  if (fb->ssr_image)
    vkDestroyImage(d, fb->ssr_image, NULL);
  if (fb->ssr_memory)
    vkFreeMemory(d, fb->ssr_memory, NULL);

  /* OIT */
  if (fb->oit_composite_framebuffer)
    vkDestroyFramebuffer(d, fb->oit_composite_framebuffer, NULL);
  if (fb->oit_framebuffer)
    vkDestroyFramebuffer(d, fb->oit_framebuffer, NULL);
  if (fb->oit_revealage_view)
    vkDestroyImageView(d, fb->oit_revealage_view, NULL);
  if (fb->oit_revealage_image)
    vkDestroyImage(d, fb->oit_revealage_image, NULL);
  if (fb->oit_revealage_mem)
    vkFreeMemory(d, fb->oit_revealage_mem, NULL);
  if (fb->oit_accum_view)
    vkDestroyImageView(d, fb->oit_accum_view, NULL);
  if (fb->oit_accum_image)
    vkDestroyImage(d, fb->oit_accum_image, NULL);
  if (fb->oit_accum_mem)
    vkFreeMemory(d, fb->oit_accum_mem, NULL);

  /* Decal */
  if (fb->decal_framebuffer)
    vkDestroyFramebuffer(d, fb->decal_framebuffer, NULL);
  if (dev->decal_ubo_mapped)
    vkUnmapMemory(d, dev->decal_ubo_mem);
  if (dev->decal_ubo)
    vkDestroyBuffer(d, dev->decal_ubo, NULL);
  if (dev->decal_ubo_mem)
    vkFreeMemory(d, dev->decal_ubo_mem, NULL);
  dev->decal_ubo = VK_NULL_HANDLE;
  dev->decal_ubo_mem = VK_NULL_HANDLE;
  dev->decal_ubo_mapped = NULL;

  /* Volumetric fog */
  if (fb->volumetric_framebuffer)
    vkDestroyFramebuffer(d, fb->volumetric_framebuffer, NULL);
  if (fb->volumetric_ubo_mapped)
    vkUnmapMemory(d, fb->volumetric_ubo_mem);
  if (fb->volumetric_ubo)
    vkDestroyBuffer(d, fb->volumetric_ubo, NULL);
  if (fb->volumetric_ubo_mem)
    vkFreeMemory(d, fb->volumetric_ubo_mem, NULL);
  fb->volumetric_ubo = VK_NULL_HANDLE;
  fb->volumetric_ubo_mem = VK_NULL_HANDLE;
  fb->volumetric_ubo_mapped = NULL;

  /* TAA history textures */
  for (int i = 0; i < 2; i++) {
    if (fb->taa_framebuffer[i])
      vkDestroyFramebuffer(d, fb->taa_framebuffer[i], NULL);
    if (fb->taa_history_view[i])
      vkDestroyImageView(d, fb->taa_history_view[i], NULL);
    if (fb->taa_history[i])
      vkDestroyImage(d, fb->taa_history[i], NULL);
    if (fb->taa_history_mem[i])
      vkFreeMemory(d, fb->taa_history_mem[i], NULL);
  }

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
  /* Select current frame's resources */
  MopVkFrameResources *frame = &dev->frames[dev->frame_index];

  /* Wait for this frame's fence (may still be in-flight from N frames ago) */
  vkWaitForFences(dev->device, 1, &frame->fence, VK_TRUE, UINT64_MAX);
  vkResetFences(dev->device, 1, &frame->fence);

  /* ---- Deferred readback from previous frame (Phase 1) ----
   * The fence wait above guarantees the GPU finished the previous frame
   * that used these resources. Read back timestamps + pixel data now. */
  if (dev->prev_framebuffer) {
    MopRhiFramebuffer *prev_fb = dev->prev_framebuffer;

    /* Read GPU timestamps */
    if (dev->has_timestamp_queries) {
      uint64_t timestamps[2] = {0, 0};
      VkResult tr = vkGetQueryPoolResults(
          dev->device, dev->timestamp_pool, 0, 2, sizeof(timestamps),
          timestamps, sizeof(uint64_t),
          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      if (tr == VK_SUCCESS && timestamps[1] >= timestamps[0]) {
        double elapsed_ns =
            (double)(timestamps[1] - timestamps[0]) * dev->timestamp_period_ns;
        dev->last_timing.gpu_frame_ms = elapsed_ns / 1e6;
      }

      /* Read per-pass GPU timestamps */
      if (dev->pass_timestamp_pool && dev->pass_timing_count > 0) {
        uint64_t pass_ts[MOP_VK_MAX_PASS_TIMESTAMPS];
        uint32_t count = dev->pass_query_count;
        if (count > MOP_VK_MAX_PASS_TIMESTAMPS)
          count = MOP_VK_MAX_PASS_TIMESTAMPS;
        tr = vkGetQueryPoolResults(
            dev->device, dev->pass_timestamp_pool, 0, count,
            count * sizeof(uint64_t), pass_ts, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (tr == VK_SUCCESS) {
          dev->pass_timing_result_count = 0;
          for (uint32_t i = 0; i < dev->pass_timing_count; i++) {
            uint32_t qs = dev->pass_timing_entries[i].query_start;
            uint32_t qe = dev->pass_timing_entries[i].query_end;
            if (qs < count && qe < count && pass_ts[qe] >= pass_ts[qs]) {
              uint32_t ri = dev->pass_timing_result_count++;
              dev->pass_timing_results[ri].name =
                  dev->pass_timing_entries[i].name;
              dev->pass_timing_results[ri].gpu_ms =
                  (double)(pass_ts[qe] - pass_ts[qs]) *
                  dev->timestamp_period_ns / 1e6;
            }
          }
        }
      }
    }

    /* Copy from mapped staging to CPU arrays (deferred readback) */
    size_t npixels = (size_t)prev_fb->width * (size_t)prev_fb->height;
    if (prev_fb->readback_color && prev_fb->readback_color_mapped)
      memcpy(prev_fb->readback_color, prev_fb->readback_color_mapped,
             npixels * 4);
    if (prev_fb->readback_pick && prev_fb->readback_pick_mapped)
      memcpy(prev_fb->readback_pick, prev_fb->readback_pick_mapped,
             npixels * sizeof(uint32_t));
    if (prev_fb->readback_depth && prev_fb->readback_depth_mapped)
      memcpy(prev_fb->readback_depth, prev_fb->readback_depth_mapped,
             npixels * sizeof(float));

    /* Read back GPU cull stats from previous frame */
    if (prev_fb->draw_count_buf_mapped) {
      prev_fb->last_visible_draws = *(uint32_t *)prev_fb->draw_count_buf_mapped;
      prev_fb->last_culled_draws =
          prev_fb->draw_count_this_frame > prev_fb->last_visible_draws
              ? prev_fb->draw_count_this_frame - prev_fb->last_visible_draws
              : 0;
    }
  }

  /* Set active-frame aliases so draw/frame_end code works unchanged */
  dev->cmd_buf = frame->cmd_buf;
  dev->fence = frame->fence;
  dev->desc_pool = frame->desc_pool;

  /* Reset descriptor pool for this frame */
  vkResetDescriptorPool(dev->device, dev->desc_pool, 0);

  /* Reset per-thread descriptor pools and secondary CBs */
  for (uint32_t t = 0; t < dev->thread_count; t++) {
    vkResetDescriptorPool(dev->device, dev->thread_states[t].desc_pool, 0);
    vkResetCommandBuffer(dev->thread_states[t].secondary_cb, 0);
    dev->thread_states[t].cb_recording = false;
  }

  /* Reset UBO offset, instance offset, and light SSBO state */
  fb->ubo_offset = 0;
  fb->instance_offset = 0;
  fb->light_count_this_frame = 0;
  fb->draw_count_this_frame = 0;
  fb->globals_scene_written = false;
  fb->bindless_ds = VK_NULL_HANDLE;
  fb->cull_ds = VK_NULL_HANDLE;

  /* Reset draw count buffer for GPU culling */
  if (fb->draw_count_buf_mapped)
    *(uint32_t *)fb->draw_count_buf_mapped = 0;

  /* Reset OIT deferred draw count */
  dev->oit_draw_count = 0;

  /* Reset shadow draw count for this frame */
  dev->shadow_draw_count = 0;
  dev->shadow_data_valid = false;

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

    /* Reset per-pass timestamp pool (Phase 9A) */
    if (dev->pass_timestamp_pool) {
      vkCmdResetQueryPool(dev->cmd_buf, dev->pass_timestamp_pool, 0,
                          MOP_VK_MAX_PASS_TIMESTAMPS);
      dev->pass_query_count = 0;
      dev->pass_timing_count = 0;
    }
  }

  /* Begin render pass — reversed-Z clears depth to 0.0 */
  float depth_clear = dev->reverse_z ? 0.0f : 1.0f;
  uint32_t clear_count;
  VkClearValue clears[6];
  /* Clear color alpha=0 marks background pixels so the tonemap pass can
   * skip exposure+ACES for them (background brightness should not change
   * with the exposure slider — only skybox/scene pixels get tonemapped). */
  if (dev->msaa_samples > VK_SAMPLE_COUNT_1_BIT) {
    /* 6 attachments: 3 MSAA (cleared) + 3 resolve (DONT_CARE but must exist) */
    clears[0] = (VkClearValue){
        .color = {{clear_color.r, clear_color.g, clear_color.b, 0.0f}}};
    clears[1] = (VkClearValue){.color = {{0}}}; /* MSAA picking */
    clears[2] =
        (VkClearValue){.depthStencil = {depth_clear, 0}}; /* MSAA depth */
    clears[3] = (VkClearValue){.color = {{0}}};           /* resolve color */
    clears[4] = (VkClearValue){.color = {{0}}};           /* resolve pick */
    clears[5] = (VkClearValue){.depthStencil = {0, 0}};   /* resolve depth */
    clear_count = 6;
  } else {
    clears[0] = (VkClearValue){
        .color = {{clear_color.r, clear_color.g, clear_color.b, 0.0f}}};
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

  /* Shadow maps are rendered in frame_end (temporal, 1 frame behind).
   * The shadow image stays in DEPTH_STENCIL_READ_ONLY_OPTIMAL during
   * the main render pass so the fragment shader can sample it. */

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
 * 10a. Bindless descriptor set allocation (once per frame, lazy)
 * ========================================================================= */

static bool vk_ensure_bindless_ds(MopRhiDevice *dev, MopRhiFramebuffer *fb) {
  if (fb->bindless_ds != VK_NULL_HANDLE)
    return true;

  /* Variable descriptor count for the texture array (binding 7) */
  uint32_t variable_count = dev->texture_registry_count;
  VkDescriptorSetVariableDescriptorCountAllocateInfo var_ci = {
      .sType =
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
      .descriptorSetCount = 1,
      .pDescriptorCounts = &variable_count,
  };

  VkDescriptorSetAllocateInfo ds_ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .pNext = &var_ci,
      .descriptorPool = dev->desc_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dev->bindless_desc_layout,
  };

  VkResult r = vkAllocateDescriptorSets(dev->device, &ds_ai, &fb->bindless_ds);
  if (r != VK_SUCCESS) {
    MOP_WARN("[VK] bindless DS alloc failed: %d", r);
    fb->bindless_ds = VK_NULL_HANDLE;
    return false;
  }

  /* Populate the descriptor set — all bindings are updated once per frame */

  /* Binding 0: Object SSBO */
  VkDescriptorBufferInfo obj_ssbo_info = {
      .buffer = fb->object_ssbo,
      .offset = 0,
      .range =
          (VkDeviceSize)MOP_VK_MAX_DRAWS_PER_FRAME * sizeof(MopVkObjectData),
  };

  /* Binding 1: Frame globals UBO */
  VkDescriptorBufferInfo globals_info = {
      .buffer = fb->globals_ubo,
      .offset = 0,
      .range = sizeof(MopVkFrameGlobals),
  };

  /* Binding 2: Light SSBO */
  VkDeviceSize light_range =
      fb->light_count_this_frame > 0
          ? (VkDeviceSize)fb->light_count_this_frame * sizeof(MopVkLight)
          : sizeof(MopVkLight);
  VkDescriptorBufferInfo light_info = {
      .buffer = fb->light_ssbo,
      .offset = 0,
      .range = light_range,
  };

  /* Binding 3: Shadow map */
  VkDescriptorImageInfo shadow_info = {
      .sampler =
          dev->shadow_sampler ? dev->shadow_sampler : dev->default_sampler,
      .imageView =
          dev->shadow_array_view ? dev->shadow_array_view : dev->white_view,
      .imageLayout = dev->shadow_array_view
                         ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  /* Bindings 4-6: IBL */
  VkDescriptorImageInfo irr_info = {
      .sampler = dev->default_sampler,
      .imageView =
          dev->irradiance_view ? dev->irradiance_view : dev->black_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo pf_info = {
      .sampler = dev->default_sampler,
      .imageView =
          dev->prefiltered_view ? dev->prefiltered_view : dev->black_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo brdf_info = {
      .sampler = dev->default_sampler,
      .imageView = dev->brdf_lut_view ? dev->brdf_lut_view : dev->black_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  /* Binding 7: Bindless texture array */
  VkDescriptorImageInfo *tex_infos = NULL;
  uint32_t tex_count = dev->texture_registry_count;
  if (tex_count > 0) {
    tex_infos = malloc(tex_count * sizeof(VkDescriptorImageInfo));
    for (uint32_t i = 0; i < tex_count; i++) {
      tex_infos[i] = (VkDescriptorImageInfo){
          .sampler = dev->default_sampler,
          .imageView = dev->texture_registry[i] ? dev->texture_registry[i]
                                                : dev->white_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
    }
  }

  VkWriteDescriptorSet writes[8] = {
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = fb->bindless_ds,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &obj_ssbo_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = fb->bindless_ds,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = &globals_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = fb->bindless_ds,
          .dstBinding = 2,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &light_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = fb->bindless_ds,
          .dstBinding = 3,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &shadow_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = fb->bindless_ds,
          .dstBinding = 4,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &irr_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = fb->bindless_ds,
          .dstBinding = 5,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &pf_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = fb->bindless_ds,
          .dstBinding = 6,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &brdf_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = fb->bindless_ds,
          .dstBinding = 7,
          .descriptorCount = tex_count > 0 ? tex_count : 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = tex_infos ? tex_infos : &irr_info, /* dummy if 0 */
      },
  };

  uint32_t write_count = tex_count > 0 ? 8 : 7;
  vkUpdateDescriptorSets(dev->device, write_count, writes, 0, NULL);
  free(tex_infos);
  return true;
}

/* =========================================================================
 * 10b. Populate bindless globals UBO (once per frame, on first draw)
 * ========================================================================= */

static void vk_write_globals_ubo(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                                 const MopRhiDrawCall *call) {
  if (!fb->globals_ubo_mapped)
    return;

  MopVkFrameGlobals *g = (MopVkFrameGlobals *)fb->globals_ubo_mapped;
  g->light_dir[0] = call->light_dir.x;
  g->light_dir[1] = call->light_dir.y;
  g->light_dir[2] = call->light_dir.z;
  g->light_dir[3] = 0.0f;
  g->cam_pos[0] = call->cam_eye.x;
  g->cam_pos[1] = call->cam_eye.y;
  g->cam_pos[2] = call->cam_eye.z;
  g->cam_pos[3] = 0.0f;
  g->shadows_enabled = dev->shadows_enabled ? 1 : 0;
  g->cascade_count = dev->shadows_enabled ? MOP_VK_CASCADE_COUNT : 0;
  g->num_lights = (int32_t)(call->light_count < MOP_VK_MAX_SSBO_LIGHTS
                                ? call->light_count
                                : MOP_VK_MAX_SSBO_LIGHTS);
  g->exposure = 1.0f; /* exposure applied in tonemap pass */
  if (dev->shadows_enabled) {
    for (int c = 0; c < MOP_VK_CASCADE_COUNT; c++)
      memcpy(g->cascade_vp[c], dev->cascade_vp[c].d, 64);
    for (int c = 0; c < MOP_VK_CASCADE_COUNT; c++)
      g->cascade_splits[c] = dev->cascade_splits[c + 1];
  } else {
    memset(g->cascade_vp, 0, sizeof(g->cascade_vp));
    memset(g->cascade_splits, 0, sizeof(g->cascade_splits));
  }

  /* View-projection matrix for GPU culling */
  MopMat4 vp_mat = mop_mat4_multiply(call->projection, call->view);
  memcpy(g->view_proj, vp_mat.d, 64);

  /* Extract frustum planes from VP matrix (Gribb-Hartmann method).
   * Each plane: (a, b, c, d) where ax + by + cz + d <= 0 is outside.
   * Column-major: M(row,col) = d[col*4 + row]. */
  const float *m = vp_mat.d;

  /* Left   plane: row3 + row0 */
  g->frustum_planes[0][0] = m[3] + m[0];
  g->frustum_planes[0][1] = m[7] + m[4];
  g->frustum_planes[0][2] = m[11] + m[8];
  g->frustum_planes[0][3] = m[15] + m[12];
  /* Right  plane: row3 - row0 */
  g->frustum_planes[1][0] = m[3] - m[0];
  g->frustum_planes[1][1] = m[7] - m[4];
  g->frustum_planes[1][2] = m[11] - m[8];
  g->frustum_planes[1][3] = m[15] - m[12];
  /* Bottom plane: row3 + row1 */
  g->frustum_planes[2][0] = m[3] + m[1];
  g->frustum_planes[2][1] = m[7] + m[5];
  g->frustum_planes[2][2] = m[11] + m[9];
  g->frustum_planes[2][3] = m[15] + m[13];
  /* Top    plane: row3 - row1 */
  g->frustum_planes[3][0] = m[3] - m[1];
  g->frustum_planes[3][1] = m[7] - m[5];
  g->frustum_planes[3][2] = m[11] - m[9];
  g->frustum_planes[3][3] = m[15] - m[13];
  /* Near   plane: row3 + row2 */
  g->frustum_planes[4][0] = m[3] + m[2];
  g->frustum_planes[4][1] = m[7] + m[6];
  g->frustum_planes[4][2] = m[11] + m[10];
  g->frustum_planes[4][3] = m[15] + m[14];
  /* Far    plane: row3 - row2 */
  g->frustum_planes[5][0] = m[3] - m[2];
  g->frustum_planes[5][1] = m[7] - m[6];
  g->frustum_planes[5][2] = m[11] - m[10];
  g->frustum_planes[5][3] = m[15] - m[14];

  /* Normalize planes */
  for (int i = 0; i < 6; i++) {
    float len = sqrtf(g->frustum_planes[i][0] * g->frustum_planes[i][0] +
                      g->frustum_planes[i][1] * g->frustum_planes[i][1] +
                      g->frustum_planes[i][2] * g->frustum_planes[i][2]);
    if (len > 1e-6f) {
      float inv = 1.0f / len;
      g->frustum_planes[i][0] *= inv;
      g->frustum_planes[i][1] *= inv;
      g->frustum_planes[i][2] *= inv;
      g->frustum_planes[i][3] *= inv;
    }
  }

  g->total_draws = 0; /* updated after all draws are recorded */
  g->_pad_globals[0] = 0;
  g->_pad_globals[1] = 0;
  g->_pad_globals[2] = 0;
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

  /* ---- Bindless draw path (Phase 2A) ---- */
  if (dev->has_descriptor_indexing && fb->object_ssbo_mapped) {
    uint32_t key = mop_vk_pipeline_key(call->wireframe, call->depth_test,
                                       call->backface_cull, call->blend_mode,
                                       vertex_stride);

    VkPipeline pipeline = mop_vk_get_bindless_pipeline(dev, key, vertex_stride);
    if (!pipeline)
      goto fallback_draw;

    /* Upload lights on first draw of the frame that carries light data */
    uint32_t lc = call->light_count < MOP_VK_MAX_SSBO_LIGHTS
                      ? call->light_count
                      : MOP_VK_MAX_SSBO_LIGHTS;
    if (fb->light_count_this_frame == 0 && lc > 0 && fb->light_ssbo_mapped) {
      MopVkLight *dst = (MopVkLight *)fb->light_ssbo_mapped;
      for (uint32_t i = 0; i < lc; i++) {
        const MopLight *src = &call->lights[i];
        dst[i].position[0] = src->position.x;
        dst[i].position[1] = src->position.y;
        dst[i].position[2] = src->position.z;
        dst[i].position[3] = (float)src->type;
        dst[i].direction[0] = src->direction.x;
        dst[i].direction[1] = src->direction.y;
        dst[i].direction[2] = src->direction.z;
        dst[i].direction[3] = 0.0f;
        dst[i].color[0] = src->color.r;
        dst[i].color[1] = src->color.g;
        dst[i].color[2] = src->color.b;
        dst[i].color[3] = src->intensity;
        dst[i].params[0] = src->range;
        dst[i].params[1] = src->spot_inner_cos;
        dst[i].params[2] = src->spot_outer_cos;
        dst[i].params[3] = src->active ? 1.0f : 0.0f;
      }
      fb->light_count_this_frame = lc;

      /* If the globals UBO was already written (by a lightless draw like
       * the background gradient), patch num_lights + cam_pos now that we
       * have the real scene data. */
      if (fb->globals_ubo_mapped && fb->draw_count_this_frame > 0) {
        MopVkFrameGlobals *g = (MopVkFrameGlobals *)fb->globals_ubo_mapped;
        g->num_lights = (int32_t)lc;
        g->cam_pos[0] = call->cam_eye.x;
        g->cam_pos[1] = call->cam_eye.y;
        g->cam_pos[2] = call->cam_eye.z;
        g->cam_pos[3] = 0.0f;
      }
    }

    /* Write globals UBO on first draw, or re-write when the first draw
     * with actual scene data arrives (lights + real camera). */
    if (fb->draw_count_this_frame == 0) {
      vk_write_globals_ubo(dev, fb, call);
    } else if (!fb->globals_scene_written && call->light_count > 0) {
      /* The first globals write came from a lightless draw (background).
       * Re-write with proper camera + frustum planes from the scene. */
      vk_write_globals_ubo(dev, fb, call);
      fb->globals_scene_written = true;
    }

    /* CPU frustum culling (Phase 2B) — skip draws whose bounding sphere
     * is entirely outside any frustum plane.  Planes are cached in the
     * globals UBO (already written above on first draw). */
    if (fb->globals_ubo_mapped) {
      float dx = call->aabb_max.x - call->aabb_min.x;
      float dy = call->aabb_max.y - call->aabb_min.y;
      float dz = call->aabb_max.z - call->aabb_min.z;
      float local_radius = sqrtf(dx * dx + dy * dy + dz * dz) * 0.5f;
      if (local_radius > 0.0f) {
        /* Transform local AABB center to world space via model matrix */
        float cx = (call->aabb_min.x + call->aabb_max.x) * 0.5f;
        float cy = (call->aabb_min.y + call->aabb_max.y) * 0.5f;
        float cz = (call->aabb_min.z + call->aabb_max.z) * 0.5f;
        const float *md = call->model.d;
        float wx = md[0] * cx + md[4] * cy + md[8] * cz + md[12];
        float wy = md[1] * cx + md[5] * cy + md[9] * cz + md[13];
        float wz = md[2] * cx + md[6] * cy + md[10] * cz + md[14];

        /* Scale radius by max axis scale */
        float s0 = sqrtf(md[0] * md[0] + md[1] * md[1] + md[2] * md[2]);
        float s1 = sqrtf(md[4] * md[4] + md[5] * md[5] + md[6] * md[6]);
        float s2 = sqrtf(md[8] * md[8] + md[9] * md[9] + md[10] * md[10]);
        float max_scale = s0 > s1 ? (s0 > s2 ? s0 : s2) : (s1 > s2 ? s1 : s2);
        float world_radius = local_radius * max_scale;

        /* Test against 6 frustum planes */
        MopVkFrameGlobals *g = (MopVkFrameGlobals *)fb->globals_ubo_mapped;
        bool culled = false;
        for (int p = 0; p < 6; p++) {
          float dist = g->frustum_planes[p][0] * wx +
                       g->frustum_planes[p][1] * wy +
                       g->frustum_planes[p][2] * wz + g->frustum_planes[p][3];
          if (dist < -world_radius) {
            culled = true;
            break;
          }
        }
        if (culled)
          return; /* entirely outside frustum — skip draw */
      }
    }

    /* Ensure bindless descriptor set is allocated for this frame */
    if (!vk_ensure_bindless_ds(dev, fb))
      goto fallback_draw;

    /* Capacity check */
    uint32_t draw_id = fb->draw_count_this_frame;
    if (draw_id >= MOP_VK_MAX_DRAWS_PER_FRAME) {
      MOP_WARN("[VK] bindless draw limit reached (%u)", draw_id);
      goto fallback_draw;
    }

    /* Write per-object data to SSBO */
    MopVkObjectData *obj =
        &((MopVkObjectData *)fb->object_ssbo_mapped)[draw_id];
    memcpy(obj->model, call->model.d, 64);
    obj->ambient = call->ambient;
    obj->opacity = call->opacity;
    obj->object_id = call->object_id;
    obj->blend_mode = (int32_t)call->blend_mode;
    obj->metallic = call->metallic;
    obj->roughness = call->roughness;
    obj->base_tex_idx =
        call->texture ? (int32_t)call->texture->bindless_index : -1;
    obj->normal_tex_idx =
        call->normal_map ? (int32_t)call->normal_map->bindless_index : -1;
    obj->emissive[0] = call->emissive.x;
    obj->emissive[1] = call->emissive.y;
    obj->emissive[2] = call->emissive.z;
    obj->emissive[3] = 0.0f;
    obj->mr_tex_idx =
        call->metallic_roughness_map
            ? (int32_t)call->metallic_roughness_map->bindless_index
            : -1;
    obj->ao_tex_idx = call->ao_map ? (int32_t)call->ao_map->bindless_index : -1;
    obj->_pad0[0] = 0;
    obj->_pad0[1] = 0;

    /* Bounding sphere in local space (from AABB center + half-diagonal) */
    {
      float dx = call->aabb_max.x - call->aabb_min.x;
      float dy = call->aabb_max.y - call->aabb_min.y;
      float dz = call->aabb_max.z - call->aabb_min.z;
      obj->bound_sphere[0] =
          (call->aabb_min.x + call->aabb_max.x) * 0.5f; /* center x */
      obj->bound_sphere[1] =
          (call->aabb_min.y + call->aabb_max.y) * 0.5f; /* center y */
      obj->bound_sphere[2] =
          (call->aabb_min.z + call->aabb_max.z) * 0.5f; /* center z */
      float half_diag = sqrtf(dx * dx + dy * dy + dz * dz) * 0.5f;
      obj->bound_sphere[3] = half_diag; /* radius (0 = no bounds) */
    }

    /* Record indirect draw command for GPU culling infrastructure.
     * Even though we currently draw immediately, this populates the
     * input buffer for future GPU-driven indirect dispatch. */
    if (fb->input_draw_cmds_mapped) {
      typedef struct VkDrawIndexedIndirectCmd {
        uint32_t indexCount;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t vertexOffset;
        uint32_t firstInstance;
      } VkDrawIndexedIndirectCmd;
      VkDrawIndexedIndirectCmd *cmd =
          &((VkDrawIndexedIndirectCmd *)fb->input_draw_cmds_mapped)[draw_id];
      cmd->indexCount = call->index_count;
      cmd->instanceCount = 1;
      cmd->firstIndex = 0;
      cmd->vertexOffset = 0;
      cmd->firstInstance = draw_id; /* = gl_InstanceIndex = SSBO index */
    }

    fb->draw_count_this_frame++;

    /* Shadow: store opaque draw info for shadow map rendering in frame_end.
     * Shadow maps are rendered after the main pass (temporal, 1 frame behind).
     * Only store opaque draws — transparent objects don't cast shadows.
     * call->cast_shadows is the per-frame gate: viewport sets it true when
     * any active directional light has cast_shadows=true. */
    if (dev->shadow_pipeline && call->blend_mode == MOP_BLEND_OPAQUE &&
        call->cast_shadows) {
      /* Capture view/proj/light on first draw for cascade computation */
      if (!dev->shadow_data_valid) {
        memcpy(dev->cached_view, call->view.d, 64);
        dev->cached_light_dir = call->light_dir;
        dev->shadow_data_valid = true;
      }
      /* Grow shadow draw array if needed */
      if (dev->shadow_draw_count >= dev->shadow_draw_capacity) {
        uint32_t new_cap =
            dev->shadow_draw_capacity ? dev->shadow_draw_capacity * 2 : 128;
        struct MopVkShadowDraw *new_arr =
            realloc(dev->shadow_draws, new_cap * sizeof(*new_arr));
        if (new_arr) {
          dev->shadow_draws = new_arr;
          dev->shadow_draw_capacity = new_cap;
        }
      }
      if (dev->shadow_draw_count < dev->shadow_draw_capacity) {
        struct MopVkShadowDraw *sd =
            &dev->shadow_draws[dev->shadow_draw_count++];
        sd->vertex_buf = call->vertex_buffer->buffer;
        sd->index_buf = call->index_buffer->buffer;
        sd->index_count = call->index_count;
        memcpy(sd->model, call->model.d, 64);
      }
    }

    /* OIT: defer transparent draws for replay in the OIT pass */
    if (dev->oit_enabled && dev->oit_pipeline &&
        call->blend_mode != MOP_BLEND_OPAQUE) {
      /* Grow deferred draw array if needed */
      if (dev->oit_draw_count >= dev->oit_draw_capacity) {
        uint32_t new_cap =
            dev->oit_draw_capacity ? dev->oit_draw_capacity * 2 : 64;
        struct MopVkDeferredOitDraw *new_arr =
            realloc(dev->oit_draws, new_cap * sizeof(*new_arr));
        if (!new_arr)
          goto oit_skip_defer;
        dev->oit_draws = new_arr;
        dev->oit_draw_capacity = new_cap;
      }
      struct MopVkDeferredOitDraw *dd = &dev->oit_draws[dev->oit_draw_count++];
      dd->vertex_buf = call->vertex_buffer->buffer;
      dd->index_buf = call->index_buffer->buffer;
      dd->index_count = call->index_count;
      dd->draw_id = draw_id;
      memcpy(dd->push_data, call->mvp.d, 64);
      memcpy(dd->push_data + 16, call->model.d, 64);
      return; /* don't record in main pass — will replay in OIT pass */
    }
  oit_skip_defer:

    /* Bind pipeline + push constants (same as non-bindless) */
    vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    float push_data[32];
    memcpy(push_data, call->mvp.d, 64);
    memcpy(push_data + 16, call->model.d, 64);
    vkCmdPushConstants(dev->cmd_buf, dev->bindless_pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, 128, push_data);

    /* Bind the single global bindless descriptor set — no dynamic offset */
    vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->bindless_pipeline_layout, 0, 1,
                            &fb->bindless_ds, 0, NULL);

    /* Mesh shading path (Phase 8): use task->mesh->frag when available.
     * MoltenVK does NOT support VK_EXT_mesh_shader — forward-looking for
     * native Vulkan (Linux/Windows). */
    if (dev->has_mesh_shader && dev->meshlet_pipeline && call->vertex_buffer &&
        call->vertex_buffer->meshlet_count > 0) {
      typedef void(VKAPI_PTR * PFN_DrawMeshTasks)(VkCommandBuffer, uint32_t,
                                                  uint32_t, uint32_t);
      PFN_DrawMeshTasks pfn = (PFN_DrawMeshTasks)dev->pfn_draw_mesh_tasks;
      if (pfn) {
        vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          dev->meshlet_pipeline);
        if (dev->meshlet_desc_set) {
          vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  dev->meshlet_pipeline_layout, 0, 1,
                                  &dev->meshlet_desc_set, 0, NULL);
        }
        uint32_t task_groups = (call->vertex_buffer->meshlet_count + 31) / 32;
        pfn(dev->cmd_buf, task_groups, 1, 1);
        return;
      }
    }

    /* Traditional vertex pipeline draw */
    VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(dev->cmd_buf, 0, 1, &call->vertex_buffer->buffer,
                           &vb_offset);
    vkCmdBindIndexBuffer(dev->cmd_buf, call->index_buffer->buffer, 0,
                         VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(dev->cmd_buf, call->index_count, 1, 0, 0, draw_id);
    return;
  }
fallback_draw:;

  /* ---- Legacy per-draw descriptor set path ---- */

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
  ubo->has_normal_map = call->normal_map ? 1 : 0;
  ubo->has_mr_map = call->metallic_roughness_map ? 1 : 0;
  ubo->has_ao_map = call->ao_map ? 1 : 0;
  ubo->_pad_maps = 0;
  ubo->cam_pos[0] = call->cam_eye.x;
  ubo->cam_pos[1] = call->cam_eye.y;
  ubo->cam_pos[2] = call->cam_eye.z;
  ubo->cam_pos[3] = 0.0f;
  ubo->emissive[0] = call->emissive.x;
  ubo->emissive[1] = call->emissive.y;
  ubo->emissive[2] = call->emissive.z;
  ubo->emissive[3] = 0.0f;

  /* Multi-light: pack lights into shared SSBO (once per frame on first draw).
   * The SSBO holds all scene lights; the UBO only carries the count. */
  {
    uint32_t lc = call->light_count < MOP_VK_MAX_SSBO_LIGHTS
                      ? call->light_count
                      : MOP_VK_MAX_SSBO_LIGHTS;
    ubo->num_lights = (int32_t)lc;

    /* Upload lights to SSBO on first draw of the frame */
    if (fb->light_count_this_frame == 0 && lc > 0 && fb->light_ssbo_mapped) {
      MopVkLight *dst = (MopVkLight *)fb->light_ssbo_mapped;
      for (uint32_t i = 0; i < lc; i++) {
        const MopLight *src = &call->lights[i];
        dst[i].position[0] = src->position.x;
        dst[i].position[1] = src->position.y;
        dst[i].position[2] = src->position.z;
        dst[i].position[3] = (float)src->type;
        dst[i].direction[0] = src->direction.x;
        dst[i].direction[1] = src->direction.y;
        dst[i].direction[2] = src->direction.z;
        dst[i].direction[3] = 0.0f;
        dst[i].color[0] = src->color.r;
        dst[i].color[1] = src->color.g;
        dst[i].color[2] = src->color.b;
        dst[i].color[3] = src->intensity;
        dst[i].params[0] = src->range;
        dst[i].params[1] = src->spot_inner_cos;
        dst[i].params[2] = src->spot_outer_cos;
        dst[i].params[3] = src->active ? 1.0f : 0.0f;
      }
      fb->light_count_this_frame = lc;
    }
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

  /* Exposure is applied once in the tonemap pass — solid shader always 1.0 */
  ubo->exposure = 1.0f;

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

  /* PBR texture map bindings — use white fallback for missing maps */
  VkDescriptorImageInfo normal_map_info = {
      .sampler = dev->default_sampler,
      .imageView = call->normal_map ? call->normal_map->view : dev->white_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo mr_map_info = {
      .sampler = dev->default_sampler,
      .imageView = call->metallic_roughness_map
                       ? call->metallic_roughness_map->view
                       : dev->white_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo ao_map_info = {
      .sampler = dev->default_sampler,
      .imageView = call->ao_map ? call->ao_map->view : dev->white_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  /* Light SSBO descriptor — same buffer for every draw this frame */
  VkDeviceSize light_ssbo_range =
      fb->light_count_this_frame > 0
          ? (VkDeviceSize)fb->light_count_this_frame * sizeof(MopVkLight)
          : sizeof(MopVkLight);
  VkDescriptorBufferInfo light_ssbo_info = {
      .buffer = fb->light_ssbo,
      .offset = 0,
      .range = light_ssbo_range,
  };

  VkWriteDescriptorSet writes[10] = {
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
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 6,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &normal_map_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 7,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &mr_map_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 8,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &ao_map_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 9,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &light_ssbo_info,
      },
  };
  vkUpdateDescriptorSets(dev->device, 10, writes, 0, NULL);

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

  /* Cache projection for SSAO (updated every draw — last one wins) */
  memcpy(dev->cached_projection, call->projection.d, 64);

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
  ubo->has_normal_map = call->normal_map ? 1 : 0;
  ubo->has_mr_map = call->metallic_roughness_map ? 1 : 0;
  ubo->has_ao_map = call->ao_map ? 1 : 0;
  ubo->_pad_maps = 0;
  ubo->cam_pos[0] = call->cam_eye.x;
  ubo->cam_pos[1] = call->cam_eye.y;
  ubo->cam_pos[2] = call->cam_eye.z;
  ubo->cam_pos[3] = 0.0f;
  ubo->emissive[0] = call->emissive.x;
  ubo->emissive[1] = call->emissive.y;
  ubo->emissive[2] = call->emissive.z;
  ubo->emissive[3] = 0.0f;

  /* Light count — actual data is in the shared SSBO (uploaded by first draw) */
  {
    uint32_t lc = call->light_count < MOP_VK_MAX_SSBO_LIGHTS
                      ? call->light_count
                      : MOP_VK_MAX_SSBO_LIGHTS;
    ubo->num_lights = (int32_t)lc;
    if (fb->light_count_this_frame == 0 && lc > 0 && fb->light_ssbo_mapped) {
      MopVkLight *dst = (MopVkLight *)fb->light_ssbo_mapped;
      for (uint32_t i = 0; i < lc; i++) {
        const MopLight *src = &call->lights[i];
        dst[i].position[0] = src->position.x;
        dst[i].position[1] = src->position.y;
        dst[i].position[2] = src->position.z;
        dst[i].position[3] = (float)src->type;
        dst[i].direction[0] = src->direction.x;
        dst[i].direction[1] = src->direction.y;
        dst[i].direction[2] = src->direction.z;
        dst[i].direction[3] = 0.0f;
        dst[i].color[0] = src->color.r;
        dst[i].color[1] = src->color.g;
        dst[i].color[2] = src->color.b;
        dst[i].color[3] = src->intensity;
        dst[i].params[0] = src->range;
        dst[i].params[1] = src->spot_inner_cos;
        dst[i].params[2] = src->spot_outer_cos;
        dst[i].params[3] = src->active ? 1.0f : 0.0f;
      }
      fb->light_count_this_frame = lc;
    }
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

  /* PBR texture maps — white fallback for instanced path */
  VkDescriptorImageInfo normal_map_info = {
      .sampler = dev->default_sampler,
      .imageView = call->normal_map ? call->normal_map->view : dev->white_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo mr_map_info = {
      .sampler = dev->default_sampler,
      .imageView = call->metallic_roughness_map
                       ? call->metallic_roughness_map->view
                       : dev->white_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo ao_map_info = {
      .sampler = dev->default_sampler,
      .imageView = call->ao_map ? call->ao_map->view : dev->white_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  /* Light SSBO descriptor — same buffer for every draw this frame */
  VkDeviceSize light_ssbo_range =
      fb->light_count_this_frame > 0
          ? (VkDeviceSize)fb->light_count_this_frame * sizeof(MopVkLight)
          : sizeof(MopVkLight);
  VkDescriptorBufferInfo light_ssbo_info = {
      .buffer = fb->light_ssbo,
      .offset = 0,
      .range = light_ssbo_range,
  };

  VkWriteDescriptorSet writes[10] = {
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
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 6,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &normal_map_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 7,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &mr_map_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 8,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &ao_map_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 9,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &light_ssbo_info,
      },
  };
  vkUpdateDescriptorSets(dev->device, 10, writes, 0, NULL);

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
 * Hi-Z depth pyramid build (Phase 2C)
 *
 * Copies the depth buffer into Hi-Z level 0, then iteratively downsamples
 * each level via the Hi-Z compute shader (2×2 max reduction).
 * ========================================================================= */

void mop_vk_build_hiz_pyramid(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                              VkCommandBuffer cb) {
  if (!dev->hiz_enabled || fb->hiz_levels == 0 || !fb->hiz_image)
    return;
  if (!fb->depth_copy_image && !fb->depth_image)
    return;

  /* Step 1: Copy depth → Hi-Z level 0.
   * Use depth_copy_image (R32_SFLOAT) if available (MoltenVK workaround),
   * otherwise blit from depth_image (D32_SFLOAT → R32_SFLOAT). */

  /* Transition Hi-Z level 0 to TRANSFER_DST */
  VkImageMemoryBarrier hiz_to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = fb->hiz_image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &hiz_to_dst);

  /* Blit depth_copy (R32_SFLOAT) → Hi-Z level 0 (R32_SFLOAT) */
  VkImage src_image =
      fb->depth_copy_image ? fb->depth_copy_image : fb->depth_image;
  VkImageAspectFlags src_aspect = fb->depth_copy_image
                                      ? VK_IMAGE_ASPECT_COLOR_BIT
                                      : VK_IMAGE_ASPECT_DEPTH_BIT;

  /* Transition source to TRANSFER_SRC */
  VkImageMemoryBarrier src_to_transfer = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = src_image,
      .subresourceRange = {src_aspect, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(cb,
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &src_to_transfer);

  VkImageBlit blit = {
      .srcSubresource = {src_aspect, 0, 0, 1},
      .srcOffsets = {{0, 0, 0},
                     {(int32_t)fb->hiz_width, (int32_t)fb->hiz_height, 1}},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstOffsets = {{0, 0, 0},
                     {(int32_t)fb->hiz_width, (int32_t)fb->hiz_height, 1}},
  };
  vkCmdBlitImage(cb, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 fb->hiz_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                 VK_FILTER_NEAREST);

  /* Transition level 0: TRANSFER_DST → SHADER_READ (for compute read) */
  VkImageMemoryBarrier hiz_l0_to_read = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = fb->hiz_image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                       NULL, 1, &hiz_l0_to_read);

  /* Step 2: Iteratively downsample each mip level */
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dev->hiz_pipeline);

  uint32_t src_w = fb->hiz_width;
  uint32_t src_h = fb->hiz_height;

  for (uint32_t m = 1; m < fb->hiz_levels; m++) {
    uint32_t dst_w = (src_w + 1) / 2;
    uint32_t dst_h = (src_h + 1) / 2;
    if (dst_w == 0)
      dst_w = 1;
    if (dst_h == 0)
      dst_h = 1;

    /* Transition level m to GENERAL for storage image write */
    VkImageMemoryBarrier dst_to_general = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = fb->hiz_image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &dst_to_general);

    /* Allocate descriptor set for this mip pass */
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->hiz_desc_layout,
    };
    VkDescriptorSet ds;
    if (vkAllocateDescriptorSets(dev->device, &ds_ai, &ds) != VK_SUCCESS)
      break;

    /* binding 0: src (level m-1, SHADER_READ_ONLY) */
    VkDescriptorImageInfo src_info = {
        .sampler = dev->hiz_sampler,
        .imageView = fb->hiz_views[m - 1],
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    /* binding 1: dst (level m, GENERAL) */
    VkDescriptorImageInfo dst_info = {
        .imageView = fb->hiz_views[m],
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };

    VkWriteDescriptorSet writes[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = ds,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &src_info},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = ds,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &dst_info},
    };
    vkUpdateDescriptorSets(dev->device, 2, writes, 0, NULL);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            dev->hiz_pipeline_layout, 0, 1, &ds, 0, NULL);

    /* Push constants: src_size + reverse_z */
    struct {
      int32_t src_w, src_h;
      int32_t reverse_z;
    } pc = {(int32_t)src_w, (int32_t)src_h, dev->reverse_z ? 1 : 0};
    vkCmdPushConstants(cb, dev->hiz_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    /* Dispatch */
    uint32_t gx = (dst_w + 7) / 8;
    uint32_t gy = (dst_h + 7) / 8;
    vkCmdDispatch(cb, gx, gy, 1);

    /* Transition level m: GENERAL → SHADER_READ for next iteration */
    VkImageMemoryBarrier dst_to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = fb->hiz_image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &dst_to_read);

    src_w = dst_w;
    src_h = dst_h;
  }
}

/* =========================================================================
 * GPU culling compute dispatch (Phase 1C)
 *
 * Records compute commands into the given command buffer (which may be the
 * primary graphics CB or the async compute CB).  The caller is responsible
 * for command buffer begin/end and queue submission.
 * ========================================================================= */

void mop_vk_dispatch_gpu_cull(MopRhiDevice *dev, MopRhiFramebuffer *fb,
                              VkCommandBuffer cb) {
  if (!dev->gpu_culling_enabled || fb->draw_count_this_frame == 0)
    return;
  if (!fb->input_draw_cmds || !fb->output_draw_cmds || !fb->draw_count_buf)
    return;
  if (!fb->object_ssbo || !fb->globals_ubo)
    return;

  /* Allocate cull descriptor set (6 bindings: obj SSBO, globals UBO,
   * input cmds, output cmds, draw count, Hi-Z pyramid) */
  if (fb->cull_ds == VK_NULL_HANDLE) {
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->cull_desc_layout,
    };
    VkResult r = vkAllocateDescriptorSets(dev->device, &ds_ai, &fb->cull_ds);
    if (r != VK_SUCCESS)
      return;

    /* Update descriptor bindings */
    VkDescriptorBufferInfo obj_bi = {
        .buffer = fb->object_ssbo,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    VkDescriptorBufferInfo globals_bi = {
        .buffer = fb->globals_ubo,
        .offset = 0,
        .range = sizeof(MopVkFrameGlobals),
    };
    VkDescriptorBufferInfo input_bi = {
        .buffer = fb->input_draw_cmds,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    VkDescriptorBufferInfo output_bi = {
        .buffer = fb->output_draw_cmds,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    VkDescriptorBufferInfo count_bi = {
        .buffer = fb->draw_count_buf,
        .offset = 0,
        .range = sizeof(uint32_t),
    };

    /* Hi-Z pyramid texture (binding 5) — use level 0 view with full mip
     * sampling, or white_view as dummy if Hi-Z is not available */
    bool use_hiz = dev->hiz_enabled && fb->hiz_levels > 0 && fb->hiz_views[0];
    VkDescriptorImageInfo hiz_ii = {
        .sampler = use_hiz ? dev->hiz_sampler : dev->default_sampler,
        .imageView = use_hiz ? fb->hiz_views[0] : dev->white_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet writes[6] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = fb->cull_ds,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &obj_bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = fb->cull_ds,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .pBufferInfo = &globals_bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = fb->cull_ds,
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &input_bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = fb->cull_ds,
         .dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &output_bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = fb->cull_ds,
         .dstBinding = 4,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &count_bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = fb->cull_ds,
         .dstBinding = 5,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &hiz_ii},
    };
    vkUpdateDescriptorSets(dev->device, 6, writes, 0, NULL);
  }

  /* Memory barrier: ensure object SSBO and input draw commands are visible
   * to the compute shader (host writes → shader reads) */
  VkMemoryBarrier pre_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
  };
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &pre_barrier,
                       0, NULL, 0, NULL);

  /* Bind compute pipeline and descriptor set */
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dev->cull_pipeline);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          dev->cull_pipeline_layout, 0, 1, &fb->cull_ds, 0,
                          NULL);

  /* Push constants: Hi-Z parameters */
  struct {
    int32_t hiz_enabled;
    int32_t hiz_w, hiz_h;
    int32_t reverse_z;
  } cull_pc = {
      .hiz_enabled = (dev->hiz_enabled && fb->hiz_levels > 0) ? 1 : 0,
      .hiz_w = (int32_t)fb->hiz_width,
      .hiz_h = (int32_t)fb->hiz_height,
      .reverse_z = dev->reverse_z ? 1 : 0,
  };
  vkCmdPushConstants(cb, dev->cull_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(cull_pc), &cull_pc);

  /* Dispatch: ceil(total_draws / 256) workgroups */
  uint32_t groups = (fb->draw_count_this_frame + 255) / 256;
  vkCmdDispatch(cb, groups, 1, 1);

  /* Memory barrier: compute writes → indirect draw reads */
  VkMemoryBarrier post_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask =
          VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
  };
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                       0, 1, &post_barrier, 0, NULL, 0, NULL);
}

/* =========================================================================
 * Async compute submission helper (Phase 1C)
 *
 * Records compute work via the callback, submits on the dedicated compute
 * queue, and signals compute_semaphore.  The graphics queue can wait on
 * compute_semaphore before consuming results.
 *
 * Returns false if async compute is unavailable or submission fails.
 * ========================================================================= */

bool mop_vk_submit_async_compute(MopRhiDevice *dev,
                                 MopVkComputeRecordFn record_fn,
                                 void *user_data) {
  if (!dev->has_async_compute || !record_fn)
    return false;

  /* Wait for previous async compute work to complete */
  vkWaitForFences(dev->device, 1, &dev->compute_fence, VK_TRUE, UINT64_MAX);
  vkResetFences(dev->device, 1, &dev->compute_fence);

  /* Reset and begin compute command buffer */
  vkResetCommandBuffer(dev->compute_cmd_buf, 0);
  VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkResult r = vkBeginCommandBuffer(dev->compute_cmd_buf, &begin);
  if (r != VK_SUCCESS)
    return false;

  /* Let the caller record compute commands */
  record_fn(dev, dev->compute_cmd_buf, user_data);

  r = vkEndCommandBuffer(dev->compute_cmd_buf);
  if (r != VK_SUCCESS)
    return false;

  /* Submit to compute queue, signal semaphore for graphics to wait on */
  VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &dev->compute_cmd_buf,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &dev->compute_semaphore,
  };
  r = vkQueueSubmit(dev->compute_queue, 1, &submit, dev->compute_fence);
  return r == VK_SUCCESS;
}

/* Async compute callback for Hi-Z + GPU cull (Phase 7).
 * Records compute commands into the compute command buffer. */
static void vk_async_hiz_cull_record(MopRhiDevice *dev, VkCommandBuffer cb,
                                     void *user_data) {
  struct {
    MopRhiDevice *dev;
    MopRhiFramebuffer *fb;
  } *data = user_data;
  mop_vk_build_hiz_pyramid(data->dev, data->fb, cb);
  mop_vk_dispatch_gpu_cull(data->dev, data->fb, cb);
}

/* =========================================================================
 * 12. frame_end
 * ========================================================================= */

static void vk_frame_end(MopRhiDevice *dev, MopRhiFramebuffer *fb) {
  /* Finalize total draw count in globals UBO for GPU culling */
  if (fb->globals_ubo_mapped && fb->draw_count_this_frame > 0) {
    MopVkFrameGlobals *g = (MopVkFrameGlobals *)fb->globals_ubo_mapped;
    g->total_draws = fb->draw_count_this_frame;
  }

  /* GPU timestamp: bottom of pipe (before ending render pass for correct
   * timing — we want rendering time, not including readback copies) */
  if (dev->has_timestamp_queries) {
    vkCmdWriteTimestamp(dev->cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        dev->timestamp_pool, 1);
  }

  vkCmdEndRenderPass(dev->cmd_buf);

  /* ---- Copy D32_SFLOAT depth → R32_SFLOAT via staging buffer ----
   * Needed for Hi-Z (R32→R32 blit) and overlay depth testing (separate CB).
   * SSAO samples D32 directly (same CB, no cross-CB issue). */
  mop_vk_pass_timestamp_begin(dev, "depth_copy");
  if (fb->depth_copy_image && fb->readback_depth_buf) {
    /* depth_image is already in TRANSFER_SRC from the render pass */
    VkBufferImageCopy depth_to_buf = {
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                             .layerCount = 1},
        .imageExtent = {(uint32_t)fb->width, (uint32_t)fb->height, 1},
    };
    vkCmdCopyImageToBuffer(dev->cmd_buf, fb->depth_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           fb->readback_depth_buf, 1, &depth_to_buf);

    VkBufferMemoryBarrier buf_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = fb->readback_depth_buf,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(dev->cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &buf_barrier, 0, NULL);

    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_copy_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy buf_to_img = {
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1},
        .imageExtent = {(uint32_t)fb->width, (uint32_t)fb->height, 1},
    };
    vkCmdCopyBufferToImage(
        dev->cmd_buf, fb->readback_depth_buf, fb->depth_copy_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &buf_to_img);
    /* Leave in TRANSFER_SRC so Hi-Z can blit from it */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_copy_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
  }
  mop_vk_pass_timestamp_end(dev);

  /* ---- Hi-Z + GPU cull: async compute when available (Phase 7) ---- */
  if (dev->has_async_compute && dev->gpu_culling_enabled) {
    struct {
      MopRhiDevice *dev;
      MopRhiFramebuffer *fb;
    } async_data = {dev, fb};
    bool async_ok = mop_vk_submit_async_compute(
        dev, (MopVkComputeRecordFn)vk_async_hiz_cull_record, &async_data);
    if (!async_ok) {
      mop_vk_pass_timestamp_begin(dev, "hiz_build");
      mop_vk_build_hiz_pyramid(dev, fb, dev->cmd_buf);
      mop_vk_pass_timestamp_end(dev);
      mop_vk_pass_timestamp_begin(dev, "gpu_cull");
      mop_vk_dispatch_gpu_cull(dev, fb, dev->cmd_buf);
      mop_vk_pass_timestamp_end(dev);
    }
  } else {
    mop_vk_pass_timestamp_begin(dev, "hiz_build");
    mop_vk_build_hiz_pyramid(dev, fb, dev->cmd_buf);
    mop_vk_pass_timestamp_end(dev);
    mop_vk_pass_timestamp_begin(dev, "gpu_cull");
    mop_vk_dispatch_gpu_cull(dev, fb, dev->cmd_buf);
    mop_vk_pass_timestamp_end(dev);
  }
  /* Phase 7: async compute semaphore wait is for future use with same-frame
   * indirect draw. With temporal (1-frame-behind) culling, no wait needed. */

  /* Track indirect draw readiness. Gated on host request — auto-flipping
   * was too eager (the real render path isn't wired to consume it yet). */
  if (dev->indirect_draw_requested && dev->gpu_culling_enabled &&
      fb->output_draw_cmds) {
    dev->indirect_draw_frame_count++;
    if (dev->indirect_draw_frame_count >= 2)
      dev->indirect_draw_enabled = true;
  } else {
    dev->indirect_draw_frame_count = 0;
    dev->indirect_draw_enabled = false;
  }

  /* ---- Indirect draw from temporal GPU cull results (Phase 4) ----
   * Uses previous frame's cull output (1 frame behind = standard practice).
   * This replaces the per-draw vkCmdDrawIndexed calls from the main pass
   * when indirect draw is available. When not yet warmed up, the main pass
   * already drew everything via CPU-driven draws -- this is additive.
   *
   * NOTE: Indirect draw integration is prepared but actual draw replacement
   * requires restructuring the main render pass to defer draws to a separate
   * indirect pass. For now, the infrastructure is wired up for future use:
   * - input_draw_cmds are populated per-draw in vk_draw()
   * - GPU cull shader compacts to output_draw_cmds + draw_count_buf
   * - Stats are read back via last_visible_draws / last_culled_draws
   * Full indirect draw will be activated when the render graph manages
   * pass ordering (Phase 6+). */

  /* MSAA: all resolves happen in-pass via pResolveAttachments and
   * VkSubpassDescriptionDepthStencilResolve.  Render pass transitions
   * the 1x resolve targets to TRANSFER_SRC_OPTIMAL automatically. */

  /* ---- Deferred decal pass: project decal textures onto scene ---- */
  mop_vk_pass_timestamp_begin(dev, "decals");
  if (dev->decal_pipeline && dev->decal_count > 0 && fb->decal_framebuffer &&
      dev->decal_ubo_mapped && fb->globals_ubo_mapped) {

    /* Compute inv_vp from the frame's view-projection matrix */
    MopVkFrameGlobals *fg = (MopVkFrameGlobals *)fb->globals_ubo_mapped;
    MopMat4 vp_for_decal;
    memcpy(vp_for_decal.d, fg->view_proj, 64);
    MopMat4 inv_vp_decal = mop_mat4_inverse(vp_for_decal);

    /* Update the decal UBO: inv_vp(64) + reverse_z(4) + opacity(4) + pad(8) */
    struct {
      float inv_vp[16];
      int32_t reverse_z;
      float opacity; /* updated per-decal via push constant */
      float _pad[2];
    } *decal_ubo = dev->decal_ubo_mapped;
    memcpy(decal_ubo->inv_vp, inv_vp_decal.d, 64);
    decal_ubo->reverse_z = dev->reverse_z ? 1 : 0;
    decal_ubo->opacity = 1.0f; /* default; overridden per-decal */

    /* Transition color_image: TRANSFER_SRC → COLOR_ATTACHMENT */
    mop_vk_transition_image(
        dev->cmd_buf, fb->color_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    /* Transition depth: TRANSFER_SRC → SHADER_READ_ONLY for sampling */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* Begin decal render pass */
    VkRenderPassBeginInfo decal_rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = dev->decal_render_pass,
        .framebuffer = fb->decal_framebuffer,
        .renderArea = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}},
    };
    vkCmdBeginRenderPass(dev->cmd_buf, &decal_rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport decal_vp = {
        .width = (float)fb->width,
        .height = (float)fb->height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D decal_scissor = {
        .extent = {(uint32_t)fb->width, (uint32_t)fb->height},
    };
    vkCmdSetViewport(dev->cmd_buf, 0, 1, &decal_vp);
    vkCmdSetScissor(dev->cmd_buf, 0, 1, &decal_scissor);
    vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      dev->decal_pipeline);

    /* Render each active decal */
    for (uint32_t di = 0; di < dev->decal_count; di++) {
      if (!dev->decals[di].active)
        continue;

      /* Update UBO opacity for this decal */
      decal_ubo->opacity = dev->decals[di].opacity;

      /* Allocate descriptor set for this decal */
      VkDescriptorSetAllocateInfo dds_ai = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          .descriptorPool = dev->desc_pool,
          .descriptorSetCount = 1,
          .pSetLayouts = &dev->decal_desc_layout,
      };
      VkDescriptorSet dds;
      if (vkAllocateDescriptorSets(dev->device, &dds_ai, &dds) != VK_SUCCESS)
        continue;

      /* Binding 0: depth buffer */
      VkDescriptorImageInfo depth_di = {
          .sampler = dev->clamp_sampler,
          .imageView = fb->depth_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      /* Binding 1: decal texture (or white fallback) */
      VkImageView tex_view = dev->white_view; /* fallback */
      if (dev->decals[di].texture_idx >= 0 &&
          dev->decals[di].texture_idx < (int32_t)dev->texture_registry_count) {
        tex_view = dev->texture_registry[dev->decals[di].texture_idx];
      }
      VkDescriptorImageInfo tex_di = {
          .sampler = dev->clamp_sampler,
          .imageView = tex_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      /* Binding 2: decal UBO */
      VkDescriptorBufferInfo ubo_di = {
          .buffer = dev->decal_ubo,
          .offset = 0,
          .range = 80,
      };
      VkWriteDescriptorSet dwrites[3] = {
          {
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet = dds,
              .dstBinding = 0,
              .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .pImageInfo = &depth_di,
          },
          {
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet = dds,
              .dstBinding = 1,
              .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .pImageInfo = &tex_di,
          },
          {
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet = dds,
              .dstBinding = 2,
              .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
              .pBufferInfo = &ubo_di,
          },
      };
      vkUpdateDescriptorSets(dev->device, 3, dwrites, 0, NULL);
      vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              dev->decal_pipeline_layout, 0, 1, &dds, 0, NULL);

      /* Push constants: mat4 mvp (VP * decal_model) + mat4 inv_decal */
      struct {
        float mvp[16];
        float inv_decal[16];
      } decal_pc;
      MopMat4 decal_model;
      memcpy(decal_model.d, dev->decals[di].model, 64);
      MopMat4 decal_mvp = mop_mat4_multiply(vp_for_decal, decal_model);
      memcpy(decal_pc.mvp, decal_mvp.d, 64);
      memcpy(decal_pc.inv_decal, dev->decals[di].inv_model, 64);
      vkCmdPushConstants(dev->cmd_buf, dev->decal_pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, 128, &decal_pc);

      /* Draw 36-vertex procedural cube (no vertex buffer needed) */
      vkCmdDraw(dev->cmd_buf, 36, 1, 0, 0);
    }

    vkCmdEndRenderPass(dev->cmd_buf);

    /* Restore depth: SHADER_READ_ONLY → TRANSFER_SRC for downstream passes */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* color_image already in TRANSFER_SRC from render pass finalLayout */
  }
  mop_vk_pass_timestamp_end(dev);

  /* ---- Volumetric fog pass: raymarch through scene, blend onto color ---- */
  mop_vk_pass_timestamp_begin(dev, "volumetric");
  if (dev->volumetric_enabled && dev->volumetric_pipeline &&
      fb->volumetric_framebuffer && fb->volumetric_ubo_mapped &&
      fb->globals_ubo_mapped) {

    /* Compute inv_vp for depth reconstruction */
    MopVkFrameGlobals *vfg = (MopVkFrameGlobals *)fb->globals_ubo_mapped;
    MopMat4 vp_vol;
    memcpy(vp_vol.d, vfg->view_proj, 64);
    MopMat4 inv_vp_vol = mop_mat4_inverse(vp_vol);

    /* Fill UBO: inv_vp(64) + cam_pos(16) + fog_params(16) +
     * anisotropy(4) + num_lights(4) + num_steps(4) + reverse_z(4) = 112 */
    struct {
      float inv_vp[16];
      float cam_pos[4];
      float fog_params[4]; /* rgb + density */
      float anisotropy;
      int32_t num_lights;
      int32_t num_steps;
      int32_t reverse_z;
    } *vol_ubo = fb->volumetric_ubo_mapped;
    memcpy(vol_ubo->inv_vp, inv_vp_vol.d, 64);
    vol_ubo->cam_pos[0] = vfg->cam_pos[0];
    vol_ubo->cam_pos[1] = vfg->cam_pos[1];
    vol_ubo->cam_pos[2] = vfg->cam_pos[2];
    vol_ubo->cam_pos[3] = 0.0f;
    vol_ubo->fog_params[0] = dev->volumetric_color[0];
    vol_ubo->fog_params[1] = dev->volumetric_color[1];
    vol_ubo->fog_params[2] = dev->volumetric_color[2];
    vol_ubo->fog_params[3] = dev->volumetric_density;
    vol_ubo->anisotropy = dev->volumetric_anisotropy;
    vol_ubo->num_lights = (int32_t)fb->light_count_this_frame;
    vol_ubo->num_steps = dev->volumetric_steps;
    vol_ubo->reverse_z = dev->reverse_z ? 1 : 0;

    /* Transition color: TRANSFER_SRC → COLOR_ATTACHMENT */
    mop_vk_transition_image(
        dev->cmd_buf, fb->color_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    /* Transition depth: TRANSFER_SRC → SHADER_READ_ONLY for sampling */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* Begin render pass */
    VkRenderPassBeginInfo vol_rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = dev->volumetric_render_pass,
        .framebuffer = fb->volumetric_framebuffer,
        .renderArea = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}},
    };
    vkCmdBeginRenderPass(dev->cmd_buf, &vol_rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vol_vp = {
        .width = (float)fb->width,
        .height = (float)fb->height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D vol_sc = {
        .extent = {(uint32_t)fb->width, (uint32_t)fb->height},
    };
    vkCmdSetViewport(dev->cmd_buf, 0, 1, &vol_vp);
    vkCmdSetScissor(dev->cmd_buf, 0, 1, &vol_sc);
    vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      dev->volumetric_pipeline);

    /* Allocate + bind descriptor set */
    VkDescriptorSetAllocateInfo vol_ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->volumetric_desc_layout,
    };
    VkDescriptorSet vol_ds;
    if (vkAllocateDescriptorSets(dev->device, &vol_ds_ai, &vol_ds) ==
        VK_SUCCESS) {
      /* Binding 0: depth buffer */
      VkDescriptorImageInfo vol_depth = {
          .sampler = dev->clamp_sampler,
          .imageView = fb->depth_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      /* Binding 1: light SSBO */
      VkDescriptorBufferInfo vol_light = {
          .buffer = fb->light_ssbo,
          .offset = 0,
          .range = (fb->light_count_this_frame > 0 ? fb->light_count_this_frame
                                                   : 1) *
                   sizeof(MopVkLight),
      };
      /* Binding 2: volumetric UBO */
      VkDescriptorBufferInfo vol_ubo_info = {
          .buffer = fb->volumetric_ubo,
          .offset = 0,
          .range = 112,
      };
      VkWriteDescriptorSet vol_writes[3] = {
          {
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet = vol_ds,
              .dstBinding = 0,
              .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .pImageInfo = &vol_depth,
          },
          {
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet = vol_ds,
              .dstBinding = 1,
              .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              .pBufferInfo = &vol_light,
          },
          {
              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              .dstSet = vol_ds,
              .dstBinding = 2,
              .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
              .pBufferInfo = &vol_ubo_info,
          },
      };
      vkUpdateDescriptorSets(dev->device, 3, vol_writes, 0, NULL);
      vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              dev->volumetric_pipeline_layout, 0, 1, &vol_ds, 0,
                              NULL);

      /* Fullscreen triangle */
      vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0);
    }

    vkCmdEndRenderPass(dev->cmd_buf);

    /* Restore depth: SHADER_READ_ONLY → TRANSFER_SRC */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* color_image is now in TRANSFER_SRC from render pass finalLayout */
  }
  mop_vk_pass_timestamp_end(dev);

  /* ---- SSAO pass: depth → raw AO → blurred AO ---- */
  mop_vk_pass_timestamp_begin(dev, "ssao");
  if (dev->ssao_enabled && dev->ssao_pipeline && dev->ssao_blur_pipeline &&
      fb->ssao_fb && fb->ssao_blur_fb) {
    /* SSAO samples D32_SFLOAT depth directly (same CB — no cross-CB issue).
     * Transition depth from TRANSFER_SRC → SHADER_READ_ONLY for sampling */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* Also transition depth_copy (R32) to SHADER_READ_ONLY — the overlay
     * pass (separate CB) will sample it for depth testing UI elements. */
    if (fb->depth_copy_image) {
      mop_vk_transition_image(
          dev->cmd_buf, fb->depth_copy_image, VK_IMAGE_ASPECT_COLOR_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    /* Transition SSAO images to COLOR_ATTACHMENT */
    mop_vk_transition_image(
        dev->cmd_buf, fb->ssao_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    mop_vk_transition_image(
        dev->cmd_buf, fb->ssao_blur_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    /* Step 1: Raw SSAO — sample depth buffer with hemisphere kernel */
    {
      VkDescriptorSetAllocateInfo ds_ai = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          .descriptorPool = dev->desc_pool,
          .descriptorSetCount = 1,
          .pSetLayouts = &dev->ssao_desc_layout,
      };
      VkDescriptorSet sds;
      if (vkAllocateDescriptorSets(dev->device, &ds_ai, &sds) == VK_SUCCESS) {
        /* Sample D32_SFLOAT depth directly — same CB, no cross-CB issue */
        VkDescriptorImageInfo depth_info = {
            .sampler = dev->clamp_sampler,
            .imageView = fb->depth_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkDescriptorImageInfo noise_info = {
            .sampler = dev->default_sampler, /* noise tiles — keep REPEAT */
            .imageView = dev->ssao_noise_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkDescriptorBufferInfo kernel_info = {
            .buffer = dev->ssao_kernel_ubo,
            .offset = 0,
            .range = 64 * 4 * sizeof(float),
        };
        VkWriteDescriptorSet ssao_writes[3] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sds,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &depth_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sds,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &noise_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sds,
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo = &kernel_info,
            },
        };
        vkUpdateDescriptorSets(dev->device, 3, ssao_writes, 0, NULL);

        VkClearValue clear = {.color = {{1.0f, 0, 0, 0}}};
        VkRenderPassBeginInfo rp_bi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = dev->ssao_render_pass,
            .framebuffer = fb->ssao_fb,
            .renderArea = {.extent = {(uint32_t)fb->width,
                                      (uint32_t)fb->height}},
            .clearValueCount = 1,
            .pClearValues = &clear,
        };
        vkCmdBeginRenderPass(dev->cmd_buf, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

        /* Y-flipped viewport — must match tonemap's orientation so that
         * the fullscreen vertex shader's UV flip produces correct UVs.
         * Without this, the SSAO output is vertically inverted. */
        VkViewport vp = {
            .y = (float)fb->height,
            .width = (float)fb->width,
            .height = -(float)fb->height,
            .maxDepth = 1.0f,
        };
        VkRect2D sc = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}};
        vkCmdSetViewport(dev->cmd_buf, 0, 1, &vp);
        vkCmdSetScissor(dev->cmd_buf, 0, 1, &sc);

        /* Use GTAO when available, fall back to classic SSAO */
        bool use_gtao = dev->gtao_available && dev->gtao_pipeline;
        vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          use_gtao ? dev->gtao_pipeline : dev->ssao_pipeline);
        vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                dev->ssao_pipeline_layout, 0, 1, &sds, 0, NULL);

        /* Push constants: projection + parameters (96 bytes).
         * GTAO and SSAO share the same 96-byte push constant range
         * but interpret fields 64..95 differently:
         *   SSAO:  radius(4) + bias(4) + kernel_size(4) + reverse_z(4) +
         *          noise_scale(8) + pad(8)
         *   GTAO:  radius(4) + intensity(4) + num_steps(4) + reverse_z(4) +
         *          noise_scale(8) + inv_resolution(8)
         */
        struct {
          float projection[16];
          float radius;
          float param1;   /* SSAO: bias, GTAO: intensity */
          int32_t param2; /* SSAO: kernel_size, GTAO: num_steps */
          int32_t reverse_z;
          float noise_scale[2];
          float param3[2]; /* SSAO: pad, GTAO: inv_resolution */
        } ssao_pc;
        memcpy(ssao_pc.projection, dev->cached_projection, 64);
        ssao_pc.reverse_z = dev->reverse_z ? 1 : 0;
        ssao_pc.noise_scale[0] = (float)fb->width / 4.0f;
        ssao_pc.noise_scale[1] = (float)fb->height / 4.0f;
        if (use_gtao) {
          ssao_pc.radius = 0.3f; /* world-space AO radius */
          ssao_pc.param1 =
              0.6f;           /* intensity (reduced to minimize edge halos) */
          ssao_pc.param2 = 6; /* num_steps per direction */
          ssao_pc.param3[0] = 1.0f / (float)fb->width;  /* inv_resolution.x */
          ssao_pc.param3[1] = 1.0f / (float)fb->height; /* inv_resolution.y */
        } else {
          ssao_pc.radius = 0.35f;
          ssao_pc.param1 = 0.04f;   /* bias */
          ssao_pc.param2 = 32;      /* kernel_size */
          ssao_pc.param3[0] = 0.0f; /* pad */
          ssao_pc.param3[1] = 0.0f;
        }

        vkCmdPushConstants(dev->cmd_buf, dev->ssao_pipeline_layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ssao_pc),
                           &ssao_pc);

        vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0);
        vkCmdEndRenderPass(dev->cmd_buf);
      }
    }

    /* Step 2: Blur the raw SSAO output */
    {
      /* ssao_image is now SHADER_READ_ONLY (from render pass final layout) */
      VkDescriptorSetAllocateInfo ds_ai = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          .descriptorPool = dev->desc_pool,
          .descriptorSetCount = 1,
          .pSetLayouts = &dev->ssao_desc_layout,
      };
      VkDescriptorSet bds;
      if (vkAllocateDescriptorSets(dev->device, &ds_ai, &bds) == VK_SUCCESS) {
        VkDescriptorImageInfo ssao_info = {
            .sampler = dev->clamp_sampler,
            .imageView = fb->ssao_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkDescriptorImageInfo depth_for_blur = {
            .sampler = dev->clamp_sampler,
            .imageView = fb->depth_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet blur_writes[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = bds,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &ssao_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = bds,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &depth_for_blur,
            },
        };
        vkUpdateDescriptorSets(dev->device, 2, blur_writes, 0, NULL);

        VkClearValue clear = {.color = {{1.0f, 0, 0, 0}}};
        VkRenderPassBeginInfo rp_bi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = dev->ssao_render_pass,
            .framebuffer = fb->ssao_blur_fb,
            .renderArea = {.extent = {(uint32_t)fb->width,
                                      (uint32_t)fb->height}},
            .clearValueCount = 1,
            .pClearValues = &clear,
        };
        vkCmdBeginRenderPass(dev->cmd_buf, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp = {
            .y = (float)fb->height,
            .width = (float)fb->width,
            .height = -(float)fb->height,
            .maxDepth = 1.0f,
        };
        VkRect2D sc = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}};
        vkCmdSetViewport(dev->cmd_buf, 0, 1, &vp);
        vkCmdSetScissor(dev->cmd_buf, 0, 1, &sc);

        vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          (dev->gtao_available && dev->gtao_blur_pipeline)
                              ? dev->gtao_blur_pipeline
                              : dev->ssao_blur_pipeline);
        vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                dev->ssao_pipeline_layout, 0, 1, &bds, 0, NULL);

        float blur_pc[2] = {1.0f / (float)fb->width, 1.0f / (float)fb->height};
        vkCmdPushConstants(dev->cmd_buf, dev->ssao_pipeline_layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, blur_pc);

        vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0);
        vkCmdEndRenderPass(dev->cmd_buf);
      }
    }

    /* ssao_blur_image is now SHADER_READ_ONLY — ready for tonemap sampling.
     * Transition depth back to TRANSFER_SRC for readback later. */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
  }
  mop_vk_pass_timestamp_end(dev);

  /* ---- OIT pass: accumulate deferred transparent draws, then composite ----
   */
  mop_vk_pass_timestamp_begin(dev, "oit");
  if (dev->oit_enabled && dev->oit_pipeline && dev->oit_composite_pipeline &&
      fb->oit_framebuffer && fb->oit_composite_framebuffer &&
      dev->oit_draw_count > 0 && fb->bindless_ds) {

    /* Transition depth: TRANSFER_SRC → DEPTH_STENCIL_READ_ONLY */
    mop_vk_transition_image(dev->cmd_buf, fb->depth_image,
                            VK_IMAGE_ASPECT_DEPTH_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                            VK_ACCESS_TRANSFER_READ_BIT,
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

    /* Transition OIT images to COLOR_ATTACHMENT for render pass */
    mop_vk_transition_image(
        dev->cmd_buf, fb->oit_accum_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    mop_vk_transition_image(
        dev->cmd_buf, fb->oit_revealage_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    /* Begin OIT accumulation render pass */
    VkClearValue oit_clears[3] = {
        {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}}, /* accum = 0 */
        {.color = {{1.0f, 0.0f, 0.0f, 0.0f}}}, /* revealage = 1 */
        {.depthStencil = {0}},                 /* depth: unused (LOAD) */
    };
    VkRenderPassBeginInfo oit_rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = dev->oit_render_pass,
        .framebuffer = fb->oit_framebuffer,
        .renderArea = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}},
        .clearValueCount = 3,
        .pClearValues = oit_clears,
    };
    vkCmdBeginRenderPass(dev->cmd_buf, &oit_rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp_oit = {
        .y = (float)fb->height,
        .width = (float)fb->width,
        .height = -(float)fb->height,
        .maxDepth = 1.0f,
    };
    VkRect2D sc_oit = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}};
    vkCmdSetViewport(dev->cmd_buf, 0, 1, &vp_oit);
    vkCmdSetScissor(dev->cmd_buf, 0, 1, &sc_oit);

    vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      dev->oit_pipeline);
    vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->bindless_pipeline_layout, 0, 1,
                            &fb->bindless_ds, 0, NULL);

    /* Replay all deferred transparent draws */
    for (uint32_t i = 0; i < dev->oit_draw_count; i++) {
      struct MopVkDeferredOitDraw *dd = &dev->oit_draws[i];
      vkCmdPushConstants(dev->cmd_buf, dev->bindless_pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT, 0, 128, dd->push_data);
      VkDeviceSize vb_off = 0;
      vkCmdBindVertexBuffers(dev->cmd_buf, 0, 1, &dd->vertex_buf, &vb_off);
      vkCmdBindIndexBuffer(dev->cmd_buf, dd->index_buf, 0,
                           VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(dev->cmd_buf, dd->index_count, 1, 0, 0, dd->draw_id);
    }

    vkCmdEndRenderPass(dev->cmd_buf);

    /* Transition OIT images to SHADER_READ_ONLY for composite sampling */
    mop_vk_transition_image(
        dev->cmd_buf, fb->oit_accum_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    mop_vk_transition_image(
        dev->cmd_buf, fb->oit_revealage_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* Transition color_image: TRANSFER_SRC → COLOR_ATTACHMENT for compositing
     */
    mop_vk_transition_image(
        dev->cmd_buf, fb->color_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    /* Allocate descriptor set for composite (accum + revealage) */
    VkDescriptorSetAllocateInfo oit_ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->oit_composite_desc_layout,
    };
    VkDescriptorSet oit_ds;
    VkResult oit_r = vkAllocateDescriptorSets(dev->device, &oit_ds_ai, &oit_ds);
    if (oit_r == VK_SUCCESS) {
      VkDescriptorImageInfo oit_imgs[2] = {
          {.sampler = dev->clamp_sampler,
           .imageView = fb->oit_accum_view,
           .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {.sampler = dev->clamp_sampler,
           .imageView = fb->oit_revealage_view,
           .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      };
      VkWriteDescriptorSet oit_writes[2];
      for (int b = 0; b < 2; b++) {
        oit_writes[b] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = oit_ds,
            .dstBinding = (uint32_t)b,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &oit_imgs[b],
        };
      }
      vkUpdateDescriptorSets(dev->device, 2, oit_writes, 0, NULL);

      /* Begin OIT composite render pass */
      VkRenderPassBeginInfo comp_rp = {
          .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
          .renderPass = dev->oit_composite_render_pass,
          .framebuffer = fb->oit_composite_framebuffer,
          .renderArea = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}},
      };
      vkCmdBeginRenderPass(dev->cmd_buf, &comp_rp, VK_SUBPASS_CONTENTS_INLINE);

      VkViewport vp_comp = {
          .y = (float)fb->height,
          .width = (float)fb->width,
          .height = -(float)fb->height,
          .maxDepth = 1.0f,
      };
      VkRect2D sc_comp = {
          .extent = {(uint32_t)fb->width, (uint32_t)fb->height}};
      vkCmdSetViewport(dev->cmd_buf, 0, 1, &vp_comp);
      vkCmdSetScissor(dev->cmd_buf, 0, 1, &sc_comp);

      vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dev->oit_composite_pipeline);
      vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              dev->oit_composite_pipeline_layout, 0, 1, &oit_ds,
                              0, NULL);
      vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0); /* fullscreen triangle */
      vkCmdEndRenderPass(dev->cmd_buf);
      /* color_image now in TRANSFER_SRC (render pass finalLayout) */
    }

    /* Transition depth back: DEPTH_STENCIL_READ_ONLY → TRANSFER_SRC */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
  }
  mop_vk_pass_timestamp_end(dev);

  /* ---- Bloom pass: extract bright → downsample → upsample chain ---- */
  mop_vk_pass_timestamp_begin(dev, "bloom");
  if (dev->bloom_enabled && dev->bloom_extract_pipeline &&
      dev->bloom_blur_pipeline && fb->bloom_fbs[0]) {
    /* Transition HDR color to SHADER_READ_ONLY for extract sampling.
     * Use BOTTOM_OF_PIPE as src stage to ensure ALL render pass work
     * (including TBDR tile stores) is fully complete before we read. */
    mop_vk_transition_image(
        dev->cmd_buf, fb->color_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* Transition all bloom images to COLOR_ATTACHMENT_OPTIMAL for use */
    for (int i = 0; i < MOP_VK_BLOOM_LEVELS; i++) {
      if (!fb->bloom_images[i])
        break;
      mop_vk_transition_image(
          dev->cmd_buf, fb->bloom_images[i], VK_IMAGE_ASPECT_COLOR_BIT,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    /* Compute bloom mip sizes */
    uint32_t bloom_widths[MOP_VK_BLOOM_LEVELS];
    uint32_t bloom_heights[MOP_VK_BLOOM_LEVELS];
    {
      uint32_t bw = (uint32_t)fb->width / 2;
      uint32_t bh = (uint32_t)fb->height / 2;
      for (int i = 0; i < MOP_VK_BLOOM_LEVELS; i++) {
        bloom_widths[i] = bw > 0 ? bw : 1;
        bloom_heights[i] = bh > 0 ? bh : 1;
        bw /= 2;
        bh /= 2;
      }
    }

    /* Step 1: Extract bright pixels from HDR → bloom[0] */
    {
      VkDescriptorSetAllocateInfo ds_ai = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          .descriptorPool = dev->desc_pool,
          .descriptorSetCount = 1,
          .pSetLayouts = &dev->bloom_desc_layout,
      };
      VkDescriptorSet bds;
      if (vkAllocateDescriptorSets(dev->device, &ds_ai, &bds) == VK_SUCCESS) {
        VkDescriptorImageInfo img_info = {
            .sampler = dev->clamp_sampler,
            .imageView = fb->color_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = bds,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &img_info,
        };
        vkUpdateDescriptorSets(dev->device, 1, &write, 0, NULL);

        VkClearValue clear = {.color = {{0, 0, 0, 0}}};
        VkRenderPassBeginInfo rp_bi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = dev->bloom_render_pass,
            .framebuffer = fb->bloom_fbs[0],
            .renderArea = {.extent = {bloom_widths[0], bloom_heights[0]}},
            .clearValueCount = 1,
            .pClearValues = &clear,
        };
        vkCmdBeginRenderPass(dev->cmd_buf, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp = {
            .y = (float)bloom_heights[0],
            .width = (float)bloom_widths[0],
            .height = -(float)bloom_heights[0],
            .maxDepth = 1.0f,
        };
        VkRect2D sc = {.extent = {bloom_widths[0], bloom_heights[0]}};
        vkCmdSetViewport(dev->cmd_buf, 0, 1, &vp);
        vkCmdSetScissor(dev->cmd_buf, 0, 1, &sc);

        vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          dev->bloom_extract_pipeline);
        vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                dev->bloom_pipeline_layout, 0, 1, &bds, 0,
                                NULL);

        /* Push extract constants: threshold + soft_knee (padded to 16) */
        float extract_pc[4] = {dev->bloom_threshold,
                               dev->bloom_threshold * 0.5f, 0.0f, 0.0f};
        vkCmdPushConstants(dev->cmd_buf, dev->bloom_pipeline_layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, extract_pc);

        vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0);
        vkCmdEndRenderPass(dev->cmd_buf);

        /* Per-image barrier after extract: flush bloom[0] tile caches */
        {
          VkImageMemoryBarrier bar = {
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
              .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = fb->bloom_images[0],
              .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .levelCount = 1,
                                   .layerCount = 1},
          };
          vkCmdPipelineBarrier(dev->cmd_buf,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                               NULL, 0, NULL, 1, &bar);
        }
      }
    }

    /* Step 2: Downsample chain — blur bloom[i-1] → bloom[i].
     * All levels are rendered to keep image layouts valid.  The tonemap
     * shader only samples levels 0-1; deeper levels have MoltenVK TBDR
     * corruption but are not read. */
    for (int i = 1; i < MOP_VK_BLOOM_LEVELS; i++) {
      if (!fb->bloom_fbs[i])
        break;

      /* Two barriers: (1) source bloom[i-1] write→read, (2) dest bloom[i]
       * transition to COLOR_ATTACHMENT.  Both per-image so MoltenVK emits
       * proper Metal texture barriers for TBDR tile flushing. */
      VkImageMemoryBarrier ds_bars[2] = {
          {
              /* Source: ensure previous render pass writes are visible */
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
              .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = fb->bloom_images[i - 1],
              .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .levelCount = 1,
                                   .layerCount = 1},
          },
          {
              /* Dest: transition to COLOR_ATTACHMENT right before use.
               * oldLayout=UNDEFINED discards stale contents (fine with
               * LOAD_OP_CLEAR). Placing this barrier adjacent to the
               * render pass avoids MoltenVK losing track of the layout
               * across intervening render passes. */
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
              .srcAccessMask = 0,
              .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
              .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = fb->bloom_images[i],
              .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .levelCount = 1,
                                   .layerCount = 1},
          },
      };
      vkCmdPipelineBarrier(dev->cmd_buf,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, 0, NULL, 0, NULL, 2, ds_bars);

      VkDescriptorSetAllocateInfo ds_ai = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          .descriptorPool = dev->desc_pool,
          .descriptorSetCount = 1,
          .pSetLayouts = &dev->bloom_desc_layout,
      };
      VkDescriptorSet bds;
      if (vkAllocateDescriptorSets(dev->device, &ds_ai, &bds) != VK_SUCCESS)
        break;

      VkDescriptorImageInfo img_info = {
          .sampler = dev->clamp_sampler,
          .imageView = fb->bloom_views[i - 1],
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      VkWriteDescriptorSet write = {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = bds,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &img_info,
      };
      vkUpdateDescriptorSets(dev->device, 1, &write, 0, NULL);

      VkClearValue clear = {.color = {{0, 0, 0, 0}}};
      VkRenderPassBeginInfo rp_bi = {
          .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
          .renderPass = dev->bloom_render_pass,
          .framebuffer = fb->bloom_fbs[i],
          .renderArea = {.extent = {bloom_widths[i], bloom_heights[i]}},
          .clearValueCount = 1,
          .pClearValues = &clear,
      };
      vkCmdBeginRenderPass(dev->cmd_buf, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

      VkViewport vp = {
          .y = (float)bloom_heights[i],
          .width = (float)bloom_widths[i],
          .height = -(float)bloom_heights[i],
          .maxDepth = 1.0f,
      };
      VkRect2D sc = {.extent = {bloom_widths[i], bloom_heights[i]}};
      vkCmdSetViewport(dev->cmd_buf, 0, 1, &vp);
      vkCmdSetScissor(dev->cmd_buf, 0, 1, &sc);

      vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dev->bloom_blur_pipeline);
      vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              dev->bloom_pipeline_layout, 0, 1, &bds, 0, NULL);

      /* Push blur constants: texel_size (of source), weight = 1.0 */
      float blur_pc[4] = {1.0f / (float)bloom_widths[i - 1],
                          1.0f / (float)bloom_heights[i - 1], 1.0f, 0.0f};
      vkCmdPushConstants(dev->cmd_buf, dev->bloom_pipeline_layout,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, blur_pc);

      vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0);
      vkCmdEndRenderPass(dev->cmd_buf);
    }

    /* No upsample chain — bloom levels are combined directly in the tonemap
     * shader (multi-sample approach).  This avoids TBDR synchronization
     * issues on MoltenVK/Apple GPUs where rendering to intermediate
     * bloom_up images corrupts texture reads. All bloom[] images are
     * already in SHADER_READ_ONLY_OPTIMAL from the downsample chain's
     * render pass finalLayout. */
  }
  mop_vk_pass_timestamp_end(dev);

  /* ---- SSR pass: depth + HDR color → ssr_image ---- */
  mop_vk_pass_timestamp_begin(dev, "ssr");
  bool ssr_ran = false;
  if (dev->ssr_enabled && dev->ssr_pipeline && fb->ssr_framebuffer) {
    /* Transition depth to SHADER_READ_ONLY for SSR sampling.
     * After SSAO it's in TRANSFER_SRC; if SSAO didn't run it's still
     * TRANSFER_SRC from the main render pass end. */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* Transition HDR color to SHADER_READ_ONLY if bloom didn't already */
    bool bloom_ran_for_ssr =
        dev->bloom_enabled && dev->bloom_extract_pipeline && fb->bloom_fbs[0];
    if (!bloom_ran_for_ssr) {
      mop_vk_transition_image(
          dev->cmd_buf, fb->color_image, VK_IMAGE_ASPECT_COLOR_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    /* Allocate descriptor set: depth + HDR color */
    VkDescriptorSetAllocateInfo ssr_ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->ssr_desc_layout,
    };
    VkDescriptorSet ssr_ds;
    if (vkAllocateDescriptorSets(dev->device, &ssr_ds_ai, &ssr_ds) ==
        VK_SUCCESS) {
      VkDescriptorImageInfo ssr_img_infos[2] = {
          {
              /* Binding 0: depth buffer */
              .sampler = dev->clamp_sampler,
              .imageView = fb->depth_view,
              .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          },
          {
              /* Binding 1: HDR color buffer */
              .sampler = dev->clamp_sampler,
              .imageView = fb->color_view,
              .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          },
      };
      VkWriteDescriptorSet ssr_writes[2];
      for (int b = 0; b < 2; b++) {
        ssr_writes[b] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ssr_ds,
            .dstBinding = (uint32_t)b,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &ssr_img_infos[b],
        };
      }
      vkUpdateDescriptorSets(dev->device, 2, ssr_writes, 0, NULL);

      /* Begin SSR render pass */
      VkClearValue clear = {.color = {{0, 0, 0, 0}}};
      VkRenderPassBeginInfo rp_bi = {
          .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
          .renderPass = dev->ssr_render_pass,
          .framebuffer = fb->ssr_framebuffer,
          .renderArea = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}},
          .clearValueCount = 1,
          .pClearValues = &clear,
      };
      vkCmdBeginRenderPass(dev->cmd_buf, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

      VkViewport vp = {
          .y = (float)fb->height,
          .width = (float)fb->width,
          .height = -(float)fb->height,
          .maxDepth = 1.0f,
      };
      VkRect2D sc = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}};
      vkCmdSetViewport(dev->cmd_buf, 0, 1, &vp);
      vkCmdSetScissor(dev->cmd_buf, 0, 1, &sc);

      vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dev->ssr_pipeline);
      vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              dev->ssr_pipeline_layout, 0, 1, &ssr_ds, 0, NULL);

      /* Push constants: projection + inv_projection + params (156 bytes) */
      struct {
        float projection[16];
        float inv_projection[16];
        float inv_resolution[2];
        int32_t reverse_z;
        float max_distance;
        float thickness;
        float intensity;
        float _pad;
      } ssr_pc;
      memcpy(ssr_pc.projection, dev->cached_projection, 64);
      /* Compute inverse projection */
      {
        MopMat4 proj;
        memcpy(proj.d, dev->cached_projection, 64);
        MopMat4 inv = mop_mat4_inverse(proj);
        memcpy(ssr_pc.inv_projection, inv.d, 64);
      }
      ssr_pc.inv_resolution[0] = 1.0f / (float)fb->width;
      ssr_pc.inv_resolution[1] = 1.0f / (float)fb->height;
      ssr_pc.reverse_z = dev->reverse_z ? 1 : 0;
      ssr_pc.max_distance = 50.0f;
      ssr_pc.thickness = 0.3f;
      ssr_pc.intensity = dev->ssr_intensity;
      ssr_pc._pad = 0.0f;

      vkCmdPushConstants(dev->cmd_buf, dev->ssr_pipeline_layout,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ssr_pc),
                         &ssr_pc);
      vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0);
      vkCmdEndRenderPass(dev->cmd_buf);
      ssr_ran = true;
    }

    /* Transition depth back to TRANSFER_SRC for readback */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
  }
  mop_vk_pass_timestamp_end(dev);

  /* ---- HDR Tonemap pass: color_image (HDR) → ldr_color_image (LDR) ---- */
  mop_vk_pass_timestamp_begin(dev, "tonemap");
  if (dev->tonemap_enabled && fb->tonemap_framebuffer) {
    /* Transition HDR color to SHADER_READ_ONLY if bloom/SSR didn't already */
    bool bloom_ran =
        dev->bloom_enabled && dev->bloom_extract_pipeline && fb->bloom_fbs[0];
    bool color_already_readable = bloom_ran || ssr_ran;
    if (!color_already_readable) {
      mop_vk_transition_image(
          dev->cmd_buf, fb->color_image, VK_IMAGE_ASPECT_COLOR_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

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
      /* Update descriptors: HDR + bloom[0..4] + SSAO + SSR (8 bindings).
       * Multi-level bloom is combined directly in the tonemap shader. */
      VkDescriptorImageInfo tm_img_infos[8];
      /* Binding 0: HDR color */
      tm_img_infos[0] = (VkDescriptorImageInfo){
          .sampler = dev->clamp_sampler,
          .imageView = fb->color_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      /* Bindings 1-5: bloom levels 0-4 */
      for (int b = 0; b < MOP_VK_BLOOM_LEVELS; b++) {
        tm_img_infos[1 + b] = (VkDescriptorImageInfo){
            .sampler = dev->clamp_sampler,
            .imageView = (dev->bloom_enabled && fb->bloom_views[b])
                             ? fb->bloom_views[b]
                             : dev->black_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
      }
      /* Binding 6: SSAO */
      tm_img_infos[6] = (VkDescriptorImageInfo){
          .sampler = dev->clamp_sampler,
          .imageView = (dev->ssao_enabled && fb->ssao_blur_view)
                           ? fb->ssao_blur_view
                           : dev->white_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      /* Binding 7: SSR */
      tm_img_infos[7] = (VkDescriptorImageInfo){
          .sampler = dev->clamp_sampler,
          .imageView =
              (ssr_ran && fb->ssr_view) ? fb->ssr_view : dev->black_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      VkWriteDescriptorSet tm_writes[8];
      for (int b = 0; b < 8; b++) {
        tm_writes[b] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = tm_ds,
            .dstBinding = (uint32_t)b,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &tm_img_infos[b],
        };
      }
      vkUpdateDescriptorSets(dev->device, 8, tm_writes, 0, NULL);

      /* Begin tonemap render pass */
      VkClearValue clear = {.color = {{0, 0, 0, 1}}};
      VkRenderPassBeginInfo rp_bi = {
          .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
          .renderPass =
              (dev->taa_enabled && fb->taa_history[0] && fb->taa_framebuffer[0])
                  ? dev->tonemap_render_pass_taa
                  : dev->tonemap_render_pass,
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
      struct {
        float exposure;
        float bloom_intensity;
      } tm_pc = {
          .exposure = dev->hdr_exposure,
          .bloom_intensity = dev->bloom_enabled ? dev->bloom_intensity : 0.0f,
      };
      vkCmdPushConstants(dev->cmd_buf, dev->tonemap_pipeline_layout,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tm_pc),
                         &tm_pc);
      vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0); /* fullscreen triangle */

      vkCmdEndRenderPass(dev->cmd_buf);
      /* When TAA is active the TAA-variant render pass leaves ldr_color
       * in SHADER_READ_ONLY_OPTIMAL; otherwise TRANSFER_SRC_OPTIMAL. */
    }
  }
  mop_vk_pass_timestamp_end(dev);

  /* ---- TAA resolve pass: LDR + history + depth → taa_history[current] ---- */
  mop_vk_pass_timestamp_begin(dev, "taa");
  if (dev->taa_enabled && fb->taa_history[0] && fb->taa_framebuffer[0]) {
    uint32_t cur = fb->taa_current;
    uint32_t prev = 1 - cur;

    /* Transition LDR color to SHADER_READ_ONLY for TAA sampling.
     * When tonemap ran with the TAA pass variant, ldr_color is already in
     * SHADER_READ_ONLY_OPTIMAL — skip the barrier (TBDR optimisation). */
    bool tonemap_ran = dev->tonemap_enabled && fb->tonemap_framebuffer;
    if (!tonemap_ran) {
      mop_vk_transition_image(
          dev->cmd_buf, fb->ldr_color_image, VK_IMAGE_ASPECT_COLOR_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    /* Transition depth: TRANSFER_SRC → SHADER_READ_ONLY */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* Transition history[prev]: first frame = UNDEFINED, else already
     * SHADER_READ_ONLY from previous frame's TAA render pass. */
    if (dev->taa_first_frame) {
      mop_vk_transition_image(
          dev->cmd_buf, fb->taa_history[prev], VK_IMAGE_ASPECT_COLOR_BIT,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    /* Allocate descriptor set: 3 combined image samplers */
    VkDescriptorSetAllocateInfo taa_ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->taa_desc_layout,
    };
    VkDescriptorSet taa_ds;
    VkResult taa_dr =
        vkAllocateDescriptorSets(dev->device, &taa_ds_ai, &taa_ds);
    if (taa_dr == VK_SUCCESS) {
      VkDescriptorImageInfo taa_imgs[3] = {
          {/* binding 0: current LDR frame */
           .sampler = dev->clamp_sampler,
           .imageView = fb->ldr_color_view,
           .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {/* binding 1: history (previous TAA output) */
           .sampler = dev->clamp_sampler,
           .imageView = fb->taa_history_view[prev],
           .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {/* binding 2: depth buffer */
           .sampler = dev->clamp_sampler,
           .imageView = fb->depth_view,
           .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      };
      VkWriteDescriptorSet taa_writes[3];
      for (int i = 0; i < 3; i++) {
        taa_writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = taa_ds,
            .dstBinding = (uint32_t)i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &taa_imgs[i],
        };
      }
      vkUpdateDescriptorSets(dev->device, 3, taa_writes, 0, NULL);

      /* Begin TAA render pass → writes to taa_history[current] */
      VkClearValue taa_clear = {.color = {{0, 0, 0, 1}}};
      VkRenderPassBeginInfo taa_rp_bi = {
          .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
          .renderPass = dev->taa_render_pass,
          .framebuffer = fb->taa_framebuffer[cur],
          .renderArea = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}},
          .clearValueCount = 1,
          .pClearValues = &taa_clear,
      };
      vkCmdBeginRenderPass(dev->cmd_buf, &taa_rp_bi,
                           VK_SUBPASS_CONTENTS_INLINE);

      VkViewport taa_vp = {
          .y = (float)fb->height,
          .width = (float)fb->width,
          .height = -(float)fb->height,
          .maxDepth = 1.0f,
      };
      VkRect2D taa_sc = {.extent = {(uint32_t)fb->width, (uint32_t)fb->height}};
      vkCmdSetViewport(dev->cmd_buf, 0, 1, &taa_vp);
      vkCmdSetScissor(dev->cmd_buf, 0, 1, &taa_sc);

      vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dev->taa_pipeline);
      vkCmdBindDescriptorSets(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              dev->taa_pipeline_layout, 0, 1, &taa_ds, 0, NULL);

      /* Push constants: 152 bytes matching TaaPush in mop_taa.frag */
      struct {
        float inv_vp_jittered[16]; /* offset   0 */
        float prev_vp[16];         /* offset  64 */
        float inv_resolution[2];   /* offset 128 */
        float jitter[2];           /* offset 136 */
        float feedback;            /* offset 144 */
        float first_frame;         /* offset 148 */
      } taa_pc;
      memcpy(taa_pc.inv_vp_jittered, dev->taa_inv_vp_jittered, 64);
      memcpy(taa_pc.prev_vp, dev->taa_prev_vp, 64);
      taa_pc.inv_resolution[0] = 1.0f / (float)fb->width;
      taa_pc.inv_resolution[1] = 1.0f / (float)fb->height;
      taa_pc.jitter[0] = dev->taa_jitter[0];
      taa_pc.jitter[1] = dev->taa_jitter[1];
      taa_pc.feedback = 0.9f;
      taa_pc.first_frame = dev->taa_first_frame ? 1.0f : 0.0f;

      vkCmdPushConstants(dev->cmd_buf, dev->taa_pipeline_layout,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(taa_pc),
                         &taa_pc);

      vkCmdDraw(dev->cmd_buf, 3, 1, 0, 0);
      vkCmdEndRenderPass(dev->cmd_buf);
      /* taa_history[cur] is now SHADER_READ_ONLY from render pass finalLayout
       */

      /* Transition taa_history[cur]: SHADER_READ_ONLY → TRANSFER_SRC
       * for readback copy */
      mop_vk_transition_image(
          dev->cmd_buf, fb->taa_history[cur], VK_IMAGE_ASPECT_COLOR_BIT,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    /* Transition depth back to TRANSFER_SRC for readback */
    mop_vk_transition_image(
        dev->cmd_buf, fb->depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    /* Also transition LDR back to TRANSFER_SRC (overlay pass expects it) */
    mop_vk_transition_image(
        dev->cmd_buf, fb->ldr_color_image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    /* Swap ping-pong for next frame */
    fb->taa_current = 1 - fb->taa_current;
  }
  mop_vk_pass_timestamp_end(dev);

  /* ---- Shadow map rendering (temporal: rendered now, used by NEXT frame) ----
   * Draw data was captured during vk_draw().  We replay all opaque draws
   * into each cascade's depth-only framebuffer.  The shadow render pass
   * handles layout transitions: initialLayout=UNDEFINED (CLEAR),
   * finalLayout=DEPTH_STENCIL_READ_ONLY (ready for sampling next frame).
   * No explicit barriers needed — render pass subpass dependencies handle
   * synchronization with prior/subsequent fragment shader reads. */
  mop_vk_pass_timestamp_begin(dev, "shadows");
  if (dev->shadow_pipeline && dev->shadow_data_valid &&
      dev->shadow_draw_count > 0 && dev->shadow_image) {
    /* Compute cascade split distances and light VP matrices */
    MopMat4 sv, sp;
    memcpy(sv.d, dev->cached_view, 64);
    memcpy(sp.d, dev->cached_projection, 64);
    vk_compute_cascades(dev, &sv, &sp, dev->cached_light_dir);

    /* Render depth for each cascade */
    for (int c = 0; c < MOP_VK_CASCADE_COUNT; c++) {
      VkClearValue shadow_clear = {.depthStencil = {1.0f, 0}};
      VkRenderPassBeginInfo shadow_rp = {
          .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
          .renderPass = dev->shadow_render_pass,
          .framebuffer = dev->shadow_fbs[c],
          .renderArea = {.extent = {MOP_VK_SHADOW_MAP_SIZE,
                                    MOP_VK_SHADOW_MAP_SIZE}},
          .clearValueCount = 1,
          .pClearValues = &shadow_clear,
      };
      vkCmdBeginRenderPass(dev->cmd_buf, &shadow_rp,
                           VK_SUBPASS_CONTENTS_INLINE);

      VkViewport shadow_vp = {
          .width = (float)MOP_VK_SHADOW_MAP_SIZE,
          .height = (float)MOP_VK_SHADOW_MAP_SIZE,
          .minDepth = 0.0f,
          .maxDepth = 1.0f,
      };
      VkRect2D shadow_sc = {
          .extent = {MOP_VK_SHADOW_MAP_SIZE, MOP_VK_SHADOW_MAP_SIZE},
      };
      vkCmdSetViewport(dev->cmd_buf, 0, 1, &shadow_vp);
      vkCmdSetScissor(dev->cmd_buf, 0, 1, &shadow_sc);
      vkCmdBindPipeline(dev->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dev->shadow_pipeline);

      /* Replay all stored opaque draws with light_vp * model */
      for (uint32_t d = 0; d < dev->shadow_draw_count; d++) {
        struct MopVkShadowDraw *sd = &dev->shadow_draws[d];

        /* Push constant: cascade_vp[c] * model (64 bytes) */
        MopMat4 m;
        memcpy(m.d, sd->model, 64);
        MopMat4 light_mvp = mop_mat4_multiply(dev->cascade_vp[c], m);
        vkCmdPushConstants(dev->cmd_buf, dev->shadow_pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, 64, light_mvp.d);

        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(dev->cmd_buf, 0, 1, &sd->vertex_buf, &zero);
        vkCmdBindIndexBuffer(dev->cmd_buf, sd->index_buf, 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(dev->cmd_buf, sd->index_count, 1, 0, 0, 0);
      }

      vkCmdEndRenderPass(dev->cmd_buf);
    }

    /* Shadows are now ready for the next frame's fragment shader.
     * shadows_enabled = true causes FrameGlobals to include cascade_vp/splits
     * in the next frame_begin, and the fragment shader samples the shadow
     * map.*/
    dev->shadows_enabled = true;
  }
  mop_vk_pass_timestamp_end(dev);

  /* Copy images to readback staging buffers.
   * Render pass transitions images to TRANSFER_SRC_OPTIMAL. */

  /* Color readback — from TAA output if available, else LDR (tonemapped),
   * else HDR color image */
  {
    VkImage readback_src;
    if (dev->taa_enabled && fb->taa_history[0]) {
      readback_src = fb->taa_history[1 - fb->taa_current];
    } else if (dev->tonemap_enabled && fb->ldr_color_image) {
      readback_src = fb->ldr_color_image;
    } else {
      readback_src = fb->color_image;
    }
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

  /* Command buffer left open — overlays may record into it before
   * vk_frame_submit() ends and submits the CB. */
}

/* =========================================================================
 * 10b. frame_submit — end command buffer and submit to GPU
 *
 * Called after frame_end + draw_overlays.  All commands for this frame
 * are now recorded in dev->cmd_buf.
 * ========================================================================= */

static void vk_frame_submit(MopRhiDevice *dev, MopRhiFramebuffer *fb) {
  vkEndCommandBuffer(dev->cmd_buf);

  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
  (void)wait_stage; /* reserved for same-frame indirect draw (Phase 7) */
  VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &dev->cmd_buf,
  };
  vkQueueSubmit(dev->queue, 1, &submit, dev->fence);

  /* GPU timestamp readback and staging->CPU memcpy are deferred to the
   * next frame's vk_frame_begin(), after the fence wait.  This allows
   * the CPU to proceed immediately while the GPU executes. */

  /* Store current framebuffer for deferred readback in next frame_begin */
  dev->prev_framebuffer = fb;

  /* Advance to next frame's resources */
  dev->frame_index = (dev->frame_index + 1) % MOP_VK_FRAMES_IN_FLIGHT;
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

/* Block until the GPU finishes the most recent submission and populate the
 * framebuffer's readback buffers eagerly. Without this, the first frame's
 * readback_color is empty because vk_frame_begin defers the memcpy until
 * the next frame's fence wait — CI screenshot tooling needs render → wait
 * → read to be a single deterministic sequence. */
static void vk_frame_wait_readback(MopRhiDevice *dev, MopRhiFramebuffer *fb) {
  if (!dev || !fb)
    return;

  /* The submission for `fb` happened in vk_frame_submit using dev->fence
   * (alias for the frame slot's fence). After submit, frame_index advanced
   * but dev->fence still references the just-submitted fence, so wait on it
   * — this is cheaper than vkDeviceWaitIdle and only blocks for this fb. */
  if (dev->fence) {
    vkWaitForFences(dev->device, 1, &dev->fence, VK_TRUE, UINT64_MAX);
  } else {
    vkDeviceWaitIdle(dev->device);
  }

  size_t npixels = (size_t)fb->width * (size_t)fb->height;
  if (fb->readback_color && fb->readback_color_mapped)
    memcpy(fb->readback_color, fb->readback_color_mapped, npixels * 4);
  if (fb->readback_pick && fb->readback_pick_mapped)
    memcpy(fb->readback_pick, fb->readback_pick_mapped,
           npixels * sizeof(uint32_t));
  if (fb->readback_depth && fb->readback_depth_mapped)
    memcpy(fb->readback_depth, fb->readback_depth_mapped,
           npixels * sizeof(float));

  /* Clear prev_framebuffer so the next vk_frame_begin doesn't redo the
   * memcpy we just did (the fence is already signaled and not yet reset;
   * frame_begin's vkResetFences would be a no-op here too). */
  if (dev->prev_framebuffer == fb)
    dev->prev_framebuffer = NULL;
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
 * Render-to-texture — blit LDR color into a host-owned texture.
 *
 * Uses vkCmdBlitImage: source is the framebuffer's ldr_color_image
 * (R8G8B8A8_SRGB after tonemap + post-process), destination is the
 * caller-supplied R8G8B8A8_UNORM texture.  Dimensions must match.
 *
 * Must be called AFTER `frame_end` so the LDR image is populated.
 * ========================================================================= */

static bool vk_framebuffer_copy_to_texture(MopRhiDevice *dev,
                                           MopRhiFramebuffer *fb,
                                           MopRhiTexture *target) {
  if (!dev || !fb || !target || !target->image)
    return false;
  if (target->is_hdr) {
    MOP_WARN("[VK] framebuffer_copy_to_texture: HDR target not supported");
    return false;
  }
  /* Target is allowed to be smaller than fb; vkCmdBlitImage with
   * VK_FILTER_LINEAR performs the SSAA-to-presentation downsample. */

  VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);

  /* Source: LDR color image transitions SHADER_READ_ONLY → TRANSFER_SRC. */
  mop_vk_transition_image(
      cb, fb->ldr_color_image, VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT);

  /* Target: UNDEFINED/SHADER_READ_ONLY → TRANSFER_DST. */
  mop_vk_transition_image(
      cb, target->image, VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkImageBlit blit = {
      .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                         .layerCount = 1},
      .srcOffsets = {{0, 0, 0}, {fb->width, fb->height, 1}},
      .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                         .layerCount = 1},
      .dstOffsets = {{0, 0, 0}, {target->width, target->height, 1}},
  };
  vkCmdBlitImage(cb, fb->ldr_color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                 VK_FILTER_LINEAR);

  /* Restore layouts. */
  mop_vk_transition_image(
      cb, fb->ldr_color_image, VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  mop_vk_transition_image(
      cb, target->image, VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  staging_submit_and_wait(dev, cb);
  return true;
}

/* =========================================================================
 * Texture readback — copy an RGBA8 texture to a host-provided buffer via a
 * transient staging buffer. Used for CPU-side frame capture / testing.
 * For production GPU pipelines, prefer keeping pixels on GPU via
 * vk_framebuffer_copy_to_texture and consuming the texture directly.
 * ========================================================================= */

static bool vk_texture_read_rgba8(MopRhiDevice *dev, MopRhiTexture *texture,
                                  uint8_t *out_buf, size_t buf_size) {
  if (!dev || !texture || !texture->image || !out_buf)
    return false;
  if (texture->is_hdr) {
    MOP_WARN("[VK] texture_read_rgba8: HDR textures not supported");
    return false;
  }
  size_t needed = (size_t)texture->width * (size_t)texture->height * 4;
  if (buf_size < needed)
    return false;

  /* If the texture fits in the persistent staging buffer, reuse it to
   * avoid an allocate/free cycle.  Otherwise allocate a transient
   * host-visible buffer just for this readback — keeps the persistent
   * staging small while still supporting 4K and beyond. */
  VkBuffer read_buf = VK_NULL_HANDLE;
  VkDeviceMemory read_mem = VK_NULL_HANDLE;
  void *read_mapped = NULL;
  bool transient = false;

  if (needed <= MOP_VK_STAGING_SIZE) {
    read_buf = dev->staging_buf;
    read_mapped = dev->staging_mapped;
  } else {
    VkResult r = mop_vk_create_buffer(dev->device, &dev->mem_props, needed,
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      &read_buf, &read_mem);
    if (r != VK_SUCCESS) {
      MOP_ERROR("[VK] texture_read_rgba8: transient buffer create failed: %d",
                r);
      return false;
    }
    r = vkMapMemory(dev->device, read_mem, 0, needed, 0, &read_mapped);
    if (r != VK_SUCCESS) {
      vkDestroyBuffer(dev->device, read_buf, NULL);
      vkFreeMemory(dev->device, read_mem, NULL);
      MOP_ERROR("[VK] texture_read_rgba8: map failed: %d", r);
      return false;
    }
    transient = true;
  }

  VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);

  /* Transition SHADER_READ_ONLY → TRANSFER_SRC. */
  mop_vk_transition_image(
      cb, texture->image, VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkBufferImageCopy region = {
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1},
      .imageExtent = {(uint32_t)texture->width, (uint32_t)texture->height, 1},
  };
  vkCmdCopyImageToBuffer(cb, texture->image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_buf, 1,
                         &region);

  /* Restore layout. */
  mop_vk_transition_image(
      cb, texture->image, VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  staging_submit_and_wait(dev, cb);
  memcpy(out_buf, read_mapped, needed);

  if (transient) {
    vkUnmapMemory(dev->device, read_mem);
    vkDestroyBuffer(dev->device, read_buf, NULL);
    vkFreeMemory(dev->device, read_mem, NULL);
  }
  return true;
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
  tex->bindless_index = -1;

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

  /* Register in bindless texture array if descriptor indexing is available */
  if (dev->has_descriptor_indexing &&
      dev->texture_registry_count < MOP_VK_MAX_BINDLESS_TEXTURES) {
    tex->bindless_index = (int32_t)dev->texture_registry_count;
    dev->texture_registry[dev->texture_registry_count++] = tex->view;
  } else {
    tex->bindless_index = -1;
  }

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
    staging_submit_and_wait(dev, cb);
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

    staging_submit_and_wait(dev, cb);
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
    staging_submit_and_wait(dev, cb);
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
  tex->bindless_index = -1;

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

  /* Register in bindless texture array if descriptor indexing is available */
  if (dev->has_descriptor_indexing &&
      dev->texture_registry_count < MOP_VK_MAX_BINDLESS_TEXTURES) {
    tex->bindless_index = (int32_t)dev->texture_registry_count;
    dev->texture_registry[dev->texture_registry_count++] = tex->view;
  } else {
    tex->bindless_index = -1;
  }

  return tex;
}

/* =========================================================================
 * 16c. texture_create_ex (compressed BC1/3/5/7 + RGBA8 passthrough)
 * ========================================================================= */

static VkFormat mop_tex_format_to_vk(int format) {
  switch (format) {
  case 0:
    return VK_FORMAT_R8G8B8A8_UNORM; /* MOP_TEX_FORMAT_RGBA8 */
  case 1:
    return VK_FORMAT_BC1_RGB_UNORM_BLOCK; /* MOP_TEX_FORMAT_BC1 */
  case 2:
    return VK_FORMAT_BC3_UNORM_BLOCK; /* MOP_TEX_FORMAT_BC3 */
  case 3:
    return VK_FORMAT_BC5_UNORM_BLOCK; /* MOP_TEX_FORMAT_BC5 */
  case 4:
    return VK_FORMAT_BC7_UNORM_BLOCK; /* MOP_TEX_FORMAT_BC7 */
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

static MopRhiTexture *vk_texture_create_ex(MopRhiDevice *dev, int width,
                                           int height, int format,
                                           int mip_levels, const uint8_t *data,
                                           size_t data_size) {
  /* RGBA8: delegate to the standard path */
  if (format == 0)
    return vk_texture_create(dev, width, height, data);

  VkFormat vk_fmt = mop_tex_format_to_vk(format);
  if (vk_fmt == VK_FORMAT_UNDEFINED) {
    MOP_WARN("[VK] texture_create_ex: unsupported format %d", format);
    return NULL;
  }

  /* Check BC texture compression support */
  VkPhysicalDeviceFeatures feats;
  vkGetPhysicalDeviceFeatures(dev->physical_device, &feats);
  if (!feats.textureCompressionBC) {
    MOP_WARN("[VK] texture_create_ex: BC texture compression not supported by "
             "device");
    return NULL;
  }

  if (!data || data_size == 0) {
    MOP_WARN("[VK] texture_create_ex: NULL or empty data");
    return NULL;
  }

  MopRhiTexture *tex = calloc(1, sizeof(MopRhiTexture));
  if (!tex)
    return NULL;

  tex->width = width;
  tex->height = height;
  tex->bindless_index = -1;

  VkResult r = mop_vk_create_image(
      dev->device, &dev->mem_props, (uint32_t)width, (uint32_t)height, vk_fmt,
      VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &tex->image,
      &tex->memory);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] texture_create_ex: image create failed: %d", r);
    free(tex);
    return NULL;
  }

  r = mop_vk_create_image_view(dev->device, tex->image, vk_fmt,
                               VK_IMAGE_ASPECT_COLOR_BIT, &tex->view);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] texture_create_ex: view create failed: %d", r);
    vkDestroyImage(dev->device, tex->image, NULL);
    vkFreeMemory(dev->device, tex->memory, NULL);
    free(tex);
    return NULL;
  }

  /* Upload compressed data via staging buffer (batched if needed) */
  {
    size_t remaining = data_size;
    size_t src_offset = 0;

    /* Transition to TRANSFER_DST once */
    VkCommandBuffer cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);
    mop_vk_transition_image(
        cb, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    staging_submit_and_wait(dev, cb);

    /* Upload in batches that fit the staging buffer */
    while (remaining > 0) {
      size_t batch = remaining;
      if (batch > MOP_VK_STAGING_SIZE)
        batch = MOP_VK_STAGING_SIZE;

      memcpy(dev->staging_mapped, data + src_offset, batch);

      cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);

      /* For compressed formats, a single copy covering the whole mip 0.
       * bufferRowLength/bufferImageHeight=0 means tightly packed. */
      VkBufferImageCopy region = {
          .bufferOffset = 0,
          .bufferRowLength = (uint32_t)width,
          .bufferImageHeight = (uint32_t)height,
          .imageSubresource =
              {
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .layerCount = 1,
              },
          .imageExtent = {(uint32_t)width, (uint32_t)height, 1},
      };

      /* If uploading in one batch, use a single copy; otherwise
       * we only support single-batch uploads for compressed data */
      if (remaining == data_size && batch == remaining) {
        vkCmdCopyBufferToImage(cb, dev->staging_buf, tex->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
      } else {
        /* Multi-batch: just copy what fits — for compressed textures
         * this is best-effort (caller should ensure data_size <= staging) */
        region.imageExtent.height = 1; /* partial upload fallback */
        vkCmdCopyBufferToImage(cb, dev->staging_buf, tex->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
      }

      staging_submit_and_wait(dev, cb);
      src_offset += batch;
      remaining -= batch;
    }

    /* Transition to SHADER_READ_ONLY */
    cb = mop_vk_begin_oneshot(dev->device, dev->cmd_pool);
    mop_vk_transition_image(
        cb, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    staging_submit_and_wait(dev, cb);
  }

  /* Register in bindless texture array if descriptor indexing is available */
  if (dev->has_descriptor_indexing &&
      dev->texture_registry_count < MOP_VK_MAX_BINDLESS_TEXTURES) {
    tex->bindless_index = (int32_t)dev->texture_registry_count;
    dev->texture_registry[dev->texture_registry_count++] = tex->view;
  } else {
    tex->bindless_index = -1;
  }

  (void)mip_levels; /* TODO: per-mip upload for compressed mip chains */
  return tex;
}

/* =========================================================================
 * 17. texture_destroy
 * ========================================================================= */

static void vk_texture_destroy(MopRhiDevice *dev, MopRhiTexture *tex) {
  if (!tex)
    return;
  if (dev && dev->device) {
    /* Clear bindless registry slot (replace with white fallback) */
    if (tex->bindless_index >= 0 &&
        (uint32_t)tex->bindless_index < MOP_VK_MAX_BINDLESS_TEXTURES) {
      dev->texture_registry[tex->bindless_index] = dev->white_view;
    }
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

/* =========================================================================
 * Per-pass GPU timing helpers (Phase 9A)
 *
 * Call mop_vk_pass_timestamp_begin/end around each render graph pass
 * to record GPU-side timing for that pass.  Results are available via
 * dev->pass_timing_results after the next frame's fence wait.
 * ========================================================================= */

void mop_vk_pass_timestamp_begin(MopRhiDevice *dev, const char *pass_name) {
  if (!dev || !dev->has_timestamp_queries || !dev->pass_timestamp_pool)
    return;
  if (dev->pass_query_count + 2 > MOP_VK_MAX_PASS_TIMESTAMPS)
    return;
  if (dev->pass_timing_count >= MOP_MAX_GPU_PASS_TIMINGS)
    return;

  uint32_t idx = dev->pass_timing_count++;
  uint32_t qi = dev->pass_query_count;
  dev->pass_timing_entries[idx].name = pass_name;
  dev->pass_timing_entries[idx].query_start = qi;
  dev->pass_timing_entries[idx].query_end = qi + 1;
  dev->pass_query_count += 2;

  vkCmdWriteTimestamp(dev->cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      dev->pass_timestamp_pool, qi);
}

void mop_vk_pass_timestamp_end(MopRhiDevice *dev) {
  if (!dev || !dev->has_timestamp_queries || !dev->pass_timestamp_pool)
    return;
  if (dev->pass_timing_count == 0)
    return;

  uint32_t idx = dev->pass_timing_count - 1;
  uint32_t qe = dev->pass_timing_entries[idx].query_end;
  if (qe < MOP_VK_MAX_PASS_TIMESTAMPS) {
    vkCmdWriteTimestamp(dev->cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        dev->pass_timestamp_pool, qe);
  }
}

static void vk_set_exposure(MopRhiDevice *dev, float exposure) {
  dev->hdr_exposure = exposure > 0.0f ? exposure : 0.001f;
}

static void vk_set_bloom(MopRhiDevice *dev, bool enabled, float threshold,
                         float intensity) {
  dev->bloom_enabled =
      enabled && dev->bloom_extract_pipeline && dev->bloom_blur_pipeline;
  dev->bloom_threshold = threshold > 0.0f ? threshold : 0.01f;
  dev->bloom_intensity = intensity > 0.0f ? intensity : 0.0f;
}

static void vk_set_ssao(MopRhiDevice *dev, bool enabled) {
  dev->ssao_enabled = enabled && dev->ssao_pipeline && dev->ssao_blur_pipeline;
}

static void vk_set_ssr(MopRhiDevice *dev, bool enabled, float intensity) {
  dev->ssr_enabled = enabled && dev->ssr_pipeline;
  dev->ssr_intensity = intensity > 0.0f ? intensity : 0.0f;
}

static void vk_set_oit(MopRhiDevice *dev, bool enabled) {
  dev->oit_enabled =
      enabled && dev->oit_pipeline && dev->oit_composite_pipeline;
}

static int32_t vk_add_decal(MopRhiDevice *dev, const float *transform,
                            float opacity, int32_t texture_idx) {
  if (!dev->decal_pipeline || dev->decal_count >= MOP_VK_MAX_DECALS)
    return -1;
  /* Find first inactive slot */
  for (uint32_t i = 0; i < MOP_VK_MAX_DECALS; i++) {
    if (!dev->decals[i].active) {
      dev->decals[i].active = true;
      memcpy(dev->decals[i].model, transform, 64);
      MopMat4 m;
      memcpy(m.d, transform, 64);
      MopMat4 inv = mop_mat4_inverse(m);
      memcpy(dev->decals[i].inv_model, inv.d, 64);
      dev->decals[i].opacity = opacity > 0.0f ? opacity : 1.0f;
      dev->decals[i].texture_idx = texture_idx;
      dev->decal_count++;
      return (int32_t)i;
    }
  }
  return -1;
}

static void vk_remove_decal(MopRhiDevice *dev, int32_t decal_id) {
  if (decal_id < 0 || decal_id >= MOP_VK_MAX_DECALS)
    return;
  if (dev->decals[decal_id].active) {
    dev->decals[decal_id].active = false;
    dev->decal_count--;
  }
}

static void vk_clear_decals(MopRhiDevice *dev) {
  for (uint32_t i = 0; i < MOP_VK_MAX_DECALS; i++)
    dev->decals[i].active = false;
  dev->decal_count = 0;
}

static void vk_set_volumetric(MopRhiDevice *dev, float density, float r,
                              float g, float b, float anisotropy, int steps) {
  if (!dev->volumetric_pipeline)
    return;
  dev->volumetric_enabled = true;
  dev->volumetric_density = density;
  dev->volumetric_color[0] = r;
  dev->volumetric_color[1] = g;
  dev->volumetric_color[2] = b;
  dev->volumetric_anisotropy = anisotropy;
  dev->volumetric_steps = steps > 0 ? steps : 32;
}

static void vk_set_taa_params(MopRhiDevice *dev, const float *inv_vp_jittered,
                              const float *prev_vp, float jitter_x,
                              float jitter_y, bool first_frame) {
  if (!dev->taa_pipeline || !dev->taa_render_pass) {
    dev->taa_enabled = false;
    return;
  }
  dev->taa_enabled = true;
  memcpy(dev->taa_inv_vp_jittered, inv_vp_jittered, 64);
  memcpy(dev->taa_prev_vp, prev_vp, 64);
  dev->taa_jitter[0] = jitter_x;
  dev->taa_jitter[1] = jitter_y;
  dev->taa_first_frame = first_frame;
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

  /* Use or grow persistent SSBO for prim data (only if we have prims) */
  VkDeviceSize ssbo_size = 0;
  if (has_prims) {
    ssbo_size = (VkDeviceSize)prim_count * 3 * sizeof(float) * 4;
    if (ssbo_size > MOP_VK_STAGING_SIZE)
      has_prims = false;
    else if (ssbo_size > fb->overlay_ssbo_size) {
      /* Grow with power-of-2 sizing */
      VkDeviceSize new_size =
          fb->overlay_ssbo_size ? fb->overlay_ssbo_size : 1024;
      while (new_size < ssbo_size)
        new_size *= 2;
      /* Destroy old */
      if (fb->overlay_ssbo) {
        if (fb->overlay_ssbo_mapped)
          vkUnmapMemory(dev->device, fb->overlay_ssbo_mem);
        vkDestroyBuffer(dev->device, fb->overlay_ssbo, NULL);
        vkFreeMemory(dev->device, fb->overlay_ssbo_mem, NULL);
        fb->overlay_ssbo = VK_NULL_HANDLE;
        fb->overlay_ssbo_mem = VK_NULL_HANDLE;
        fb->overlay_ssbo_mapped = NULL;
        fb->overlay_ssbo_size = 0;
      }
      VkResult r = mop_vk_create_buffer(
          dev->device, &dev->mem_props, new_size,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
          &fb->overlay_ssbo, &fb->overlay_ssbo_mem);
      if (r != VK_SUCCESS)
        has_prims = false;
      else {
        r = vkMapMemory(dev->device, fb->overlay_ssbo_mem, 0, new_size, 0,
                        &fb->overlay_ssbo_mapped);
        if (r != VK_SUCCESS) {
          vkDestroyBuffer(dev->device, fb->overlay_ssbo, NULL);
          vkFreeMemory(dev->device, fb->overlay_ssbo_mem, NULL);
          fb->overlay_ssbo = VK_NULL_HANDLE;
          fb->overlay_ssbo_mem = VK_NULL_HANDLE;
          fb->overlay_ssbo_mapped = NULL;
          has_prims = false;
        } else {
          fb->overlay_ssbo_size = new_size;
        }
      }
    }
  }

  if (!has_prims && !has_grid)
    return;

  /* Fill SSBO if we have prims */
  if (has_prims && fb->overlay_ssbo_mapped) {
    const uint8_t *src = (const uint8_t *)prims;
    float *dst = (float *)fb->overlay_ssbo_mapped;
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
  }

  /* Record overlay commands into the main command buffer (still open from
   * frame_end).  The submit happens in vk_frame_submit() after this. */
  VkCommandBuffer cb = dev->cmd_buf;

  /* The LDR image already has the current frame's content from frame_end.
   * Transition from TRANSFER_SRC (its state after readback copy) so the
   * overlay render pass can load it. */
  {
    VkImageMemoryBarrier to_attach = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = fb->ldr_color_image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1,
                             .layerCount = 1},
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 1, &to_attach);
  }

  /* Depth copy now happens inline in the main command buffer (before SSAO),
   * so we no longer need to upload stale readback data here. */

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
          .sampler = dev->clamp_sampler,
          .imageView =
              fb->depth_copy_view ? fb->depth_copy_view : fb->depth_view,
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
          .sampler = dev->clamp_sampler,
          .imageView =
              fb->depth_copy_view ? fb->depth_copy_view : fb->depth_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      VkDescriptorBufferInfo ssbo_info = {
          .buffer = fb->overlay_ssbo,
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

  /* No submit here — vk_frame_submit() handles CB end + queue submit.
   * Readback memcpy is deferred to next frame's vk_frame_begin(). */
}

/* =========================================================================
 * Shader module management (Phase 0C — runtime shader loading)
 * ========================================================================= */

static MopRhiShader *vk_shader_create(MopRhiDevice *dev,
                                      const uint32_t *bytecode, size_t size) {
  if (!dev || !bytecode || size < 4)
    return NULL;

  VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size,
      .pCode = bytecode,
  };
  VkShaderModule mod = VK_NULL_HANDLE;
  VkResult r = vkCreateShaderModule(dev->device, &ci, NULL, &mod);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] shader_create failed: %d", r);
    return NULL;
  }

  MopRhiShader *shader = malloc(sizeof(MopRhiShader));
  if (!shader) {
    vkDestroyShaderModule(dev->device, mod, NULL);
    return NULL;
  }
  shader->module = mod;
  return shader;
}

static void vk_shader_destroy(MopRhiDevice *dev, MopRhiShader *shader) {
  if (!shader)
    return;
  if (dev && dev->device)
    vkDestroyShaderModule(dev->device, shader->module, NULL);
  free(shader);
}

/* =========================================================================
 * GPU-driven rendering toggle (scaffolding)
 *
 * Flips `indirect_draw_requested`, which the frame-end warm-up tracker
 * consumes to promote the flag to `indirect_draw_enabled`. The main
 * render path does not yet consume the enabled flag — the actual
 * per-pipeline-bucket vkCmdDrawIndexedIndirectCount dispatch needs
 * uber-shader + bindless + pipeline bucketing work (docs/TODO.md).
 * Safe to toggle today; it just gates whether the warm-up counter runs.
 * ========================================================================= */

static void vk_set_gpu_driven_rendering(MopRhiDevice *dev, bool enabled) {
  if (!dev)
    return;
  dev->indirect_draw_requested = enabled;
  if (!enabled) {
    dev->indirect_draw_enabled = false;
    dev->indirect_draw_frame_count = 0;
  }
}

static const MopRhiBackend VK_BACKEND = {
    .name = "vulkan",
    .device_create = vk_device_create,
    .device_destroy = vk_device_destroy,
    .buffer_create = vk_buffer_create,
    .buffer_destroy = vk_buffer_destroy,
    .framebuffer_create = vk_framebuffer_create,
    .framebuffer_create_from_texture =
        NULL, /* TODO: wrap host VkImage as color attachment */
    .framebuffer_destroy = vk_framebuffer_destroy,
    .framebuffer_resize = vk_framebuffer_resize,
    .frame_begin = vk_frame_begin,
    .frame_end = vk_frame_end,
    .frame_submit = vk_frame_submit,
    .draw = vk_draw,
    .frame_wait_readback = vk_frame_wait_readback,
    .pick_read_id = vk_pick_read_id,
    .pick_read_depth = vk_pick_read_depth,
    .framebuffer_read_color = vk_framebuffer_read_color,
    .framebuffer_read_object_id = vk_framebuffer_read_object_id,
    .framebuffer_read_depth = vk_framebuffer_read_depth,
    .texture_create = vk_texture_create,
    .texture_create_hdr = vk_texture_create_hdr,
    .texture_create_ex = vk_texture_create_ex,
    .texture_destroy = vk_texture_destroy,
    .draw_instanced = vk_draw_instanced,
    .buffer_update = vk_buffer_update,
    .buffer_read = vk_buffer_read,
    .frame_gpu_time_ms = vk_frame_gpu_time_ms,
    .set_exposure = vk_set_exposure,
    .set_bloom = vk_set_bloom,
    .set_ssao = vk_set_ssao,
    .set_ssr = vk_set_ssr,
    .set_oit = vk_set_oit,
    .add_decal = vk_add_decal,
    .remove_decal = vk_remove_decal,
    .clear_decals = vk_clear_decals,
    .set_volumetric = vk_set_volumetric,
    .set_taa_params = vk_set_taa_params,
    .set_ibl_textures = vk_set_ibl_textures,
    .draw_skybox = vk_draw_skybox,
    .draw_overlays = vk_draw_overlays,
    .shader_create = vk_shader_create,
    .shader_destroy = vk_shader_destroy,
    .framebuffer_copy_to_texture = vk_framebuffer_copy_to_texture,
    .texture_read_rgba8 = vk_texture_read_rgba8,
    .set_gpu_driven_rendering = vk_set_gpu_driven_rendering,
};

const MopRhiBackend *mop_rhi_backend_vulkan(void) { return &VK_BACKEND; }

#endif /* MOP_HAS_VULKAN */
