#pragma once

#include "luth/core/LuthTypes.h"
#include <atomic>

namespace Luth::JobSystem
{
    // ===================================================================================
    // Atomic Counter (Fiber Synchronization)
    // ===================================================================================
    // Used to track job completion.
    // Fibers wait on this counter reaching a target value (usually 0).
    
    struct AtomicCounter
    {
        std::atomic<u32> Value = 0;

        AtomicCounter(u32 initialValue = 0) : Value(initialValue) {}

        // Increment the counter (e.g., adding a job)
        void Increment(u32 count = 1)
        {
            Value.fetch_add(count, std::memory_order_relaxed);
        }

        // Decrement the counter (e.g., job finished)
        // Returns true if the counter reached 0 (or target)
        bool Decrement(u32 count = 1)
        {
            u32 prev = Value.fetch_sub(count, std::memory_order_release);
            return prev == count;
        }

        u32 Get() const
        {
            return Value.load(std::memory_order_acquire);
        }
        
        // Reset the counter
        void Reset(u32 value = 0)
        {
            Value.store(value, std::memory_order_relaxed);
        }
    };
}
