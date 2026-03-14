#pragma once

#include "luth/core/LuthTypes.h"
#include <atomic>

namespace Luth::JobSystem
{
    struct Fiber;

    // ===================================================================================
    // Atomic Counter with Busy Bit and SpinLock
    // Value: Bits 1-31 = Count, Bit 0 = Busy Flag
    // Lock: Protects WaitingListHead
    // ===================================================================================
    
    struct AtomicCounter
    {
        std::atomic<u32> Value;
        std::atomic_flag Lock = ATOMIC_FLAG_INIT; 
        Fiber* WaitingListHead = nullptr;

        AtomicCounter() : Value(0), WaitingListHead(nullptr) {}
        AtomicCounter(u32 initialValue) : Value(initialValue << 1), WaitingListHead(nullptr) {}

        // Non-copyable
        AtomicCounter(const AtomicCounter&) = delete;
        AtomicCounter& operator=(const AtomicCounter&) = delete;
    };
}
