#pragma once

#include "luth/core/LuthTypes.h"
#include <atomic>
#include <functional>

namespace Luth::JobSystem
{
    struct JobArgs
    {
        u32 jobIndex;
        u32 groupIndex;
        void* data;
    };

    // Standard job function signature
    using JobFunction = void(*)(JobArgs);

    // Synchronization primitive
    struct Counter
    {
        std::atomic<u32> value = 0;
    };

    struct Stats
    {
        u32 ThreadCount;
        u32 TotalFibers;
        u32 FreeFibers;
        u32 PeakFibers;
        u32 QueueSize;
    };

    // Fiber Local Storage (FLS)
    // This struct travels with the Fiber, not the OS Thread.
    struct JobContext
    {
        // Pointers to per-frame allocators will go here
        void* FrameAllocator = nullptr; 
        void* CommandAllocator = nullptr;
        u32 ThreadIndex = 0;
    };

    // Lifecycle
    void Init(u32 numThreads = 0); 
    void Shutdown();
    void ResetFrameStats();

    // Run a single task
    void Execute(JobFunction function, void* data = nullptr, Counter* counter = nullptr);

    // Run multiple tasks (data parallelism)
    void Dispatch(u32 jobCount, u32 groupSize, JobFunction function, void* data = nullptr, Counter* counter = nullptr);

    // Fiber-aware wait
    void WaitForCounter(Counter* counter, u32 targetValue = 0);

    // Returns true if the counter has not reached the target value
    bool IsBusy(const Counter* counter);
    
    Stats GetStats();

    // Access the current Fiber's context
    JobContext* GetCurrentJobContext();
}
