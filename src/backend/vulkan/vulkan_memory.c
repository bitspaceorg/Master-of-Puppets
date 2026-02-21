/*
 * Master of Puppets — Vulkan Backend
 * vulkan_memory.c — Memory type finder, buffer/image allocation helpers
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(MOP_HAS_VULKAN)

#include "vulkan_internal.h"

/* -------------------------------------------------------------------------
 * Find a memory type matching the requirements
 * ------------------------------------------------------------------------- */

int mop_vk_find_memory_type(const VkPhysicalDeviceMemoryProperties *props,
                            uint32_t type_filter, VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
    if ((type_filter & (1u << i)) &&
        (props->memoryTypes[i].propertyFlags & flags) == flags) {
      return (int)i;
    }
  }
  return -1;
}

/* -------------------------------------------------------------------------
 * Create a VkBuffer with dedicated memory
 * ------------------------------------------------------------------------- */

VkResult mop_vk_create_buffer(VkDevice device,
                              const VkPhysicalDeviceMemoryProperties *props,
                              VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags mem_flags,
                              VkBuffer *out_buffer,
                              VkDeviceMemory *out_memory) {
  VkBufferCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VkResult r = vkCreateBuffer(device, &ci, NULL, out_buffer);
  if (r != VK_SUCCESS)
    return r;

  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device, *out_buffer, &req);

  int mem_idx = mop_vk_find_memory_type(props, req.memoryTypeBits, mem_flags);
  if (mem_idx < 0) {
    vkDestroyBuffer(device, *out_buffer, NULL);
    *out_buffer = VK_NULL_HANDLE;
    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = (uint32_t)mem_idx,
  };

  r = vkAllocateMemory(device, &ai, NULL, out_memory);
  if (r != VK_SUCCESS) {
    vkDestroyBuffer(device, *out_buffer, NULL);
    *out_buffer = VK_NULL_HANDLE;
    return r;
  }

  vkBindBufferMemory(device, *out_buffer, *out_memory, 0);
  return VK_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Create a VkImage with dedicated memory
 * ------------------------------------------------------------------------- */

VkResult mop_vk_create_image(VkDevice device,
                             const VkPhysicalDeviceMemoryProperties *props,
                             uint32_t width, uint32_t height, VkFormat format,
                             VkImageUsageFlags usage, VkImage *out_image,
                             VkDeviceMemory *out_memory) {
  VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {width, height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  VkResult r = vkCreateImage(device, &ci, NULL, out_image);
  if (r != VK_SUCCESS)
    return r;

  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, *out_image, &req);

  int mem_idx = mop_vk_find_memory_type(props, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mem_idx < 0) {
    vkDestroyImage(device, *out_image, NULL);
    *out_image = VK_NULL_HANDLE;
    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = (uint32_t)mem_idx,
  };

  r = vkAllocateMemory(device, &ai, NULL, out_memory);
  if (r != VK_SUCCESS) {
    vkDestroyImage(device, *out_image, NULL);
    *out_image = VK_NULL_HANDLE;
    return r;
  }

  vkBindImageMemory(device, *out_image, *out_memory, 0);
  return VK_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Create a VkImageView
 * ------------------------------------------------------------------------- */

VkResult mop_vk_create_image_view(VkDevice device, VkImage image,
                                  VkFormat format, VkImageAspectFlags aspect,
                                  VkImageView *out_view) {
  VkImageViewCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .components =
          {
              .r = VK_COMPONENT_SWIZZLE_IDENTITY,
              .g = VK_COMPONENT_SWIZZLE_IDENTITY,
              .b = VK_COMPONENT_SWIZZLE_IDENTITY,
              .a = VK_COMPONENT_SWIZZLE_IDENTITY,
          },
      .subresourceRange =
          {
              .aspectMask = aspect,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };

  return vkCreateImageView(device, &ci, NULL, out_view);
}

#endif /* MOP_HAS_VULKAN */
