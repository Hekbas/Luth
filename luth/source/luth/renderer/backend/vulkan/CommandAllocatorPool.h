#pragma once

#include "luth/core/types/LuthTypes.h"
#include "CommandAllocator.h"
#include "luth/jobs/SpinLock.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    // Thread-safe pool of CommandAllocator instances. Fibers Acquire one to record commands and
    // Release it when done. Released allocators are not reset at Release time; they're reset only
    // when the owning frame is recycled past the GPU fence, so V3 thread-affinity rules stay safe
    // for any in-flight command buffers the GPU is still consuming.

    class CommandAllocatorPool
    {
    public:
        CommandAllocatorPool(u32 queueFamilyIndex);
        ~CommandAllocatorPool();

        void Init();
        void Shutdown();

        // Acquires a CommandAllocator for the current thread/fiber; creates a new one if none are available.
        CommandAllocator* Acquire();

        // Returns an allocator to the pool once the fiber is done recording for the frame.
        void Release(CommandAllocator* allocator);

        // Resets all allocators. WARNING: only safe when no allocator is in use (e.g. after a GPU wait).
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
