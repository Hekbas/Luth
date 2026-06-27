#include "luthpch.h"
#include "luth/jobs/JobSystem.h"
#include "luth/jobs/Fiber.h"
#include "luth/jobs/SpinLock.h"
#include "luth/jobs/MPMCQueue.h"
#include "luth/jobs/WorkStealingDeque.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/core/diagnostics/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <emmintrin.h> // _mm_pause
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>   // Fibers, FLS, WaitOnAddress
#pragma comment(lib, "Synchronization.lib") // WaitOnAddress / WakeByAddress
#endif

// V<n> markers refer to JobSystem hazards — see docs/development/arch/version-glossary.md

namespace Luth::JobSystem
{
    // ── Constants ──

    static constexpr u32 MAX_FIBERS         = 512;
    static constexpr u32 HIGH_QUEUE_SIZE    = 4096;  // Must be power of 2
    static constexpr u32 LOCAL_DEQUE_SIZE   = 1024;
    static constexpr u32 MAX_INLINE_DEPTH   = 4;     // V5: depth-limited inline execution

    // Stable pointer-identity names for TRACY_FIBERS instrumentation.
    static char s_FiberNames[MAX_FIBERS][16];
    static char s_SchedulerNames[MAX_WORKER_THREADS][16];

    // ── Internal Job Struct ──

    struct Job
    {
        JobFunction Function = nullptr;
        void* Data = nullptr;
        Counter* CounterPtr = nullptr;
        u32 JobIndex = 0;
        u32 GroupIndex = 0;
        const char* Name = "Job"; // Tracy zone label
        Stage StageTag = Stage::Main;
    };

    // Per-fiber JobContext lookup via TIB ArbitraryUserPointer at gs:[0x28], swapped
    // per switch by FiberPrimitive.asm's jump_fcontext save/restore of NT_TIB fields.

    static void SetCurrentContext(JobContext* ctx)
    {
        __writegsqword(0x28, reinterpret_cast<uintptr_t>(ctx));
    }

    // Forward-declare; body needs s_Data which is defined below.
    static const char* GetMyTracyName();

    // ── Per-Worker Data (indexed by thread ID, not per-fiber) ──

    struct WorkerData
    {
        std::thread Thread;
        Fiber SchedulerFiber;   // The OS thread converted to fiber
        Fiber* CurrentFiber = nullptr;
        std::unique_ptr<WorkStealingDeque<Job>> LocalDeque;
        JobContext Context;     // One context per fiber (swapped on fiber switch)
        u32 ThreadIndex = 0;

        WorkerData() : LocalDeque(std::make_unique<WorkStealingDeque<Job>>(LOCAL_DEQUE_SIZE)) {}
    };

    // ── Global Scheduler Data ──

    struct SchedulerData
    {
        // Workers
        std::vector<WorkerData> Workers;
        u32 ThreadCount = 0;
        std::atomic<bool> Running = false;

        // Fiber Pool
        Fiber FiberPool[MAX_FIBERS];
        Job FiberJobs[MAX_FIBERS]; // Per-fiber job storage (avoids thread_local lifetime issue)
        JobContext FiberContexts[MAX_FIBERS]; // One context per fiber
        SpinLock FiberPoolLock;
        u32 FreeFibers[MAX_FIBERS];
        u32 FreeFiberCount = 0;

        // Global High-Priority Queue (Lock-Free MPMC)
        MPMCQueue<Job, HIGH_QUEUE_SIZE> HighQueue;

        // Ready Fibers (waiting fibers whose counter reached target)
        Fiber* ReadyFibers[MAX_FIBERS];
        u32 ReadyFiberCount = 0;
        SpinLock ReadyFibersLock;

        // Global Contexts (propagated at frame boundary)
        std::atomic<CommandAllocatorPool*> GlobalCommandPool = nullptr;

        // Stats
        std::atomic<u32> PeakFibers = 0;

        // Per-worker live state (separate from WorkerData to avoid breaking move semantics). Read by
        // SetWorkerState to bank time into the state being left.
        std::atomic<WorkerState> WorkerStates[MAX_WORKER_THREADS]{};

        // Per-frame stats counters (reset by ResetFrameStats). Read by ProfilerPanel
        // only — relaxed loads/stores throughout; no synchronization with workers.
        std::atomic<u32> FrameJobsExecuted = 0;
        std::atomic<u32> FrameStealAttempts = 0;
        std::atomic<u32> FrameStealSuccesses = 0;
        std::atomic<u32> FrameFiberYields = 0;

        // Sticky (not reset per frame) so ProfilerPanel still shows the
        // last value on iterations where a stage was gated off.
        std::atomic<f32> LastGameStageMs = 0.0f;
        std::atomic<f32> LastRenderStageMs = 0.0f;

        // Per-worker occupancy + counters. StateNanos bank the time spent in the state a worker is LEAVING
        // on each SetWorkerState transition — written only by the owning worker, so no contention; monotonic
        // (the profiler diffs successive snapshots for a window). Jobs/Steals reset per frame.
        std::atomic<u64> WorkerStateNanos[MAX_WORKER_THREADS][4]{};
        u64              WorkerLastTransitionNs[MAX_WORKER_THREADS]{};
        std::atomic<u32> WorkerJobs[MAX_WORKER_THREADS]{};
        std::atomic<u32> WorkerSteals[MAX_WORKER_THREADS]{};
    };

    static SchedulerData s_Data;

    static const char* GetMyTracyName()
    {
        JobContext* ctx = GetCurrentJobContext();
        if (!ctx) return "Unknown";
        // Pool-fiber contexts live in FiberContexts[]; worker schedulers in WorkerData::Context.
        JobContext* poolBase = &s_Data.FiberContexts[0];
        if (ctx >= poolBase && ctx < poolBase + MAX_FIBERS)
            return s_FiberNames[ctx - poolBase];
        return s_SchedulerNames[ctx->ThreadIndex];
    }

    // Monotonic nanosecond clock for per-worker occupancy accounting.
    static u64 NowNs()
    {
        return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Worker state observed by the profiler — relaxed throughout. Banks time spent in the state being LEFT
    // into this worker's occupancy bin (owner-only write; the first transition is skipped via last==0).
    static void SetWorkerState(u32 index, WorkerState state)
    {
        const u64 now  = NowNs();
        const u64 last = s_Data.WorkerLastTransitionNs[index];
        if (last != 0) {
            const WorkerState prev = s_Data.WorkerStates[index].load(std::memory_order_relaxed);
            s_Data.WorkerStateNanos[index][static_cast<u8>(prev)].fetch_add(now - last, std::memory_order_relaxed);
        }
        s_Data.WorkerLastTransitionNs[index] = now;

        s_Data.WorkerStates[index].store(state, std::memory_order_relaxed);
    }

    // ── Worker Thread ID ──
    // thread_local is safe here: per-OS-thread, not per-fiber. Worker index
    // doesn't change when fibers switch on the same OS thread.

    static thread_local u32 t_WorkerIndex = 0;
    static thread_local bool t_IsMainThread = false;

    // ── Fiber Pool Management ──

    static Fiber* AllocateFiber()
    {
        SpinLockGuard lock(s_Data.FiberPoolLock);

        if (s_Data.FreeFiberCount == 0)
        {
            LH_CORE_ERROR("Fiber Pool Exhausted! ({0} fibers)", MAX_FIBERS);
            return nullptr;
        }

        u32 index = s_Data.FreeFibers[--s_Data.FreeFiberCount];

        u32 used = MAX_FIBERS - s_Data.FreeFiberCount;
        u32 peak = s_Data.PeakFibers.load(std::memory_order_relaxed);
        if (used > peak) s_Data.PeakFibers.store(used, std::memory_order_relaxed);

        return &s_Data.FiberPool[index];
    }

    static void FreeFiber(Fiber* fiber)
    {
        u32 index = (u32)(fiber - s_Data.FiberPool);

        SpinLockGuard lock(s_Data.FiberPoolLock);
        s_Data.FreeFibers[s_Data.FreeFiberCount++] = index;
    }

    static JobContext* GetFiberContext(Fiber* fiber)
    {
        u32 index = (u32)(fiber - s_Data.FiberPool);
        return &s_Data.FiberContexts[index];
    }

    // ── Counter Decrement Logic ──

    static void DecrementCounter(Counter* counter)
    {
        // Atomic decrement with Busy Bit (Bit 0 = Busy, Bits 1..31 = Count)
        u32 old = counter->Value.load();
        while (true)
        {
            // If Busy (odd), spin until clear
            if ((old & 1) == 1)
            {
                _mm_pause();
                old = counter->Value.load();
                continue;
            }

            if (old < 2) break; // Should not happen

            if (old == 2) // Last job (Count 1 -> 0)
            {
                // Set Busy Bit (Value 1)
                if (counter->Value.compare_exchange_weak(old, 1))
                    break;
            }
            else // old > 2
            {
                // Decrement count (Value - 2)
                if (counter->Value.compare_exchange_weak(old, old - 2))
                    break;
            }
        }

        if (old == 2)
        {
            // Counter reached zero — wake waiting fibers
            // Acquire spinlock on the wait list
            while (counter->Lock.test_and_set(std::memory_order_acquire))
                _mm_pause();

            Fiber* waitingFiber = counter->WaitingListHead;
            counter->WaitingListHead = nullptr;

            counter->Lock.clear(std::memory_order_release);

            // Move all waiting fibers to the Ready list
            while (waitingFiber)
            {
                Fiber* next = waitingFiber->NextWaiting;
                waitingFiber->NextWaiting = nullptr;

                {
                    SpinLockGuard lock(s_Data.ReadyFibersLock);
                    s_Data.ReadyFibers[s_Data.ReadyFiberCount++] = waitingFiber;
                }

                // V4: wake a sleeping worker. WakeByAddressSingle pairs with
                // the WaitOnAddress in WorkerThreadLoop's idle path.
                s_Data.HighQueue.GetGeneration();
                u32 gen = s_Data.HighQueue.GetGeneration();
#ifdef _WIN32
                WakeByAddressSingle(s_Data.HighQueue.GetGenerationPtr());
#endif

                waitingFiber = next;
            }

            // Clear Busy Bit (Value 1 -> 0)
            counter->Value.fetch_sub(1);
        }
    }

    // ── Public counter primitives (declared in AtomicCounter.h) ──

    void AtomicCounter::Increment(u32 n)
    {
        Value.fetch_add(n << 1);
    }

    void AtomicCounter::Decrement(u32 n)
    {
        for (u32 i = 0; i < n; ++i)
            DecrementCounter(this);
    }

    // ── Fiber Entry Point ──

    static void WINAPI FiberEntryPoint(void* args)
    {
        Job* jobPtr = (Job*)args;

        // Set up FLS for this fiber
        Fiber* self = s_Data.Workers[t_WorkerIndex].CurrentFiber;
        JobContext* ctx = GetFiberContext(self);

        // Propagate global state. GlobalCommandPool is set once per frame by the
        // main thread before any worker dispatch — relaxed is safe; the frame
        // fence orders the publish.
        ctx->CommandPool = s_Data.GlobalCommandPool.load(std::memory_order_relaxed);
        ctx->ThreadIndex = t_WorkerIndex;
        ctx->FiberID = (u32)(self - s_Data.FiberPool);
        ctx->IsRecording = false;
        ctx->InlineDepth = 0;
        ctx->CurrentStage = jobPtr ? jobPtr->StageTag : Stage::Main;
        ctx->Allocator = &Memory::TaggedPageAllocator::Get();
        ctx->CpuCache = {};
        ctx->GpuCache = {};
        SetCurrentContext(ctx);

        // Pin yielded resumes to this worker — see WorkerThreadLoop ready-pickup.
        self->PinnedThreadIndex = t_WorkerIndex;

        LH_PROFILE_FIBER_ENTER(s_FiberNames[ctx->FiberID]);

        if (jobPtr && jobPtr->Function)
        {
            LH_PROFILE_SCOPE_DYNAMIC_CSTR(jobPtr->Name);

            JobArgs jArgs{ jobPtr->JobIndex, jobPtr->GroupIndex, jobPtr->Data };
            jobPtr->Function(jArgs);

            if (jobPtr->CounterPtr)
                DecrementCounter(jobPtr->CounterPtr);

            s_Data.FrameJobsExecuted.fetch_add(1, std::memory_order_relaxed);
            s_Data.WorkerJobs[t_WorkerIndex].fetch_add(1, std::memory_order_relaxed);
        }

        self->IsFinished = true;
        LH_PROFILE_FIBER_LEAVE;
        Fiber::SwitchTo(*self, s_Data.Workers[t_WorkerIndex].SchedulerFiber);
    }

    // ── Worker Thread Loop ──

    static void WorkerThreadLoop(u32 workerIndex)
    {
        LH_PROFILE_THREAD("Worker");

        t_WorkerIndex = workerIndex;
        t_IsMainThread = false;

        WorkerData& worker = s_Data.Workers[workerIndex];
        worker.ThreadIndex = workerIndex;

        // Wrap OS thread as a Fiber (so we can switch away). Seeds gs:[0x28] / FLS
        // with worker.Context on the custom / Win32 backends respectively.
        worker.SchedulerFiber = Fiber::CaptureCurrentThreadAsFiber(&worker.Context);
        worker.CurrentFiber = &worker.SchedulerFiber;

        // Set up initial context for the scheduler fiber
        worker.Context.ThreadIndex = workerIndex;
        SetCurrentContext(&worker.Context);

        LH_PROFILE_FIBER_ENTER(s_SchedulerNames[workerIndex]);

        while (s_Data.Running.load(std::memory_order_relaxed))
        {
            // Priority 1: resume ready fibers pinned to this worker.
            // Yielded fibers may carry per-thread state (Tracy zones, etc.)
            // that must end on the OS thread it began on. Sub-jobs never yield,
            // so work-stealing still distributes them freely.
            Fiber* readyFiber = nullptr;
            {
                SpinLockGuard lock(s_Data.ReadyFibersLock);
                for (u32 i = s_Data.ReadyFiberCount; i-- > 0; )
                {
                    Fiber* candidate = s_Data.ReadyFibers[i];
                    if (!candidate->IsPinned() || candidate->PinnedThreadIndex == workerIndex)
                    {
                        readyFiber = candidate;
                        s_Data.ReadyFibers[i] = s_Data.ReadyFibers[--s_Data.ReadyFiberCount];
                        break;
                    }
                }
            }

            if (readyFiber)
            {
                SetWorkerState(workerIndex, WorkerState::Running);
                JobContext* fiberCtx = GetFiberContext(readyFiber);
                fiberCtx->ThreadIndex = workerIndex;
                SetCurrentContext(fiberCtx);

                worker.CurrentFiber = readyFiber;

                LH_PROFILE_FIBER_LEAVE;
                Fiber::SwitchTo(worker.SchedulerFiber, *readyFiber);
                LH_PROFILE_FIBER_ENTER(s_SchedulerNames[workerIndex]);

                worker.CurrentFiber = &worker.SchedulerFiber;
                SetCurrentContext(&worker.Context);

                if (readyFiber->IsFinished)
                    FreeFiber(readyFiber);

                continue;
            }

            // Priority 2: global high queue (MPMC)
            Job job;
            bool foundJob = false;

            if (s_Data.HighQueue.TryPop(job))
            {
                foundJob = true;
            }
            // Priority 3: local deque (LIFO — cache locality)
            else if (worker.LocalDeque->TryPop(job))
            {
                foundJob = true;
            }
            // Priority 4: steal from other workers (FIFO — load balance)
            else
            {
                s_Data.FrameStealAttempts.fetch_add(1, std::memory_order_relaxed);
                u32 victimStart = workerIndex + 1;
                for (u32 i = 0; i < s_Data.ThreadCount; ++i)
                {
                    u32 victim = (victimStart + i) % s_Data.ThreadCount;
                    if (victim == workerIndex) continue;

                    if (s_Data.Workers[victim].LocalDeque->TrySteal(job))
                    {
                        SetWorkerState(workerIndex, WorkerState::Stealing);
                        s_Data.FrameStealSuccesses.fetch_add(1, std::memory_order_relaxed);
                        s_Data.WorkerSteals[workerIndex].fetch_add(1, std::memory_order_relaxed);
                        foundJob = true;
                        break;
                    }
                }
            }

            if (foundJob)
            {
                SetWorkerState(workerIndex, WorkerState::Running);
                // Get a fiber from the pool
                Fiber* fiber = AllocateFiber();
                if (!fiber)
                {
                    // Pool exhausted — push job back and spin
                    s_Data.HighQueue.TryPush(job);
                    _mm_pause();
                    continue;
                }

                // Destroy old fiber + reset stack. Destroy is idempotent on both backends.
                Fiber::Destroy(*fiber);

                // Store job in per-fiber storage (NOT thread_local — survives fiber yield)
                u32 fiberIndex = (u32)(fiber - s_Data.FiberPool);
                s_Data.FiberJobs[fiberIndex] = job;

                // Pre-compute fiber's owning JobContext so Create can patch gs:[0x28]
                // save slot — first resume of this fiber restores it into TIB.
                JobContext* fiberCtx = GetFiberContext(fiber);
                *fiber = Fiber::Create(FiberEntryPoint, &s_Data.FiberJobs[fiberIndex],
                                        fiberCtx);

                // Reset fiber context
                *fiberCtx = {};
                fiberCtx->ThreadIndex = workerIndex;
                SetCurrentContext(fiberCtx);

                worker.CurrentFiber = fiber;

                LH_PROFILE_FIBER_LEAVE;
                Fiber::SwitchTo(worker.SchedulerFiber, *fiber);
                LH_PROFILE_FIBER_ENTER(s_SchedulerNames[workerIndex]);

                worker.CurrentFiber = &worker.SchedulerFiber;
                SetCurrentContext(&worker.Context);

                if (fiber->IsFinished)
                    FreeFiber(fiber);

                continue;
            }

            // Priority 5: idle — wait for work (V4)
#ifdef _WIN32
            {
                // V4: Compare-and-wait pattern to prevent lost wakeups
                u32 gen = s_Data.HighQueue.GetGeneration();

                // Double-check: is there work now?
                if (!s_Data.HighQueue.IsEmpty())
                    continue;

                {
                    SpinLockGuard lock(s_Data.ReadyFibersLock);
                    if (s_Data.ReadyFiberCount > 0)
                        continue;
                }

                // Sleep until generation changes (new work arrives)
                SetWorkerState(workerIndex, WorkerState::Sleeping);
                WaitOnAddress(
                    s_Data.HighQueue.GetGenerationPtr(),
                    &gen,
                    sizeof(gen),
                    1 // 1ms timeout to check for shutdown
                );
            }
#else
            _mm_pause();
#endif
        }

        LH_PROFILE_FIBER_LEAVE;
    }

    // ── API Implementation ──

    void Init(u32 numThreads)
    {
        if (numThreads == 0) numThreads = std::thread::hardware_concurrency() - 1;
        if (numThreads < 1) numThreads = 1;

        s_Data.ThreadCount = numThreads + 1; // +1 for main thread (index 0)
        s_Data.Running = true;

        // Tracy fiber names — must be ready before any FiberEnter.
        for (u32 i = 0; i < MAX_FIBERS; ++i)
            std::snprintf(s_FiberNames[i], sizeof(s_FiberNames[i]), "Fiber_%u", i);
        for (u32 i = 0; i < MAX_WORKER_THREADS; ++i)
            std::snprintf(s_SchedulerNames[i], sizeof(s_SchedulerNames[i]), "Sched_%u", i);

        // Initialize Fiber Pool
        s_Data.FreeFiberCount = MAX_FIBERS;
        for (u32 i = 0; i < MAX_FIBERS; ++i)
            s_Data.FreeFibers[i] = i;

        // Initialize Workers (index 0 is reserved for main thread)
        s_Data.Workers.resize(s_Data.ThreadCount);

        // Pre-wire the global TaggedPageAllocator pointer onto every scheduler context
        // and every fiber-pool context. The pointer is stable for engine lifetime
        // (function-local singleton); FiberEntryPoint resets CpuCache per-job.
        Memory::TaggedPageAllocator& tpa = Memory::TaggedPageAllocator::Get();
        for (u32 i = 0; i < s_Data.ThreadCount; ++i)
            s_Data.Workers[i].Context.Allocator = &tpa;
        for (u32 i = 0; i < MAX_FIBERS; ++i)
            s_Data.FiberContexts[i].Allocator = &tpa;

        // Main thread setup (index 0)
        t_WorkerIndex = 0;
        t_IsMainThread = true;
        s_Data.Workers[0].ThreadIndex = 0;
        s_Data.Workers[0].SchedulerFiber =
            Fiber::CaptureCurrentThreadAsFiber(&s_Data.Workers[0].Context);
        s_Data.Workers[0].CurrentFiber = &s_Data.Workers[0].SchedulerFiber;
        s_Data.Workers[0].Context.ThreadIndex = 0;
        SetCurrentContext(&s_Data.Workers[0].Context);

        LH_PROFILE_FIBER_ENTER(s_SchedulerNames[0]);

        // Spawn Worker Threads (index 1..N)
        for (u32 i = 1; i < s_Data.ThreadCount; ++i)
        {
            s_Data.Workers[i].Thread = std::thread(WorkerThreadLoop, i);
        }

        LH_CORE_INFO("JobSystem initialized: {0} workers + main thread (isolated)", numThreads);
    }

    void Shutdown()
    {
        s_Data.Running = false;

        LH_PROFILE_FIBER_LEAVE;

        // Wake all sleeping workers
#ifdef _WIN32
        WakeByAddressAll(s_Data.HighQueue.GetGenerationPtr());
#endif

        for (u32 i = 1; i < s_Data.ThreadCount; ++i)
        {
            if (s_Data.Workers[i].Thread.joinable())
                s_Data.Workers[i].Thread.join();
        }

        // Destroy fiber pool
        for (u32 i = 0; i < MAX_FIBERS; ++i)
            Fiber::Destroy(s_Data.FiberPool[i]);

        s_Data.Workers.clear();
        LH_CORE_INFO("JobSystem shut down.");
    }

    void ResetFrameStats()
    {
        s_Data.PeakFibers = 0;
        s_Data.FrameJobsExecuted = 0;
        s_Data.FrameStealAttempts = 0;
        s_Data.FrameStealSuccesses = 0;
        s_Data.FrameFiberYields = 0;

        // Per-worker per-frame counters (occupancy nanos are monotonic — not reset here).
        for (u32 i = 0; i < s_Data.ThreadCount && i < MAX_WORKER_THREADS; ++i) {
            s_Data.WorkerJobs[i].store(0, std::memory_order_relaxed);
            s_Data.WorkerSteals[i].store(0, std::memory_order_relaxed);
        }
    }

    void Execute(JobFunction function, void* data, Counter* counter, const char* name, Priority priority)
    {
        if (counter) counter->Value.fetch_add(2); // Add 1 count (shifted by 1 for busy bit)

        // Capture parent's stage for child inheritance. Main thread has no
        // JobContext, so its dispatches default to Stage::Main.
        JobContext* parentCtx = GetCurrentJobContext();
        Stage parentStage = parentCtx ? parentCtx->CurrentStage : Stage::Main;

        Job job;
        job.Function = function;
        job.Data = data;
        job.CounterPtr = counter;
        job.JobIndex = 0;
        job.GroupIndex = 0;
        job.Name = name;
        job.StageTag = parentStage;

        if (priority == Priority::Normal && !t_IsMainThread)
        {
            // Push to local deque (if we're on a worker thread)
            s_Data.Workers[t_WorkerIndex].LocalDeque->Push(job);
        }
        else
        {
            // High/Low priority — global queue. Spin-retry if full.
            while (!s_Data.HighQueue.TryPush(job))
                _mm_pause(); // Workers are consuming, space will free up
        }
    }

    void Dispatch(u32 jobCount, u32 groupSize, JobFunction function,
                  void* data, Counter* counter, const char* name, Priority priority)
    {
        if (jobCount == 0) return;

        u32 groupCount = (jobCount + groupSize - 1) / groupSize;
        if (counter) counter->Value.fetch_add(groupCount << 1);

        JobContext* parentCtx = GetCurrentJobContext();
        Stage parentStage = parentCtx ? parentCtx->CurrentStage : Stage::Main;

        for (u32 i = 0; i < groupCount; ++i)
        {
            Job job;
            job.Function = function;
            job.Data = data;
            job.CounterPtr = counter;
            job.JobIndex = i * groupSize;
            job.GroupIndex = i;
            job.Name = name;
            job.StageTag = parentStage;

            if (priority == Priority::Normal && !t_IsMainThread)
            {
                s_Data.Workers[t_WorkerIndex].LocalDeque->Push(job);
            }
            else
            {
                while (!s_Data.HighQueue.TryPush(job))
                    _mm_pause();
            }
        }
    }

    void WaitForCounter(Counter* counter, u32 targetValue)
    {
        if (!counter) return;

        u32 target = targetValue << 1;

        if (t_IsMainThread)
        {
            // V2: Main thread is ISOLATED — no job stealing.
            // Busy-spin with _mm_pause until counter reaches target.
            while (true)
            {
                u32 v = counter->Value.load(std::memory_order_acquire);
                if (v <= target && (v & 1) == 0) return;
                _mm_pause();
            }
        }

        // Worker fiber path — yield to scheduler
        while (true)
        {
            u32 v = counter->Value.load(std::memory_order_acquire);
            if (v <= target && (v & 1) == 0) return;

            // If Busy bit set, spin briefly
            if ((v & 1) == 1)
            {
                _mm_pause();
                continue;
            }

            // V5: Try to help (depth-limited inline execution)
            JobContext* ctx = GetCurrentJobContext();
            if (ctx && ctx->InlineDepth < MAX_INLINE_DEPTH)
            {
                // Try to grab a local job
                Job inlineJob;
                bool found = false;

                if (s_Data.Workers[t_WorkerIndex].LocalDeque->TryPop(inlineJob))
                    found = true;
                else if (s_Data.HighQueue.TryPop(inlineJob))
                    found = true;

                if (found)
                {
                    ctx->InlineDepth++;
                    // Run inline job under its own stage tag so its
                    // assertions fire correctly and SetCurrentStage calls
                    // inside don't leak into the calling fiber.
                    Stage savedStage = ctx->CurrentStage;
                    ctx->CurrentStage = inlineJob.StageTag;
                    {
                        JobArgs jArgs{ inlineJob.JobIndex, inlineJob.GroupIndex, inlineJob.Data };
                        inlineJob.Function(jArgs);

                        if (inlineJob.CounterPtr)
                            DecrementCounter(inlineJob.CounterPtr);
                    }
                    ctx->CurrentStage = savedStage;
                    ctx->InlineDepth--;
                    continue; // Re-check our counter
                }
            }

            // No inline work available or depth exceeded — yield to scheduler
            // Add ourselves to the counter's wait list
            while (counter->Lock.test_and_set(std::memory_order_acquire))
                _mm_pause();

            // Double-check inside lock
            v = counter->Value.load(std::memory_order_acquire);
            if (v <= target && (v & 1) == 0)
            {
                counter->Lock.clear(std::memory_order_release);
                return;
            }

            if ((v & 1) == 1)
            {
                counter->Lock.clear(std::memory_order_release);
                _mm_pause();
                continue;
            }

            // Add current fiber to wait list
            Fiber* currentFiber = s_Data.Workers[t_WorkerIndex].CurrentFiber;
            currentFiber->NextWaiting = counter->WaitingListHead;
            counter->WaitingListHead = currentFiber;

            counter->Lock.clear(std::memory_order_release);

            // Set wait state
            currentFiber->WaitCounter = counter;
            currentFiber->WaitTarget = targetValue;

            LH_PROFILE_FIBER_LEAVE;
            Fiber::SwitchTo(*currentFiber, s_Data.Workers[t_WorkerIndex].SchedulerFiber);
            LH_PROFILE_FIBER_ENTER(GetMyTracyName());

            // invariant: DecrementCounter wakes us BEFORE its final fetch_sub(1) clears the busy bit; returning would UAF
            // caller's stack-local counter. Loop back; the busy-bit spin path blocks until fetch_sub(1) completes.
        }
    }

    void YieldFiber()
    {
        if (t_IsMainThread) return; // Main thread cannot yield (V2)

        // V3: Assert not recording
        JobContext* ctx = GetCurrentJobContext();
        assert(!ctx || !ctx->IsRecording &&
            "YieldFiber called while IsRecording == true! Contract 4 violation.");

        Fiber* currentFiber = s_Data.Workers[t_WorkerIndex].CurrentFiber;
        if (currentFiber == &s_Data.Workers[t_WorkerIndex].SchedulerFiber)
            return; // Already in scheduler

        s_Data.FrameFiberYields.fetch_add(1, std::memory_order_relaxed);

        // Push to ready list (so it gets picked up again)
        {
            SpinLockGuard lock(s_Data.ReadyFibersLock);
            s_Data.ReadyFibers[s_Data.ReadyFiberCount++] = currentFiber;
        }

        LH_PROFILE_FIBER_LEAVE;
        Fiber::SwitchTo(*currentFiber, s_Data.Workers[t_WorkerIndex].SchedulerFiber);
        LH_PROFILE_FIBER_ENTER(GetMyTracyName());
    }

    bool IsBusy(const Counter* counter)
    {
        if (!counter) return false;
        u32 v = counter->Value.load(std::memory_order_acquire);
        return v > 0;
    }

    Stats GetStats()
    {
        Stats stats{};
        stats.ThreadCount = s_Data.ThreadCount;
        stats.TotalFibers = MAX_FIBERS;
        stats.FreeFibers  = s_Data.FreeFiberCount;
        stats.PeakFibers  = s_Data.PeakFibers.load(std::memory_order_relaxed);
        stats.HighQueueSize = s_Data.HighQueue.GetSize();

        // Per-worker occupancy nanos + counters + live deque depth (index 0 = main thread).
        for (u32 i = 0; i < s_Data.ThreadCount && i < MAX_WORKER_THREADS; ++i) {
            for (u32 s = 0; s < 4; ++s)
                stats.PerThreadStateNanos[i][s] = s_Data.WorkerStateNanos[i][s].load(std::memory_order_relaxed);
            stats.PerThreadJobs[i]   = s_Data.WorkerJobs[i].load(std::memory_order_relaxed);
            stats.PerThreadSteals[i] = s_Data.WorkerSteals[i].load(std::memory_order_relaxed);
            stats.PerThreadQueued[i] = (i < s_Data.Workers.size())
                ? (u32)std::max<i64>(0, s_Data.Workers[i].LocalDeque->Size()) : 0u;
        }

        stats.JobsExecuted   = s_Data.FrameJobsExecuted.load(std::memory_order_relaxed);
        stats.StealAttempts   = s_Data.FrameStealAttempts.load(std::memory_order_relaxed);
        stats.StealSuccesses  = s_Data.FrameStealSuccesses.load(std::memory_order_relaxed);
        stats.FiberYields     = s_Data.FrameFiberYields.load(std::memory_order_relaxed);
        stats.ReadyFiberCount = s_Data.ReadyFiberCount;

        stats.GameStageMs   = s_Data.LastGameStageMs.load(std::memory_order_relaxed);
        stats.RenderStageMs = s_Data.LastRenderStageMs.load(std::memory_order_relaxed);

        return stats;
    }

    u32 GetWorkerThreadId()
    {
        return t_WorkerIndex;
    }

    JobContext* GetCurrentJobContext()
    {
        return reinterpret_cast<JobContext*>(__readgsqword(0x28));
    }

    void SetGlobalCommandPool(CommandAllocatorPool* pool)
    {
        s_Data.GlobalCommandPool.store(pool, std::memory_order_relaxed);
    }

    // ── Stage tag accessors ──

    Stage GetCurrentStage()
    {
        JobContext* ctx = GetCurrentJobContext();
        return ctx ? ctx->CurrentStage : Stage::Main;
    }

    void SetCurrentStage(Stage stage)
    {
        JobContext* ctx = GetCurrentJobContext();
        if (ctx) ctx->CurrentStage = stage;
    }

    void RecordStageTime(Stage stage, f32 ms)
    {
        switch (stage)
        {
            case Stage::Game:   s_Data.LastGameStageMs.store(ms, std::memory_order_relaxed);   break;
            case Stage::Render: s_Data.LastRenderStageMs.store(ms, std::memory_order_relaxed); break;
            default: break;
        }
    }
}
