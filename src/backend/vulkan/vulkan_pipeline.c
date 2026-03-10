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

VkResult mop_vk_create_render_pass(VkDevice device,
                                   VkSampleCountFlagBits samples,
                                   VkRenderPass *out) {
  if (samples > VK_SAMPLE_COUNT_1_BIT) {
    /* ---- MSAA render pass via vkCreateRenderPass2 (Vulkan 1.2) ----
     *
     * 6 attachments — all resolves happen in-pass (no manual
     * vkCmdResolveImage): 0: MSAA color   (4x, CLEAR → DONT_CARE, auto-resolved
     * → 3) 1: MSAA picking (4x, CLEAR → DONT_CARE, auto-resolved → 4) 2: MSAA
     * depth   (4x, CLEAR → DONT_CARE, depth-resolved → 5) 3: Resolve color (1x,
     * DONT_CARE → STORE, TRANSFER_SRC) 4: Resolve picking (1x, DONT_CARE →
     * STORE, TRANSFER_SRC) 5: Resolve depth   (1x, DONT_CARE → STORE,
     * TRANSFER_SRC)
     *
     * Integer picking (R32_UINT) resolve takes sample 0 per Vulkan spec.
     * Depth resolve uses VK_RESOLVE_MODE_SAMPLE_ZERO_BIT (mandatory in 1.2).
     */
    VkAttachmentDescription2 attachments[6] = {
        /* 0: MSAA color (HDR) */
        {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .samples = samples,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        },
        /* 1: MSAA picking */
        {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = VK_FORMAT_R32_UINT,
            .samples = samples,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        },
        /* 2: MSAA depth */
        {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = VK_FORMAT_D32_SFLOAT,
            .samples = samples,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
        /* 3: Resolve color (1x, HDR) — CLEAR prevents stale tile data
         * on MoltenVK/Metal TBDR; resolve overwrites all pixels anyway. */
        {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        },
        /* 4: Resolve picking (1x) */
        {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = VK_FORMAT_R32_UINT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        },
        /* 5: Resolve depth (1x) */
        {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
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

    VkAttachmentReference2 color_refs[2] = {
        {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
         .attachment = 0,
         .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT},
        {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
         .attachment = 1,
         .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT},
    };

    VkAttachmentReference2 resolve_refs[2] = {
        {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
         .attachment = 3,
         .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT},
        {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
         .attachment = 4,
         .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT},
    };

    VkAttachmentReference2 depth_ref = {
        .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
        .attachment = 2,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
    };

    VkAttachmentReference2 depth_resolve_ref = {
        .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
        .attachment = 5,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
    };

    VkSubpassDescriptionDepthStencilResolve depth_resolve = {
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
        .depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
        .stencilResolveMode = VK_RESOLVE_MODE_NONE,
        .pDepthStencilResolveAttachment = &depth_resolve_ref,
    };

    VkSubpassDescription2 subpass = {
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
        .pNext = &depth_resolve,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 2,
        .pColorAttachments = color_refs,
        .pResolveAttachments = resolve_refs,
        .pDepthStencilAttachment = &depth_ref,
    };

    VkSubpassDependency2 dep = {
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,
        .srcSubpass = 0,
        .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask =
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
        .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
    };

    VkRenderPassCreateInfo2 ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
        .attachmentCount = 6,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };

    return vkCreateRenderPass2(device, &ci, NULL, out);
  }

  /* ---- Non-MSAA: original 3-attachment render pass ---- */
  VkAttachmentDescription attachments[3] = {
      /* 0: Color (HDR) */
      {
          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
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

  /* Ensure the render pass transitions are visible before transfer AND
   * fragment shader reads (post-processing on TBDR GPUs like Apple/MoltenVK
   * needs the tile store to flush before bloom/SSAO sample the attachments) */
  VkSubpassDependency dep = {
      .srcSubpass = 0,
      .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
      .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
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
  VkDescriptorSetLayoutBinding bindings[9] = {
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
      {
          .binding = 2,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
      /* IBL: irradiance map (diffuse) */
      {
          .binding = 3,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
      /* IBL: prefiltered specular map */
      {
          .binding = 4,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
      /* IBL: BRDF LUT */
      {
          .binding = 5,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
      /* PBR: normal map */
      {
          .binding = 6,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
      /* PBR: metallic-roughness map (glTF: G=roughness, B=metallic) */
      {
          .binding = 7,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
      /* PBR: ambient occlusion map (R channel) */
      {
          .binding = 8,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
  };

  VkDescriptorSetLayoutCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 9,
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
      .rasterizationSamples = dev->msaa_samples,
  };

  /* Depth/stencil — reversed-Z uses GREATER_OR_EQUAL */
  VkCompareOp depth_op =
      dev->reverse_z ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS;
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = depth_test ? VK_TRUE : VK_FALSE,
      .depthWriteEnable = depth_test ? VK_TRUE : VK_FALSE,
      .depthCompareOp = depth_op,
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

/* -------------------------------------------------------------------------
 * Instanced rendering pipeline
 *
 * Uses mop_instanced.vert with two vertex bindings:
 *   Binding 0: per-vertex (position, normal, color, texcoord)
 *   Binding 1: per-instance (mat4 model = 4 × vec4 at locations 4-7)
 * ------------------------------------------------------------------------- */

static VkPipeline create_instanced_pipeline(struct MopRhiDevice *dev,
                                            uint32_t vertex_stride) {
  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->instanced_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->solid_frag,
          .pName = "main",
      },
  };

  /* Two vertex bindings: per-vertex + per-instance */
  VkVertexInputBindingDescription bindings[2] = {
      {
          .binding = 0,
          .stride = vertex_stride,
          .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
      },
      {
          .binding = 1,
          .stride = 64, /* sizeof(MopMat4) = 16 floats × 4 bytes */
          .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
      },
  };

  /* Vertex attributes: 4 per-vertex + 4 per-instance (mat4 columns) */
  VkVertexInputAttributeDescription attrs[8] = {
      /* Per-vertex (binding 0) */
      {.location = 0,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = 0},
      {.location = 1,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = 3 * sizeof(float)},
      {.location = 2,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
       .offset = 6 * sizeof(float)},
      {.location = 3,
       .binding = 0,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = 10 * sizeof(float)},
      /* Per-instance model matrix columns (binding 1) */
      {.location = 4,
       .binding = 1,
       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
       .offset = 0},
      {.location = 5,
       .binding = 1,
       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
       .offset = 16},
      {.location = 6,
       .binding = 1,
       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
       .offset = 32},
      {.location = 7,
       .binding = 1,
       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
       .offset = 48},
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 2,
      .pVertexBindingDescriptions = bindings,
      .vertexAttributeDescriptionCount = 8,
      .pVertexAttributeDescriptions = attrs,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

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

  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_BACK_BIT,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = dev->msaa_samples,
  };

  VkCompareOp inst_depth_op =
      dev->reverse_z ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS;
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = inst_depth_op,
  };

  VkPipelineColorBlendAttachmentState blend_attachments[2];
  memset(blend_attachments, 0, sizeof(blend_attachments));
  blend_attachments[0].colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    MOP_ERROR("[VK] instanced pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

VkPipeline mop_vk_get_instanced_pipeline(struct MopRhiDevice *dev,
                                         uint32_t vertex_stride) {
  if (dev->instanced_pipeline != VK_NULL_HANDLE &&
      dev->instanced_pipeline_stride != vertex_stride) {
    vkDestroyPipeline(dev->device, dev->instanced_pipeline, NULL);
    dev->instanced_pipeline = VK_NULL_HANDLE;
  }
  if (dev->instanced_pipeline == VK_NULL_HANDLE) {
    dev->instanced_pipeline = create_instanced_pipeline(dev, vertex_stride);
    dev->instanced_pipeline_stride = vertex_stride;
  }
  return dev->instanced_pipeline;
}

/* =========================================================================
 * Shadow render pass (depth-only)
 * ========================================================================= */

VkResult mop_vk_create_shadow_render_pass(VkDevice device, VkRenderPass *out) {
  VkAttachmentDescription attachment = {
      .format = VK_FORMAT_D32_SFLOAT,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
  };

  VkAttachmentReference depth_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 0,
      .pDepthStencilAttachment = &depth_ref,
  };

  VkSubpassDependency deps[2] = {
      {
          .srcSubpass = VK_SUBPASS_EXTERNAL,
          .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
          .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      },
      {
          .srcSubpass = 0,
          .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      },
  };

  VkRenderPassCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 2,
      .pDependencies = deps,
  };

  return vkCreateRenderPass(device, &ci, NULL, out);
}

/* =========================================================================
 * Shadow pipeline creation (depth-only with bias)
 * ========================================================================= */

VkPipeline mop_vk_create_shadow_pipeline(struct MopRhiDevice *dev) {
  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->shadow_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->shadow_frag,
          .pName = "main",
      },
  };

  /* Vertex input: position + normal + color (must match binding layout) */
  VkVertexInputBindingDescription binding = {
      .binding = 0,
      .stride = (uint32_t)sizeof(MopVertex),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
  };

  VkVertexInputAttributeDescription attrs[3] = {
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
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &binding,
      .vertexAttributeDescriptionCount = 3,
      .pVertexAttributeDescriptions = attrs,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

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

  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_BACK_BIT,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .depthBiasEnable = VK_TRUE,
      .depthBiasConstantFactor = 1.0f,
      .depthBiasSlopeFactor = 1.5f,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
  };

  /* No color attachments for shadow pass */
  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 0,
      .pAttachments = NULL,
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
      .layout = dev->shadow_pipeline_layout,
      .renderPass = dev->shadow_render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] shadow pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

/* =========================================================================
 * Post-process render pass (single color output)
 * ========================================================================= */

VkResult mop_vk_create_postprocess_render_pass(VkDevice device,
                                               VkRenderPass *out) {
  VkAttachmentDescription attachment = {
      .format = VK_FORMAT_R8G8B8A8_SRGB,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
  };

  VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
  };

  VkSubpassDependency dep = {
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
  };

  VkRenderPassCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dep,
  };

  return vkCreateRenderPass(device, &ci, NULL, out);
}

/* =========================================================================
 * FXAA post-process pipeline (fullscreen triangle)
 * ========================================================================= */

VkPipeline mop_vk_create_postprocess_pipeline(struct MopRhiDevice *dev) {
  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->fullscreen_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->fxaa_frag,
          .pName = "main",
      },
  };

  /* No vertex input — fullscreen triangle generated in shader */
  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

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

  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };

  VkPipelineColorBlendAttachmentState blend_att = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_att,
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
      .layout = dev->postprocess_pipeline_layout,
      .renderPass = dev->postprocess_render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] postprocess pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

/* -------------------------------------------------------------------------
 * HDR Tonemap render pass + pipeline
 *
 * Single R8G8B8A8_SRGB attachment — LDR output after ACES tonemapping.
 * Final layout is TRANSFER_SRC_OPTIMAL for readback copy.
 * ------------------------------------------------------------------------- */

VkResult mop_vk_create_tonemap_render_pass(VkDevice device, VkRenderPass *out) {
  VkAttachmentDescription attachment = {
      .format = VK_FORMAT_R8G8B8A8_SRGB,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
  };

  VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
  };

  VkSubpassDependency dep = {
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
  };

  VkRenderPassCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dep,
  };

  return vkCreateRenderPass(device, &ci, NULL, out);
}

VkPipeline mop_vk_create_tonemap_pipeline(struct MopRhiDevice *dev) {
  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->fullscreen_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->tonemap_frag,
          .pName = "main",
      },
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

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

  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };

  VkPipelineColorBlendAttachmentState blend_att = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_att,
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
      .layout = dev->tonemap_pipeline_layout,
      .renderPass = dev->tonemap_render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] tonemap pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

/* =========================================================================
 * Skybox pipeline (equirectangular env map, fullscreen triangle)
 *
 * Renders into the main render pass (color + picking attachments).
 * No depth test/write — draws as background behind everything.
 * ========================================================================= */

VkPipeline mop_vk_create_skybox_pipeline(struct MopRhiDevice *dev) {
#if defined(MOP_VK_HAS_SKYBOX_SHADERS)
  if (!dev->skybox_frag || !dev->fullscreen_vert)
    return VK_NULL_HANDLE;

  /* Descriptor set layout: binding 0 = UBO (dynamic), binding 1 = env map
   * sampler */
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

  VkDescriptorSetLayoutCreateInfo desc_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings,
  };
  VkResult r = vkCreateDescriptorSetLayout(dev->device, &desc_ci, NULL,
                                           &dev->skybox_desc_layout);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] skybox descriptor set layout failed: %d", r);
    return VK_NULL_HANDLE;
  }

  VkPipelineLayoutCreateInfo layout_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dev->skybox_desc_layout,
  };
  r = vkCreatePipelineLayout(dev->device, &layout_ci, NULL,
                             &dev->skybox_pipeline_layout);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] skybox pipeline layout failed: %d", r);
    return VK_NULL_HANDLE;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->fullscreen_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->skybox_frag,
          .pName = "main",
      },
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

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

  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = dev->msaa_samples,
  };

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };

  /* Two color attachments: color (HDR) + picking (R32_UINT) */
  VkPipelineColorBlendAttachmentState blend_atts[2] = {
      {
          .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                            VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
      },
      {
          .colorWriteMask = VK_COLOR_COMPONENT_R_BIT,
      },
  };

  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = blend_atts,
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
      .layout = dev->skybox_pipeline_layout,
      .renderPass = dev->render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci, NULL,
                                &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] skybox pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
#else
  (void)dev;
  return VK_NULL_HANDLE;
#endif
}

/* -------------------------------------------------------------------------
 * Overlay render pass
 *
 * Single R8G8B8A8_SRGB attachment — loads existing LDR color, blends SDF
 * overlays on top, stores result.  Final layout: TRANSFER_SRC_OPTIMAL
 * for readback copy.
 * ------------------------------------------------------------------------- */

VkResult mop_vk_create_overlay_render_pass(VkDevice device, VkRenderPass *out) {
  VkAttachmentDescription attachment = {
      .format = VK_FORMAT_R8G8B8A8_SRGB,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
  };

  VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
  };

  VkSubpassDependency dep = {
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask =
          VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
  };

  VkRenderPassCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dep,
  };

  return vkCreateRenderPass(device, &ci, NULL, out);
}

/* -------------------------------------------------------------------------
 * SDF overlay pipeline (fullscreen triangle + overlay fragment shader)
 *
 * Uses alpha blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA).
 * Descriptor set: binding 0 = depth sampler, binding 1 = SSBO prims.
 * Push constants: prim_count + fb dimensions + reverse_z.
 * ------------------------------------------------------------------------- */

VkPipeline mop_vk_create_overlay_pipeline(struct MopRhiDevice *dev) {
#if defined(MOP_VK_HAS_OVERLAY_SHADERS)
  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->fullscreen_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->overlay_frag,
          .pName = "main",
      },
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

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

  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };

  /* Premultiplied alpha blending: ONE, ONE_MINUS_SRC_ALPHA
   * (shader outputs premultiplied RGB from accumulated SDF compositing) */
  VkPipelineColorBlendAttachmentState blend_att = {
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_att,
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
      .layout = dev->overlay_pipeline_layout,
      .renderPass = dev->overlay_render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] overlay pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
#else
  (void)dev;
  return VK_NULL_HANDLE;
#endif
}

/* -------------------------------------------------------------------------
 * Analytical grid pipeline (fullscreen triangle + grid fragment shader)
 *
 * Same render pass as overlay (alpha blend onto LDR color).
 * Push constants carry homography, VP matrix rows, grid params, colors.
 * Descriptor set: binding 0 = depth sampler.
 * ------------------------------------------------------------------------- */

VkPipeline mop_vk_create_grid_pipeline(struct MopRhiDevice *dev) {
#if defined(MOP_VK_HAS_OVERLAY_SHADERS)
  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->fullscreen_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->grid_frag,
          .pName = "main",
      },
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

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

  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };

  VkPipelineColorBlendAttachmentState blend_att = {
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_att,
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
      .layout = dev->grid_pipeline_layout,
      .renderPass = dev->overlay_render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] grid pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
#else
  (void)dev;
  return VK_NULL_HANDLE;
#endif
}

/* =========================================================================
 * Bloom render pass (R16G16B16A16_SFLOAT, single color attachment)
 *
 * Used for both extract and blur passes. Each invocation writes to a
 * bloom mip image. Final layout is SHADER_READ_ONLY so the result
 * can be sampled by the next pass or by tonemap.
 * ========================================================================= */

VkResult mop_vk_create_bloom_render_pass(VkDevice device, VkRenderPass *out) {
  VkAttachmentDescription attachment = {
      .format = VK_FORMAT_R16G16B16A16_SFLOAT,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
  };

  /* No BY_REGION_BIT: bloom chain uses different-sized framebuffers across
   * passes; per-region dependencies can cause incomplete tile flushes on
   * MoltenVK / Apple TBDR GPUs. */
  VkSubpassDependency deps[2] = {
      {
          .srcSubpass = VK_SUBPASS_EXTERNAL,
          .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask =
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      },
      {
          .srcSubpass = 0,
          .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      },
  };

  VkRenderPassCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 2,
      .pDependencies = deps,
  };

  return vkCreateRenderPass(device, &ci, NULL, out);
}

/* =========================================================================
 * Bloom extract pipeline (fullscreen triangle + threshold extraction)
 * ========================================================================= */

VkPipeline mop_vk_create_bloom_extract_pipeline(struct MopRhiDevice *dev) {
  if (!dev->bloom_extract_frag || !dev->fullscreen_vert)
    return VK_NULL_HANDLE;

  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->fullscreen_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->bloom_extract_frag,
          .pName = "main",
      },
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

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

  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };

  VkPipelineColorBlendAttachmentState blend_att = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_att,
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
      .layout = dev->bloom_pipeline_layout,
      .renderPass = dev->bloom_render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] bloom extract pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

/* =========================================================================
 * Bloom blur pipeline (fullscreen triangle + 9-tap Gaussian)
 * ========================================================================= */

VkPipeline mop_vk_create_bloom_blur_pipeline(struct MopRhiDevice *dev) {
  if (!dev->bloom_blur_frag || !dev->fullscreen_vert)
    return VK_NULL_HANDLE;

  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->fullscreen_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->bloom_blur_frag,
          .pName = "main",
      },
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

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

  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };

  VkPipelineColorBlendAttachmentState blend_att = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_att,
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
      .layout = dev->bloom_pipeline_layout,
      .renderPass = dev->bloom_render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] bloom blur pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

/* =========================================================================
 * Bloom upsample render pass (LOAD_OP_LOAD for additive blend)
 *
 * Same as bloom render pass but preserves existing content so the upsample
 * uses a two-texture shader to combine the smaller mip blur with the
 * current level's downsample content.  Uses LOAD_OP_CLEAR render pass
 * to avoid LOAD_OP_LOAD which fails on MoltenVK / TBDR GPUs.
 * ========================================================================= */

VkPipeline mop_vk_create_bloom_upsample_pipeline(struct MopRhiDevice *dev) {
  if (!dev->bloom_upsample_frag || !dev->fullscreen_vert ||
      !dev->bloom_render_pass || !dev->bloom_upsample_pl_layout)
    return VK_NULL_HANDLE;

  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->fullscreen_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->bloom_upsample_frag,
          .pName = "main",
      },
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

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

  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };

  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };

  /* No blending — shader combines both inputs and writes final color */
  VkPipelineColorBlendAttachmentState blend_att = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_att,
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
      .layout = dev->bloom_upsample_pl_layout,
      .renderPass = dev->bloom_render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] bloom upsample pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

/* =========================================================================
 * SSAO render pass (R8_UNORM, single color attachment)
 * ========================================================================= */

VkResult mop_vk_create_ssao_render_pass(VkDevice device, VkRenderPass *out) {
  VkAttachmentDescription attachment = {
      .format = VK_FORMAT_R8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };

  VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
  };

  /* Two dependencies to ensure correct read-after-write synchronization:
   *   EXTERNAL → 0: previous pass's color writes visible to our fragment reads
   *   0 → EXTERNAL: our color writes visible to next pass's fragment reads */
  VkSubpassDependency deps[2] = {
      {
          .srcSubpass = VK_SUBPASS_EXTERNAL,
          .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask =
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      },
      {
          .srcSubpass = 0,
          .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      },
  };

  VkRenderPassCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 2,
      .pDependencies = deps,
  };

  return vkCreateRenderPass(device, &ci, NULL, out);
}

/* =========================================================================
 * SSAO pipeline (fullscreen triangle + hemisphere kernel sampling)
 * ========================================================================= */

VkPipeline mop_vk_create_ssao_pipeline(struct MopRhiDevice *dev) {
  if (!dev->ssao_frag || !dev->fullscreen_vert)
    return VK_NULL_HANDLE;

  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->fullscreen_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->ssao_frag,
          .pName = "main",
      },
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                     VK_DYNAMIC_STATE_SCISSOR};
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
  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };
  VkPipelineColorBlendAttachmentState blend_att = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT,
  };
  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_att,
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
      .layout = dev->ssao_pipeline_layout,
      .renderPass = dev->ssao_render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] SSAO pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

/* =========================================================================
 * SSAO blur pipeline (fullscreen triangle + bilateral blur)
 * ========================================================================= */

VkPipeline mop_vk_create_ssao_blur_pipeline(struct MopRhiDevice *dev) {
  if (!dev->ssao_blur_frag || !dev->fullscreen_vert)
    return VK_NULL_HANDLE;

  VkPipelineShaderStageCreateInfo stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = dev->fullscreen_vert,
          .pName = "main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = dev->ssao_blur_frag,
          .pName = "main",
      },
  };

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                     VK_DYNAMIC_STATE_SCISSOR};
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
  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
  };
  VkPipelineColorBlendAttachmentState blend_att = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT,
  };
  VkPipelineColorBlendStateCreateInfo color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_att,
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
      .layout = dev->ssao_pipeline_layout,
      .renderPass = dev->ssao_render_pass,
      .subpass = 0,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &ci,
                                         NULL, &pipeline);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK] SSAO blur pipeline creation failed: %d", r);
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

#endif /* MOP_HAS_VULKAN */
