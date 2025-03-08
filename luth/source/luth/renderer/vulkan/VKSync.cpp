#include "Luthpch.h"
#include "luth/renderer/vulkan/VKSync.h"
#include "luth/renderer/vulkan/VKCommon.h"

namespace Luth
{
    VKSync::VKSync(VkDevice device, size_t maxFramesInFlight)
        : m_Device(device), m_MaxFrames(maxFramesInFlight)
    {
        m_Frames.resize(m_MaxFrames);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (auto& frame : m_Frames) {
            VK_CHECK_RESULT(vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &frame.imageAvailable),
                "Failed to create semaphore!");
            VK_CHECK_RESULT(vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &frame.renderFinished),
                "Failed to create semaphore!");
            VK_CHECK_RESULT(vkCreateFence(m_Device, &fenceInfo, nullptr, &frame.inFlightFence),
                "Failed to create fence!");
        }
    }

    VKSync::~VKSync()
    {
        for (auto& frame : m_Frames) {
            vkDestroySemaphore(m_Device, frame.imageAvailable, nullptr);
            vkDestroySemaphore(m_Device, frame.renderFinished, nullptr);
            vkDestroyFence(m_Device, frame.inFlightFence, nullptr);
        }
    }
}
