#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/Fiber.h"
#include <atomic>

namespace Luth::JobSystem
{
    // ===================================================================================
    // Atomic Counter (Fiber Synchronization)
    // ===================================================================================
    
    struct AtomicCounter
    {
        std::atomic<u32> Value = 0;
        
        // Lock-Free Stack of Waiting Fibers
        std::atomic<Fiber*> WaitingListHead = nullptr;

        AtomicCounter(u32 initialValue = 0) : Value(initialValue), WaitingListHead(nullptr) {}

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
        
        void Reset(u32 value = 0)
        {
            Value.store(value, std::memory_order_relaxed);
            WaitingListHead.store(nullptr, std::memory_order_relaxed);
        }

        // 1.2 Mechanism: Add a fiber to the wait list (Lock-Free Push)
        void AddWaitingFiber(Fiber* fiber)
        {
            Fiber* oldHead = WaitingListHead.load(std::memory_order_relaxed);
            do
            {
                fiber->NextWaiting = oldHead;
            } 
            while (!WaitingListHead.compare_exchange_weak(oldHead, fiber, 
                                                          std::memory_order_release, 
                                                          std::memory_order_relaxed));
        }

        // 1.2 Mechanism: Retrieve all waiting fibers (Lock-Free Pop All)
        // Returns the head of the linked list. The caller must traverse NextWaiting.
        Fiber* TakeAllWaitingFibers()
        {
            // Atomically detach the entire list
            return WaitingListHead.exchange(nullptr, std::memory_order_acquire);
        }
    };
}
