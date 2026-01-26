#pragma once

#include "luth/core/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    // ===================================================================================
    // Command Allocator (Thread-Local Command Buffer Cache)
    // ===================================================================================
    // Wraps a VkCommandPool and manages a cache of VkCommandBuffers.
    // Owned by a Fiber during recording.
    // Not thread-safe (intended for single-thread use).
    
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
