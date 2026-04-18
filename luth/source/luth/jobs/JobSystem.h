#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/jobs/AtomicCounter.h"
#include <functional>
#include <cassert>

namespace Luth
{
    struct FrameParams;
    class CommandAllocatorPool;
    namespace Memory { class TaggedPageAllocator; }
}

namespace Luth::JobSystem
{
    // ===================================================================================
    // Job Descriptor
    // ===================================================================================

    struct JobArgs
    {
        u32 jobIndex;
        u32 groupIndex;
        void* data;
    };

    using JobFunction = void(*)(JobArgs);
    using Counter = AtomicCounter;

    // ===================================================================================
    // Job Priority
    // ===================================================================================

    enum class Priority : u8
    {
        High,   // Critical path: render recording, physics, audio
        Normal, // General gameplay, entity logic (per-thread Chase-Lev deque)
        Low     // Asset loading, background tasks
    };

    // ===================================================================================
    // Worker State (per-thread, for profiler visualization)
    // ===================================================================================

    enum class WorkerState : u8
    {
        Idle,
        Running,
        Stealing,
        Sleeping
    };

    // ===================================================================================
    // Runtime Stats
    // ===================================================================================

    static constexpr u32 MAX_WORKER_THREADS = 64;

    struct Stats
    {
        u32 ThreadCount;
        u32 TotalFibers;
        u32 FreeFibers;
        u32 PeakFibers;
        u32 HighQueueSize;
        WorkerState PerThreadState[MAX_WORKER_THREADS];

        // Per-frame aggregated counters (reset each frame)
        u32 JobsExecuted;
        u32 StealAttempts;
        u32 StealSuccesses;
        u32 FiberYields;
        u32 ReadyFiberCount;
    };

    // ===================================================================================
    // Job Context — Fiber Local Storage (FLS)
    // ===================================================================================
    // Carried per-fiber, NOT per-thread.
    // Accessed via GetCurrentJobContext(). Stored in Win32 FLS.

    struct JobContext
    {
        // Memory
        Memory::TaggedPageAllocator* Allocator = nullptr;

        // Frame Data
        const FrameParams* Params = nullptr; // Read-only params for the current frame

        // Command Recording
        CommandAllocatorPool* CommandPool = nullptr;
        void* CurrentCommandAllocator = nullptr;

        // V3: Recording Guard — Prevents yield while recording VkCommandBuffer
        bool IsRecording = false;

        // V5: Inline Execution Depth Tracking
        u32 InlineDepth = 0;

        // Metadata
        u32 ThreadIndex = 0;
        u32 FiberID = 0;
    };

    // ===================================================================================
    // V3: RAII Recording Scope
    // ===================================================================================
    // Sets IsRecording = true for the duration of the scope.
    // Fiber::Yield() will assert if IsRecording is true, catching violations
    // of Contract 4 (VkCommandBuffer thread affinity) at the point of violation.

    struct RecordingScope
    {
        explicit RecordingScope(JobContext* ctx) : m_Ctx(ctx)
        {
            assert(ctx && "RecordingScope: null JobContext");
            assert(!ctx->IsRecording && "RecordingScope: nested recording not allowed");
            ctx->IsRecording = true;
        }

        ~RecordingScope()
        {
            m_Ctx->IsRecording = false;
        }

        RecordingScope(const RecordingScope&) = delete;
        RecordingScope& operator=(const RecordingScope&) = delete;

    private:
        JobContext* m_Ctx;
    };

    // ===================================================================================
    // API
    // ===================================================================================

    // Lifecycle
    void Init(u32 numThreads = 0);
    void Shutdown();
    void ResetFrameStats();

    // Run a single task (defaults to High priority for backward compat)
    void Execute(JobFunction function, void* data = nullptr,
                 Counter* counter = nullptr, Priority priority = Priority::High);

    // Run multiple tasks (data parallelism)
    void Dispatch(u32 jobCount, u32 groupSize, JobFunction function,
                  void* data = nullptr, Counter* counter = nullptr,
                  Priority priority = Priority::Normal);

    // Fiber-aware wait.
    // V5: Uses depth-limited inline execution (up to depth 4) before switching fibers.
    // If called from main thread: busy-spins (isolated, no job stealing per V2).
    void WaitForCounter(Counter* counter, u32 targetValue = 0);

    // Yield the current fiber to the scheduler.
    // V3: Asserts that IsRecording == false. Hard crash in debug on violation.
    void YieldFiber();

    // Returns true if the counter has not reached the target value
    bool IsBusy(const Counter* counter);

    Stats GetStats();

    // Returns the index of the current worker thread (0 to N-1)
    u32 GetWorkerThreadId();

    // Access the current Fiber's context (via FLS)
    JobContext* GetCurrentJobContext();

    // Set the global command pool for the current frame (used by Renderer at frame boundary)
    void SetGlobalCommandPool(CommandAllocatorPool* pool);
}
