#pragma once

#include "luth/core/types/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    // Wraps a VkCommandPool and caches the VkCommandBuffers it has handed out so they can be
    // reused next frame after pool reset. Owned by a single Fiber during recording — not
    // thread-safe by design (V3 forbids fiber yield while recording, so single-thread is fine).

    class CommandAllocator
    {
    public:
        CommandAllocator(VkDevice device, VkCommandPool pool);
        ~CommandAllocator();

        // Get a fresh command buffer from the cache or allocate new
        VkCommandBuffer GetBuffer(VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_SECONDARY);

        // Reset the underlying pool and clear cache index
        void Reset();

        VkCommandPool GetPool() const { return m_Pool; }

    private:
        VkDevice m_Device;
        VkCommandPool m_Pool;
        std::vector<VkCommandBuffer> m_Buffers;
        u32 m_UsedCount = 0;
    };
}
