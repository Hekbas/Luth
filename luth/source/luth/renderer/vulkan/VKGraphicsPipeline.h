#pragma once

#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    class VKGraphicsPipeline
    {
    public:
        VKGraphicsPipeline(VkDevice device, VkExtent2D swapchainExtent, VkRenderPass renderPass,
            const std::vector<VkVertexInputBindingDescription>& bindingDescriptions,
            const std::vector<VkVertexInputAttributeDescription>& attributeDescriptions);
        ~VKGraphicsPipeline();

        VkPipeline GetHandle() const { return m_Pipeline; }
        VkPipelineLayout GetLayout() const { return m_PipelineLayout; }

    private:
        void CreateShaderModule(const std::vector<char>& code, VkShaderModule* shaderModule);

        VkDevice m_Device;
        VkPipeline m_Pipeline;
        VkPipelineLayout m_PipelineLayout;
    };
}
