/*
 * Master of Puppets — Vulkan Backend
 * vulkan_pipeline.c — Render pass, pipeline layout, pipeline cache
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(MOP_HAS_VULKAN)

#include "vulkan_internal.h"
#include "vulkan_shaders.h"

/* -------------------------------------------------------------------------
 * Render pass: color (R8G8B8A8_UNORM) + picking (R32_UINT) + depth (D32_SFLOAT)
 *
 * Final layouts are TRANSFER_SRC_OPTIMAL so we can readback at frame_end.
 * ------------------------------------------------------------------------- */

VkResult mop_vk_create_render_pass(VkDevice device, VkRenderPass *out) {
  VkAttachmentDescription attachments[3] = {
      /* 0: Color */
      {
          .format = VK_FORMAT_R8G8B8A8_SRGB,
          .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      },
      /* 1: Picking (R32_UINT) */
      {
          .format = VK_FORMAT_R32_UINT,
          .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      },
      /* 2: Depth (D32_SFLOAT) */
      {
          .format = VK_FORMAT_D32_SFLOAT,
          .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      },
  };

  VkAttachmentReference color_refs[2] = {
      {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {.attachment = 1, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
  };

  VkAttachmentReference depth_ref = {
      .attachment = 2,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 2,
      .pColorAttachments = color_refs,
      .pDepthStencilAttachment = &depth_ref,
  };

  /* Ensure the render pass transitions are visible before transfer */
  VkSubpassDependency dep = {
      .srcSubpass = 0,
      .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
  };

  VkRenderPassCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 3,
      .pAttachments = attachments,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dep,
  };

  return vkCreateRenderPass(device, &ci, NULL, out);
}

/* -------------------------------------------------------------------------
 * Descriptor set layout: UBO (binding 0) + sampler (binding 1)
 * ------------------------------------------------------------------------- */

VkResult mop_vk_create_desc_set_layout(VkDevice device,
                                       VkDescriptorSetLayout *out) {
  VkDescriptorSetLayoutBinding bindings[2] = {
      {
          .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
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

  VkDescriptorSetLayoutCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings,
  };

  return vkCreateDescriptorSetLayout(device, &ci, NULL, out);
}

/* -------------------------------------------------------------------------
 * Pipeline layout: push constants (128 bytes) + one descriptor set
 * ------------------------------------------------------------------------- */

VkResult mop_vk_create_pipeline_layout(VkDevice device,
                                       VkDescriptorSetLayout desc_layout,
                                       VkPipelineLayout *out) {
  VkPushConstantRange push = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
      .offset = 0,
      .size = 128, /* mat4 mvp + mat4 model */
  };

  VkPipelineLayoutCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &desc_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push,
  };

  return vkCreatePipelineLayout(device, &ci, NULL, out);
}

/* -------------------------------------------------------------------------
 * Pipeline creation for a given state key
 * ------------------------------------------------------------------------- */

static VkPipeline create_pipeline(struct MopRhiDevice *dev, uint32_t key,
                                  uint32_t vertex_stride) {
  bool wireframe = (key & 1) != 0;
  bool depth_test = (key & 2) != 0;
  bool backface_cull = (key & 4) != 0;
  MopBlendMode blend = (MopBlendMode)((key >> 3) & 0x3);

  /* Shader stages */
  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->solid_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = wireframe ? dev->wireframe_frag : dev->solid_frag,
          .pName = "main",
      },
  };

  /* Vertex input — stride comes from the draw call's vertex format.
   * Standard MopVertex is 48 bytes; flex vertex formats may be wider. */
  VkVertexInputBindingDescription binding = {
      .binding = 0,
      .stride = vertex_stride,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
  };

  VkVertexInputAttributeDescription attrs[4] = {
      {.location = 0,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = 0}, /* position */
      {.location = 1,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = 3 * sizeof(float)}, /* normal */
      {.location = 2,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
       .offset = 6 * sizeof(float)}, /* color */
      {.location = 3,
       .binding = 0,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = 10 * sizeof(float)}, /* texcoord */
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &binding,
      .vertexAttributeDescriptionCount = 4,
      .pVertexAttributeDescriptions = attrs,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

  /* Dynamic viewport and scissor */
  VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };

  VkPipelineDynamicStateCreateInfo dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dynamic_states,
  };

  VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
  };

  /* Rasterizer */
  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = (wireframe && dev->has_fill_mode_non_solid)
                         ? VK_POLYGON_MODE_LINE
                         : VK_POLYGON_MODE_FILL,
      .cullMode = backface_cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  /* Depth/stencil */
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = depth_test ? VK_TRUE : VK_FALSE,
      .depthWriteEnable = depth_test ? VK_TRUE : VK_FALSE,
      .depthCompareOp = VK_COMPARE_OP_LESS,
  };

  /* Color blend — two attachments: color (index 0) and picking (index 1) */
  VkPipelineColorBlendAttachmentState blend_attachments[2];
  memset(blend_attachments, 0, sizeof(blend_attachments));

  /* Attachment 0: color — blending depends on mode */
  blend_attachments[0].colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  if (blend != MOP_BLEND_OPAQUE) {
    blend_attachments[0].blendEnable = VK_TRUE;
    switch (blend) {
    case MOP_BLEND_ADDITIVE:
      blend_attachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      blend_attachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
      blend_attachments[0].colorBlendOp = VK_BLEND_OP_ADD;
      blend_attachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blend_attachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blend_attachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
      break;
    case MOP_BLEND_MULTIPLY:
      blend_attachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
      blend_attachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
      blend_attachments[0].colorBlendOp = VK_BLEND_OP_ADD;
      blend_attachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
      blend_attachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      blend_attachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
      break;
    default: /* ALPHA */
      blend_attachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      blend_attachments[0].dstColorBlendFactor =
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend_attachments[0].colorBlendOp = VK_BLEND_OP_ADD;
      blend_attachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blend_attachments[0].dstAlphaBlendFactor =
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend_attachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
      break;
    }
  }

  /* Attachment 1: picking — no blending, write uint */
  blend_attachments[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;

  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = blend_attachments,
  };

  VkGraphicsPipelineCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pDepthStencilState = &depth_stencil,
      .pColorBlendState = &color_blend,
      .pDynamicState = &dynamic_state,
      .layout = dev->pipeline_layout,
      .renderPass = dev->render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] pipeline creation failed for key=%u: %d", key, r);
    return VK_NULL_HANDLE;
  }

  return pipeline;
}

/* -------------------------------------------------------------------------
 * Get or lazily create a pipeline
 * ------------------------------------------------------------------------- */

VkPipeline mop_vk_get_pipeline(struct MopRhiDevice *dev, uint32_t key,
                               uint32_t vertex_stride) {
  if (key >= MOP_VK_MAX_PIPELINES) {
    MOP_ERROR("[VK] pipeline key %u out of range", key);
    return VK_NULL_HANDLE;
  }

  /* Recreate if stride changed (non-standard stride slot reused) */
  if (dev->pipelines[key] != VK_NULL_HANDLE &&
      dev->pipeline_strides[key] != vertex_stride) {
    vkDestroyPipeline(dev->device, dev->pipelines[key], NULL);
    dev->pipelines[key] = VK_NULL_HANDLE;
  }

  if (dev->pipelines[key] == VK_NULL_HANDLE) {
    dev->pipelines[key] = create_pipeline(dev, key, vertex_stride);
    dev->pipeline_strides[key] = vertex_stride;
  }

  return dev->pipelines[key];
}

#endif /* MOP_HAS_VULKAN */
