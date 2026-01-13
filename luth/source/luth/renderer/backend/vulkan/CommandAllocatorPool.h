#pragma once

#include "luth/core/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>

namespace Luth
{
    // Manages a pool of VkCommandPools to be used by fibers.
    // Since fibers migrate between threads, we cannot rely on thread_local command pools.
    // Instead, a fiber requests an allocator from this pool, records commands, and returns it.
    class CommandAllocatorPool
    {
    public:
        CommandAllocatorPool(u32 queueFamilyIndex);
        ~CommandAllocatorPool();

        void Init();
        void Shutdown();

        // Acquires a command pool for the current thread/fiber.
        // If no pool is available, a new one is created.
        VkCommandPool AcquirePool();

        // Returns a pool to the available list.
        // Should be called when the fiber is done recording for the frame.
        void ReleasePool(VkCommandPool pool);

        // Resets all pools. Should be called at the start of a frame.
        void ResetAll();

    private:
        VkCommandPool CreatePool();

        u32 m_QueueFamilyIndex = 0;
        VkDevice m_Device = VK_NULL_HANDLE;

        std::mutex m_Lock;
        std::vector<VkCommandPool> m_AvailablePools;
        std::vector<VkCommandPool> m_AllPools;
    };
}
