#pragma once

#include "luth/core/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    class VKComputePipeline
    {
    public:
        VKComputePipeline(const std::vector<u32>& computeSpv,
                          const std::vector<VkDescriptorSetLayout>& layouts,
                          const std::vector<VkPushConstantRange>& pushConstantRanges = {});
        ~VKComputePipeline();

        void Bind(VkCommandBuffer cmd) const;
        VkPipelineLayout GetLayout() const { return m_PipelineLayout; }

    private:
        VkPipeline       m_Pipeline       = VK_NULL_HANDLE;
        VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
        VkDevice         m_Device         = VK_NULL_HANDLE;
    };
}
