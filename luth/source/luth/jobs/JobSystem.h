#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/jobs/AtomicCounter.h"
#include "luth/memory/TaggedPageAllocator.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include <functional>
#include <cassert>

// V<n> markers in this file refer to JobSystem hazards — see docs/development/arch/version-glossary.md

namespace Luth
{
    struct FrameParams;
    class CommandAllocatorPool;
}

namespace Luth::JobSystem
{
    // ── Job Descriptor ──

    struct JobArgs
    {
        u32 jobIndex;
        u32 groupIndex;
        void* data;
    };

    using JobFunction = void(*)(JobArgs);
    using Counter = AtomicCounter;

    // ── Job Priority ──

    enum class Priority : u8
    {
        High,   // Critical path: render recording, physics, audio
        Normal, // General gameplay, entity logic (per-thread Chase-Lev deque)
        Low     // Asset loading, background tasks
    };

    // ── Worker State (per-thread, profiler viz) ──

    enum class WorkerState : u8
    {
        Idle,
        Running,
        Stealing,
        Sleeping
    };

    // ── Stage Tag ──
    // Frame-pipeline stage owning the current fiber. Sub-jobs inherit
    // from their dispatching parent. Stage-isolated subsystems assert
    // on it to catch cross-stage mutations that would race under
    // concurrent dispatch.

    enum class Stage : u8
    {
        Main,
        Game,
        Render
    };

    // ── Runtime Stats ──

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

        // Per-stage CPU body time of the last completed frame. Game and
        // render run concurrently in steady state so these can overlap.
        f32 GameStageMs;
        f32 RenderStageMs;
    };

    // ── Job Context — Fiber Local Storage (carried per-fiber, accessed via GetCurrentJobContext, stored in Win32 FLS) ──

    struct JobContext
    {
        // Memory — Allocator points at the global TaggedPageAllocator instance;
        // CpuCache is the per-fiber active page + tag (V6 hot path holds no lock).
        // GpuCache is the per-fiber tagged-heap cache (GPU-side Onion/Garlic split).
        Memory::TaggedPageAllocator* Allocator = nullptr;
        Memory::TaggedPageAllocator::ThreadCache CpuCache{};
        Memory::GPUThreadCache GpuCache{};

        // Frame Data
        const FrameParams* Params = nullptr; // Read-only params for the current frame

        // Command Recording
        CommandAllocatorPool* CommandPool = nullptr;
        void* CurrentCommandAllocator = nullptr;

        // V3: prevents yield while recording VkCommandBuffer
        bool IsRecording = false;

        // V5: inline execution depth tracking
        u32 InlineDepth = 0;

        Stage CurrentStage = Stage::Main;

        // Metadata
        u32 ThreadIndex = 0;
        u32 FiberID = 0;
    };

    // ── V3: RAII Recording Scope ──
    // Sets IsRecording = true for the scope duration. Fiber::Yield() asserts on
    // IsRecording, catching VkCommandBuffer thread-affinity violations at the call site.

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

    // ── API ──

    // Lifecycle
    void Init(u32 numThreads = 0);
    void Shutdown();
    void ResetFrameStats();

    // Run a single task (defaults to High priority for backward compat).
    // `name` is a Tracy zone label for the dispatched job's outer fiber zone — pass a
    // string literal at the call site (e.g. "AssetLoad"). Defaults to "Job" if omitted.
    void Execute(JobFunction function, void* data = nullptr,
                 Counter* counter = nullptr, const char* name = "Job",
                 Priority priority = Priority::High);

    // Run multiple tasks (data parallelism). See Execute for the `name` contract.
    void Dispatch(u32 jobCount, u32 groupSize, JobFunction function,
                  void* data = nullptr, Counter* counter = nullptr,
                  const char* name = "Job", Priority priority = Priority::Normal);

    // Fiber-aware wait. V5: depth-limited inline execution before fiber switch.
    // Main thread: busy-spins (V2 isolated, no stealing).
    void WaitForCounter(Counter* counter, u32 targetValue = 0);

    // Yield current fiber to scheduler. V3: asserts IsRecording == false.
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

    // Stage tag accessors. GetCurrentStage on the main thread (no
    // JobContext) returns Stage::Main.
    Stage GetCurrentStage();
    void  SetCurrentStage(Stage stage);

    // Records the outer stage body's wall time for ProfilerPanel.
    void RecordStageTime(Stage stage, f32 ms);
}
