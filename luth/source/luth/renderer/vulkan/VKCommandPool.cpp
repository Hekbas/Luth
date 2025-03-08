#include "Luthpch.h"
#include "luth/renderer/vulkan/VKCommandPool.h"
#include "luth/renderer/vulkan/VKCommon.h"

namespace Luth
{
    VKCommandPool::VKCommandPool(VkDevice device, uint32_t queueFamilyIndex)
        : m_Device(device)
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VK_CHECK_RESULT(vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool),
            "Failed to create command pool!");
    }

    VKCommandPool::~VKCommandPool() {
        vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
    }
}
