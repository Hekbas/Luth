#pragma once

#include "luth/core/types/LuthTypes.h"
#include <atomic>

namespace Luth::JobSystem
{
    struct Fiber;

    // Atomic Counter with Busy Bit and SpinLock.
    // Value: Bits 1-31 = Count, Bit 0 = Busy Flag. Lock: Protects WaitingListHead.

    struct AtomicCounter
    {
        std::atomic<u32> Value;
        std::atomic_flag Lock = ATOMIC_FLAG_INIT; 
        Fiber* WaitingListHead = nullptr;

        AtomicCounter() : Value(0), WaitingListHead(nullptr) {}
        AtomicCounter(u32 initialValue) : Value(initialValue << 1), WaitingListHead(nullptr) {}

        AtomicCounter(const AtomicCounter&) = delete;
        AtomicCounter& operator=(const AtomicCounter&) = delete;

        // Public counter primitives. Execute/Dispatch manage the counter internally; these are for external
        // schedulers (e.g. the Jolt JobSystem adapter) that need to manipulate it directly while still
        // cooperating with WaitForCounter's V5 fiber-yield path. Increment is a plain shifted add. Decrement
        // routes through the same wake-waiters path as job completion, including the busy-bit serialization
        // at count 0; n=1 is the common case, n>1 iterates.
        void Increment(u32 n = 1);
        void Decrement(u32 n = 1);
    };
}
