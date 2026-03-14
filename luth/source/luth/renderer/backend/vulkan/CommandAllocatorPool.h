#pragma once

#include "luth/core/LuthTypes.h"
#include "CommandAllocator.h"
#include "luth/core/SpinLock.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    // ===================================================================================
    // Command Allocator Pool (Thread-Safe Global Pool)
    // ===================================================================================
    // Manages a pool of CommandAllocator objects.
    // Fibers acquire an allocator to record commands and release it when done.
    // Released allocators are NOT reset immediately; they are reset when the frame they were used for is recycled.
    
    class CommandAllocatorPool
    {
    public:
        CommandAllocatorPool(u32 queueFamilyIndex);
        ~CommandAllocatorPool();

        void Init();
        void Shutdown();

        // Acquires a CommandAllocator for the current thread/fiber.
        // If no pool is available, a new one is created.
        CommandAllocator* Acquire();

        // Returns an allocator to the pool.
        // Should be called when the fiber is done recording for the frame.
        void Release(CommandAllocator* allocator);

        // Resets all allocators in the pool.
        // WARNING: Only call this when you are sure NO allocators are in use (e.g. after GPU wait).
        void ResetAll();

    private:
        CommandAllocator* CreateAllocator();

        u32 m_QueueFamilyIndex = 0;
        VkDevice m_Device = VK_NULL_HANDLE;

        SpinLock m_Lock;
        std::vector<CommandAllocator*> m_AvailableAllocators;
        std::vector<CommandAllocator*> m_AllAllocators;
    };
}
