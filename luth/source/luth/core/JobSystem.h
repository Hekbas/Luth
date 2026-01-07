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
    };

    // Standard job function signature
    using JobFunction = std::function<void(JobArgs)>;

    // Synchronization primitive
    struct Counter
    {
        std::atomic<u32> value = 0;
    };

    // Lifecycle
    void Init(u32 numThreads = 0); // 0 = Auto-detect
    void Shutdown();

    // Run a single task
    // counter: Optional counter to increment. If provided, you can Wait() on it.
    void Execute(const JobFunction& job, Counter* counter = nullptr);

    // Run multiple tasks (good for data parallelism)
    // jobCount: Total number of items to process
    // groupSize: How many items per job (chunking)
    void Dispatch(u32 jobCount, u32 groupSize, const JobFunction& job, Counter* counter = nullptr);

    // Blocks the current thread (by running other jobs) until counter reaches targetValue
    void WaitForCounter(Counter* counter, u32 targetValue = 0);

    // Returns true if the counter has not reached the target value
    bool IsBusy(const Counter* counter);
}
