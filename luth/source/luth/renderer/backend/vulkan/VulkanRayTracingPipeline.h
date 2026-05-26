#pragma once

#include "luth/core/types/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    // One shader stage in an RT pipeline (raygen / miss / chit / ahit / intersection / callable).
    struct RayTracingShaderStage
    {
        VkShaderStageFlagBits stage      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        std::vector<u32>      spirv;
        const char*           entryPoint = "main";
    };

    // One shader group — maps stage index(es) to a group consumed by vkCmdTraceRaysKHR.
    // GENERAL groups reference exactly one shader (raygen/miss/callable). Hit groups
    // bundle up to one of {closestHit, anyHit, intersection}.
    struct RayTracingShaderGroup
    {
        VkRayTracingShaderGroupTypeKHR type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        u32 generalShader      = VK_SHADER_UNUSED_KHR;
        u32 closestHitShader   = VK_SHADER_UNUSED_KHR;
        u32 anyHitShader       = VK_SHADER_UNUSED_KHR;
        u32 intersectionShader = VK_SHADER_UNUSED_KHR;
    };

    // Stage + group descriptor pair handed to VKRayTracingPipeline ctor.
    struct RayTracingStages
    {
        std::vector<RayTracingShaderStage> stages;
        std::vector<RayTracingShaderGroup> groups;
    };

    // RAII RT pipeline wrapper — RT analog of VKComputePipeline. Owns the VkPipeline + its
    // layout. Caches the shader-group handles (vkGetRayTracingShaderGroupHandlesKHR result)
    // so the SBT builder can copy them into its mapped buffer without re-querying.
    class VKRayTracingPipeline
    {
    public:
        VKRayTracingPipeline(const RayTracingStages& stages,
                             const std::vector<VkDescriptorSetLayout>& layouts,
                             const std::vector<VkPushConstantRange>& pushConstantRanges = {},
                             u32 maxRecursionDepth = 1);
        ~VKRayTracingPipeline();

        VKRayTracingPipeline(const VKRayTracingPipeline&) = delete;
        VKRayTracingPipeline& operator=(const VKRayTracingPipeline&) = delete;

        void Bind(VkCommandBuffer cmd) const;

        VkPipeline       GetPipeline() const { return m_Pipeline; }
        VkPipelineLayout GetLayout()   const { return m_PipelineLayout; }
        u32              GetGroupCount()    const { return m_GroupCount; }
        const std::vector<u8>& GetGroupHandles() const { return m_GroupHandles; }

    private:
        VkPipeline       m_Pipeline       = VK_NULL_HANDLE;
        VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
        VkDevice         m_Device         = VK_NULL_HANDLE;
        u32              m_GroupCount     = 0;
        std::vector<u8>  m_GroupHandles;
    };
}
