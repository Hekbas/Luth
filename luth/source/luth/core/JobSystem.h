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
    // We use a raw function pointer + void* for maximum performance (no std::function overhead)
    using JobFunction = void(*)(JobArgs);

    // Synchronization primitive
    struct Counter
    {
        std::atomic<u32> value = 0;
    };

    // Lifecycle
    void Init(u32 numThreads = 0); 
    void Shutdown();

    // Run a single task
    void Execute(JobFunction function, void* data = nullptr, Counter* counter = nullptr);

    // Run multiple tasks (data parallelism)
    void Dispatch(u32 jobCount, u32 groupSize, JobFunction function, void* data = nullptr, Counter* counter = nullptr);

    // Fiber-aware wait
    // If the counter is not zero, the current fiber is suspended and a new job is picked up.
    void WaitForCounter(Counter* counter, u32 targetValue = 0);

    // Returns true if the counter has not reached the target value
    bool IsBusy(const Counter* counter);
    
    // Helper for lambdas (less performant but convenient)
    // Note: This requires allocating the lambda on the heap or LinearAllocator if it captures state!
    template<typename F>
    void ExecuteLambda(F&& lambda, Counter* counter = nullptr)
    {
        // TODO: Allocate lambda storage
        // For now, we don't support capturing lambdas directly without an allocator.
        // Users should use Execute with a static function and void* data.
    }
}
