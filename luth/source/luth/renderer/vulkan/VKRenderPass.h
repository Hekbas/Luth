#pragma once
#include <vulkan/vulkan.h>

namespace Luth
{
    class VKRenderPass
    {
    public:
        VKRenderPass(VkDevice device, VkFormat swapchainFormat);
        ~VKRenderPass();

        VkRenderPass GetHandle() const { return m_RenderPass; }

    private:
        VkDevice m_Device;
        VkRenderPass m_RenderPass;
    };
}
