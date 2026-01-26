#pragma once

#include "luth/core/LuthTypes.h"
#include <atomic>

namespace Luth::JobSystem
{
    struct Fiber;

    // ===================================================================================
    // Lock-Free Atomic Counter
    // ===================================================================================
    
    struct AtomicCounter
    {
        // The value of the counter.
        // When this reaches 0, the counter is considered "signaled".
        std::atomic<u32> Value = 0;
        
        // Lock-Free Stack of waiting fibers.
        // When a fiber waits on this counter, it pushes itself onto this list.
        // When the counter hits 0, the decrementing thread pops all fibers and wakes them.
        std::atomic<Fiber*> WaitingListHead = nullptr;

        AtomicCounter(u32 initialValue = 0) : Value(initialValue), WaitingListHead(nullptr) {}

        // Non-copyable to prevent accidental atomic copies
        AtomicCounter(const AtomicCounter&) = delete;
        AtomicCounter& operator=(const AtomicCounter&) = delete;
    };
}
