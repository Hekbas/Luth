#include "luthpch.h"
#include <chrono>
#include "VulkanRayTracingPipeline.h"
#include "VulkanContext.h"
#include "PipelineCache.h"
#include "luth/core/diagnostics/Log.h"

namespace Luth
{
    VKRayTracingPipeline::VKRayTracingPipeline(const RayTracingStages& stages,
                                               const std::vector<VkDescriptorSetLayout>& layouts,
                                               const std::vector<VkPushConstantRange>& pushConstantRanges,
                                               u32 maxRecursionDepth)
    {
        auto& ctx = VulkanContext::Get();
        m_Device  = ctx.GetDevice();

        if (stages.stages.empty() || stages.groups.empty())
        {
            LH_CORE_CRITICAL("VKRayTracingPipeline: empty stages/groups (need ≥1 raygen + ≥1 group)");
            return;
        }

        // Shader modules — one per stage; destroyed after vkCreateRayTracingPipelinesKHR returns.
        std::vector<VkShaderModule> modules(stages.stages.size(), VK_NULL_HANDLE);
        std::vector<VkPipelineShaderStageCreateInfo> stageInfos(stages.stages.size());

        for (size_t i = 0; i < stages.stages.size(); ++i)
        {
            const auto& s = stages.stages[i];

            VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            mi.codeSize = s.spirv.size() * sizeof(u32);
            mi.pCode    = s.spirv.data();
            if (vkCreateShaderModule(m_Device, &mi, nullptr, &modules[i]) != VK_SUCCESS)
            {
                LH_CORE_CRITICAL("VKRayTracingPipeline: vkCreateShaderModule failed at stage {}", i);
                for (auto m : modules) if (m) vkDestroyShaderModule(m_Device, m, nullptr);
                return;
            }

            stageInfos[i].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stageInfos[i].stage  = s.stage;
            stageInfos[i].module = modules[i];
            stageInfos[i].pName  = s.entryPoint;
        }

        // Pipeline layout
        VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        lci.setLayoutCount         = (u32)layouts.size();
        lci.pSetLayouts            = layouts.data();
        lci.pushConstantRangeCount = (u32)pushConstantRanges.size();
        lci.pPushConstantRanges    = pushConstantRanges.data();
        if (vkCreatePipelineLayout(m_Device, &lci, nullptr, &m_PipelineLayout) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("VKRayTracingPipeline: vkCreatePipelineLayout failed");
            for (auto m : modules) if (m) vkDestroyShaderModule(m_Device, m, nullptr);
            return;
        }

        // Group create infos
        std::vector<VkRayTracingShaderGroupCreateInfoKHR> groupInfos(stages.groups.size());
        for (size_t i = 0; i < stages.groups.size(); ++i)
        {
            const auto& g = stages.groups[i];
            groupInfos[i].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            groupInfos[i].type               = g.type;
            groupInfos[i].generalShader      = g.generalShader;
            groupInfos[i].closestHitShader   = g.closestHitShader;
            groupInfos[i].anyHitShader       = g.anyHitShader;
            groupInfos[i].intersectionShader = g.intersectionShader;
        }

        // RT pipeline
        VkRayTracingPipelineCreateInfoKHR pi{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
        pi.stageCount                   = (u32)stageInfos.size();
        pi.pStages                      = stageInfos.data();
        pi.groupCount                   = (u32)groupInfos.size();
        pi.pGroups                      = groupInfos.data();
        pi.maxPipelineRayRecursionDepth = maxRecursionDepth;
        pi.layout                       = m_PipelineLayout;

        const auto pcStart = std::chrono::high_resolution_clock::now();
        VkResult res = ctx.GetRtFn().vkCreateRayTracingPipelinesKHR(
            m_Device, VK_NULL_HANDLE, PipelineCache::Get(), 1, &pi, nullptr, &m_Pipeline);
        PipelineCache::RecordCompile(std::chrono::duration<f64, std::milli>(std::chrono::high_resolution_clock::now() - pcStart).count());
        if (res != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("VKRayTracingPipeline: vkCreateRayTracingPipelinesKHR failed ({})", (int)res);
            for (auto m : modules) if (m) vkDestroyShaderModule(m_Device, m, nullptr);
            return;
        }

        m_GroupCount = (u32)groupInfos.size();

        // Cache shader-group handles for the SBT builder. handleSize is vendor-dependent
        // (NVIDIA Ampere/Ada: 32, AMD RDNA2+: 32) — total = handleSize × groupCount.
        const u32 handleSize = ctx.GetRtPipelineProperties().shaderGroupHandleSize;
        m_GroupHandles.resize(handleSize * m_GroupCount);
        res = ctx.GetRtFn().vkGetRayTracingShaderGroupHandlesKHR(
            m_Device, m_Pipeline, 0, m_GroupCount,
            m_GroupHandles.size(), m_GroupHandles.data());
        if (res != VK_SUCCESS)
            LH_CORE_CRITICAL("VKRayTracingPipeline: vkGetRayTracingShaderGroupHandlesKHR failed ({})", (int)res);

        for (auto m : modules) if (m) vkDestroyShaderModule(m_Device, m, nullptr);
    }

    VKRayTracingPipeline::~VKRayTracingPipeline()
    {
        if (m_Pipeline)       vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
        if (m_PipelineLayout) vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
    }

    void VKRayTracingPipeline::Bind(VkCommandBuffer cmd) const
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_Pipeline);
    }
}
