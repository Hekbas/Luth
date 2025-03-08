#include "Luthpch.h"
#include "luth/renderer/vulkan/VKFramebuffer.h"
#include "luth/renderer/vulkan/VKCommon.h"

namespace Luth
{
    VKFramebuffer::VKFramebuffer(VkDevice device,
        VkRenderPass renderPass,
        VkImageView imageView,
        VkExtent2D extent)
        : m_Device(device)
    {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &imageView;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        VK_CHECK_RESULT(vkCreateFramebuffer(m_Device, &framebufferInfo, nullptr, &m_Framebuffer),
            "Failed to create framebuffer!");
    }

    VKFramebuffer::~VKFramebuffer()
    {
        if (m_Framebuffer) {
            vkDestroyFramebuffer(m_Device, m_Framebuffer, nullptr);
        }
    }
}
