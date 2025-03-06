#pragma once

#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    class VKFramebuffer
    {
    public:
        VKFramebuffer(VkDevice device,
            VkRenderPass renderPass,
            VkImageView imageView,
            VkExtent2D extent);
        ~VKFramebuffer();

        VkFramebuffer GetHandle() const { return m_Framebuffer; }

    private:
        VkDevice m_Device;
        VkFramebuffer m_Framebuffer;
    };
}
