#include "luthpch.h"
#include "VulkanPipeline.h"
#include "VulkanContext.h"
#include "PipelineCache.h"
#include "luth/core/diagnostics/Log.h"
#include <fstream>

namespace Luth
{
    VKPipeline::VKPipeline(const PipelineConfig& config, 
                           const std::vector<u32>& vertCode, 
                           const std::vector<u32>& fragCode,
                           const std::vector<VkDescriptorSetLayout>& layouts)
    {
        m_Device = VulkanContext::Get().GetDevice();
        CreatePipeline(config, vertCode, fragCode, layouts);
    }

    VKPipeline::~VKPipeline()
    {
        vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
        vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
    }

    void VKPipeline::Bind(VkCommandBuffer cmd)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
    }

    VkShaderModule VKPipeline::CreateShaderModule(const std::vector<u32>& code)
    {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size() * sizeof(u32);
        createInfo.pCode = code.data();

        VkShaderModule shaderModule;
        if (vkCreateShaderModule(m_Device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            LH_CORE_ERROR("Failed to create shader module!");
            return VK_NULL_HANDLE;
        }
        return shaderModule;
    }

    void VKPipeline::CreatePipeline(const PipelineConfig& config, 
                                    const std::vector<u32>& vertCode, 
                                    const std::vector<u32>& fragCode,
                                    const std::vector<VkDescriptorSetLayout>& layouts)
    {
        VkShaderModule vertShader = CreateShaderModule(vertCode);
        VkShaderModule fragShader = CreateShaderModule(fragCode);

        VkPipelineShaderStageCreateInfo shaderStages[] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertShader, "main", nullptr },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragShader, "main", nullptr }
        };

        // Dynamic Rendering Info
        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = (u32)config.colorFormats.size();
        renderingInfo.pColorAttachmentFormats = config.colorFormats.data();
        renderingInfo.depthAttachmentFormat = config.depthFormat;

        // Vertex Input (Empty for now, using Bindless buffers usually)
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = (u32)config.bindingDescriptions.size();
        vertexInputInfo.pVertexBindingDescriptions = config.bindingDescriptions.data();
        vertexInputInfo.vertexAttributeDescriptionCount = (u32)config.attributeDescriptions.size();
        vertexInputInfo.pVertexAttributeDescriptions = config.attributeDescriptions.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = config.topology;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // Viewport & Scissor (Dynamic)
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = config.polygonMode;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = config.cullMode;
        rasterizer.frontFace = config.frontFace;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = config.depthTest ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = config.depthWrite ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = config.depthCompareOp;

        // One blend state per color attachment (Vulkan requires matching count)
        std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(config.colorFormats.size());
        for (size_t i = 0; i < blendAttachments.size(); ++i)
        {
            auto& att = blendAttachments[i];
            att = {};
            att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            // Integer formats (R32_UINT, etc.) do not support color attachment blending
            bool isIntegerFormat = (config.colorFormats[i] == VK_FORMAT_R32_UINT  ||
                                    config.colorFormats[i] == VK_FORMAT_R32_SINT  ||
                                    config.colorFormats[i] == VK_FORMAT_R8_UINT   ||
                                    config.colorFormats[i] == VK_FORMAT_R16_UINT);

            att.blendEnable = (config.blendEnabled && !isIntegerFormat) ? VK_TRUE : VK_FALSE;
            if (att.blendEnable) {
                att.srcColorBlendFactor = config.srcColorBlendFactor;
                att.dstColorBlendFactor = config.dstColorBlendFactor;
                att.colorBlendOp = VK_BLEND_OP_ADD;
                att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                att.alphaBlendOp = VK_BLEND_OP_ADD;
            }
        }

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = (u32)blendAttachments.size();
        colorBlending.pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data();

        std::vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // Pipeline Layout
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = (u32)layouts.size();
        pipelineLayoutInfo.pSetLayouts = layouts.data();
        pipelineLayoutInfo.pushConstantRangeCount = (u32)config.pushConstantRanges.size();
        pipelineLayoutInfo.pPushConstantRanges = config.pushConstantRanges.data();

        if (vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create pipeline layout!");
        }

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo; // Dynamic Rendering
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = m_PipelineLayout;

        if (vkCreateGraphicsPipelines(m_Device, PipelineCache::Get(), 1, &pipelineInfo, nullptr, &m_Pipeline) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create graphics pipeline!");
        }

        vkDestroyShaderModule(m_Device, vertShader, nullptr);
        vkDestroyShaderModule(m_Device, fragShader, nullptr);
    }
}