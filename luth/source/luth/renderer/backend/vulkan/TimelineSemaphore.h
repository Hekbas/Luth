#pragma once

#include "luth/core/LuthTypes.h"
#include <vulkan/vulkan.h>

namespace Luth
{
    class TimelineSemaphore
    {
    public:
        // Default constructor does NOT initialize the Vulkan object.
        // You must call Init() manually.
        TimelineSemaphore() = default;

        // Constructor that initializes the semaphore immediately.
        // VulkanContext must be initialized before calling this.
        explicit TimelineSemaphore(u64 initialValue);

        ~TimelineSemaphore();

        void Init(u64 initialValue = 0);
        void Shutdown();

        // Signal the semaphore from the CPU (Host)
        void Signal(u64 value);

        // Wait for the semaphore on the CPU (Host)
        // Returns true if successful, false on timeout
        bool Wait(u64 value, u64 timeoutNanoseconds = UINT64_MAX);

        // Get the current counter value
        u64 GetValue() const;

        VkSemaphore GetHandle() const { return m_Semaphore; }

    private:
        VkSemaphore m_Semaphore = VK_NULL_HANDLE;
    };
}
