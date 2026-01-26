#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/AtomicCounter.h"
#include <functional>
#include <concepts>

namespace Luth
{
    struct FrameParams;
    class CommandAllocatorPool;
    namespace Memory { class TaggedPageAllocator; }
}

namespace Luth::JobSystem
{
    struct JobArgs
    {
        u32 jobIndex;
        u32 groupIndex;
        void* data;
    };

    using JobFunction = void(*)(JobArgs);
    using Counter = AtomicCounter;

    struct Stats
    {
        u32 ThreadCount;
        u32 TotalFibers;
        u32 FreeFibers;
        u32 PeakFibers;
        u32 QueueSize;
    };

    // Fiber Local Storage (FLS)
    struct JobContext
    {
        // Memory
        Memory::TaggedPageAllocator* Allocator = nullptr;
        // Memory::TaggedPageAllocator::ThreadCache AllocatorCache; // Per-fiber cache for lock-free alloc

        // Frame Data
        const FrameParams* Params = nullptr; // Read-only params for the current job's frame context

        // Command Recording
        CommandAllocatorPool* CommandPool = nullptr;
        void* CurrentCommandAllocator = nullptr; // Pointer to the thread-local CommandAllocator (if acquired)

        // Metadata
        u32 ThreadIndex = 0;
        u32 FiberID = 0;
    };

    // ===================================================================================
    // API
    // ===================================================================================

    // Lifecycle
    void Init(u32 numThreads = 0); 
    void Shutdown();
    void ResetFrameStats();
    
    // Main Thread Loop (OS-Aware)
    void ExecuteMainThreadLoop();

    // Run a single task
    void Execute(JobFunction function, void* data = nullptr, Counter* counter = nullptr);

    // Run multiple tasks (data parallelism)
    void Dispatch(u32 jobCount, u32 groupSize, JobFunction function, void* data = nullptr, Counter* counter = nullptr);

    // Fiber-aware wait
    // If targetValue == 0, waits until counter == 0.
    void WaitForCounter(Counter* counter, u32 targetValue = 0);
    
    // Yield the current fiber to the scheduler.
    void YieldFiber();

    // Returns true if the counter has not reached the target value
    bool IsBusy(const Counter* counter);
    
    Stats GetStats();
    
    // Returns the index of the current worker thread (0 to N-1)
    u32 GetWorkerThreadId();

    // Access the current Fiber's context
    JobContext* GetCurrentJobContext();

    // Set the global command pool for the current thread (used by Renderer)
    void SetGlobalCommandPool(CommandAllocatorPool* pool);
}
