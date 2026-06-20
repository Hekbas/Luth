#include "luthpch.h"
#include <chrono>
#include "VulkanComputePipeline.h"
#include "VulkanContext.h"
#include "PipelineCache.h"
#include "luth/core/diagnostics/Log.h"

namespace Luth
{
    VKComputePipeline::VKComputePipeline(const std::vector<u32>& computeSpv,
                                         const std::vector<VkDescriptorSetLayout>& layouts,
                                         const std::vector<VkPushConstantRange>& pushConstantRanges)
    {
        LH_PROFILE_FUNCTION();

        m_Device = VulkanContext::Get().GetDevice();

        // Shader module
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = computeSpv.size() * sizeof(u32);
        moduleInfo.pCode    = computeSpv.data();

        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_Device, &moduleInfo, nullptr, &shaderModule) != VK_SUCCESS)
        {
            LH_CORE_ERROR("VKComputePipeline: Failed to create shader module!");
            return;
        }

        // Pipeline layout
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount         = (u32)layouts.size();
        layoutInfo.pSetLayouts            = layouts.data();
        layoutInfo.pushConstantRangeCount = (u32)pushConstantRanges.size();
        layoutInfo.pPushConstantRanges    = pushConstantRanges.data();

        if (vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("VKComputePipeline: Failed to create pipeline layout!");
            vkDestroyShaderModule(m_Device, shaderModule, nullptr);
            return;
        }

        // Compute pipeline
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.layout       = m_PipelineLayout;
        pipelineInfo.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shaderModule;
        pipelineInfo.stage.pName  = "main";

        const auto pcStart = std::chrono::high_resolution_clock::now();
        VkResult pipeRes = vkCreateComputePipelines(m_Device, PipelineCache::Get(), 1, &pipelineInfo, nullptr, &m_Pipeline);
        PipelineCache::RecordCompile(std::chrono::duration<f64, std::milli>(std::chrono::high_resolution_clock::now() - pcStart).count());
        if (pipeRes != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("VKComputePipeline: Failed to create compute pipeline!");
        }

        vkDestroyShaderModule(m_Device, shaderModule, nullptr);
    }

    VKComputePipeline::~VKComputePipeline()
    {
        vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
        vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
    }

    void VKComputePipeline::Bind(VkCommandBuffer cmd) const
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
    }
}
