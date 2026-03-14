#include "luthpch.h"
#include "luth/jobs/JobSystem.h"
#include "luth/jobs/Fiber.h"
#include "luth/jobs/SpinLock.h"
#include "luth/jobs/MPMCQueue.h"
#include "luth/jobs/WorkStealingDeque.h"
#include "luth/core/Log.h"
#include "luth/core/Profiler.h"

#include <thread>
#include <vector>
#include <atomic>
#include <emmintrin.h> // _mm_pause

#ifdef _WIN32
#include <windows.h>   // Fibers, FLS, WaitOnAddress
#pragma comment(lib, "Synchronization.lib") // WaitOnAddress / WakeByAddress
#endif

// ===================================================================================
// Internal Implementation
// ===================================================================================

namespace Luth::JobSystem
{
    // -------------------------------------------------------------------------------
    // Constants
    // -------------------------------------------------------------------------------

    static constexpr u32 MAX_FIBERS         = 512;
    static constexpr u32 HIGH_QUEUE_SIZE    = 4096;  // Must be power of 2
    static constexpr u32 LOCAL_DEQUE_SIZE   = 1024;
    static constexpr u32 MAX_INLINE_DEPTH   = 4;     // V5: depth-limited inline execution

    // -------------------------------------------------------------------------------
    // Internal Job Struct
    // -------------------------------------------------------------------------------

    struct Job
    {
        JobFunction Function = nullptr;
        void* Data = nullptr;
        Counter* CounterPtr = nullptr;
        u32 JobIndex = 0;
        u32 GroupIndex = 0;
    };

    // -------------------------------------------------------------------------------
    // Fiber Local Storage (FLS) — Replaces thread_local
    // -------------------------------------------------------------------------------

    static DWORD s_FlsIndex = FLS_OUT_OF_INDEXES;

    static void SetCurrentContext(JobContext* ctx)
    {
        FlsSetValue(s_FlsIndex, ctx);
    }

    // -------------------------------------------------------------------------------
    // Per-Worker Data (indexed by thread ID, NOT per-fiber)
    // -------------------------------------------------------------------------------

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

    // -------------------------------------------------------------------------------
    // Global Scheduler Data
    // -------------------------------------------------------------------------------

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
    };

    static SchedulerData s_Data;

    // -------------------------------------------------------------------------------
    // Worker Thread ID (safe to use thread_local for this ONE variable because
    // this is per-OS-thread, not per-fiber, and that's correct — worker index
    // doesn't change when fibers switch on the same OS thread)
    // -------------------------------------------------------------------------------

    static thread_local u32 t_WorkerIndex = 0;
    static thread_local bool t_IsMainThread = false;

    // -------------------------------------------------------------------------------
    // Fiber Pool Management
    // -------------------------------------------------------------------------------

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

    // -------------------------------------------------------------------------------
    // Counter Decrement Logic
    // -------------------------------------------------------------------------------

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

                // V4: Wake a sleeping worker
                s_Data.HighQueue.GetGeneration(); // Touch to trigger wake
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

    // -------------------------------------------------------------------------------
    // Fiber Entry Point
    // -------------------------------------------------------------------------------

    static void WINAPI FiberEntryPoint(void* args)
    {
        Job* jobPtr = (Job*)args;

        // Set up FLS for this fiber
        Fiber* self = s_Data.Workers[t_WorkerIndex].CurrentFiber;
        JobContext* ctx = GetFiberContext(self);

        // Propagate global state
        ctx->CommandPool = s_Data.GlobalCommandPool.load(std::memory_order_relaxed);
        ctx->ThreadIndex = t_WorkerIndex;
        ctx->FiberID = (u32)(self - s_Data.FiberPool);
        ctx->IsRecording = false;
        ctx->InlineDepth = 0;
        SetCurrentContext(ctx);

        // Execute the job
        if (jobPtr && jobPtr->Function)
        {
            LH_PROFILE_SCOPE("Job");

            JobArgs jArgs{ jobPtr->JobIndex, jobPtr->GroupIndex, jobPtr->Data };
            jobPtr->Function(jArgs);

            // Decrement counter
            if (jobPtr->CounterPtr)
                DecrementCounter(jobPtr->CounterPtr);
        }

        // Mark finished
        self->IsFinished = true;

        // Return to scheduler fiber
        Fiber::SwitchTo(s_Data.Workers[t_WorkerIndex].SchedulerFiber);
    }

    // -------------------------------------------------------------------------------
    // Worker Thread Loop
    // -------------------------------------------------------------------------------

    static void WorkerThreadLoop(u32 workerIndex)
    {
        LH_PROFILE_THREAD("Worker");

        t_WorkerIndex = workerIndex;
        t_IsMainThread = false;

        WorkerData& worker = s_Data.Workers[workerIndex];
        worker.ThreadIndex = workerIndex;

        // Convert OS thread to fiber (so we can switch away from it)
        worker.SchedulerFiber = Fiber::ConvertThreadToFiber(nullptr);
        worker.CurrentFiber = &worker.SchedulerFiber;

        // Set up initial context for the scheduler fiber
        worker.Context.ThreadIndex = workerIndex;
        SetCurrentContext(&worker.Context);

        while (s_Data.Running.load(std::memory_order_relaxed))
        {
            // ---- Priority 1: Resume Ready Fibers ----
            Fiber* readyFiber = nullptr;
            {
                SpinLockGuard lock(s_Data.ReadyFibersLock);
                if (s_Data.ReadyFiberCount > 0)
                {
                    readyFiber = s_Data.ReadyFibers[--s_Data.ReadyFiberCount];
                }
            }

            if (readyFiber)
            {
                // Swap FLS to the resumed fiber's context
                JobContext* fiberCtx = GetFiberContext(readyFiber);
                fiberCtx->ThreadIndex = workerIndex; // Update thread index (may have migrated)
                SetCurrentContext(fiberCtx);

                worker.CurrentFiber = readyFiber;
                Fiber::SwitchTo(*readyFiber);
                worker.CurrentFiber = &worker.SchedulerFiber;

                // Restore scheduler context
                SetCurrentContext(&worker.Context);

                // Cleanup if finished
                if (readyFiber->IsFinished)
                    FreeFiber(readyFiber);

                continue;
            }

            // ---- Priority 2: Global High Queue (MPMC) ----
            Job job;
            bool foundJob = false;

            if (s_Data.HighQueue.TryPop(job))
            {
                foundJob = true;
            }
            // ---- Priority 3: Local Deque (LIFO — cache locality) ----
            else if (worker.LocalDeque->TryPop(job))
            {
                foundJob = true;
            }
            // ---- Priority 4: Steal from other workers (FIFO — load balance) ----
            else
            {
                u32 victimStart = workerIndex + 1;
                for (u32 i = 0; i < s_Data.ThreadCount; ++i)
                {
                    u32 victim = (victimStart + i) % s_Data.ThreadCount;
                    if (victim == workerIndex) continue;

                    if (s_Data.Workers[victim].LocalDeque->TrySteal(job))
                    {
                        foundJob = true;
                        break;
                    }
                }
            }

            if (foundJob)
            {
                // Get a fiber from the pool
                Fiber* fiber = AllocateFiber();
                if (!fiber)
                {
                    // Pool exhausted — push job back and spin
                    s_Data.HighQueue.TryPush(job);
                    _mm_pause();
                    continue;
                }

                // Destroy old fiber handle to reset stack
                if (fiber->Handle) Fiber::Destroy(*fiber);

                // Store job in per-fiber storage (NOT thread_local — survives fiber yield)
                u32 fiberIndex = (u32)(fiber - s_Data.FiberPool);
                s_Data.FiberJobs[fiberIndex] = job;

                *fiber = Fiber::Create(FiberEntryPoint, &s_Data.FiberJobs[fiberIndex]);

                // Reset fiber context
                JobContext* fiberCtx = GetFiberContext(fiber);
                *fiberCtx = {};
                fiberCtx->ThreadIndex = workerIndex;
                SetCurrentContext(fiberCtx);

                worker.CurrentFiber = fiber;
                Fiber::SwitchTo(*fiber);
                worker.CurrentFiber = &worker.SchedulerFiber;

                // Restore scheduler context
                SetCurrentContext(&worker.Context);

                if (fiber->IsFinished)
                    FreeFiber(fiber);

                continue;
            }

            // ---- Priority 5: Idle — Wait for work (V4 compliant) ----
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
    }

    // -------------------------------------------------------------------------------
    // API Implementation
    // -------------------------------------------------------------------------------

    void Init(u32 numThreads)
    {
        // Allocate FLS index
        s_FlsIndex = FlsAlloc(nullptr);
        if (s_FlsIndex == FLS_OUT_OF_INDEXES)
        {
            LH_CORE_CRITICAL("Failed to allocate FLS index!");
            return;
        }

        if (numThreads == 0) numThreads = std::thread::hardware_concurrency() - 1;
        if (numThreads < 1) numThreads = 1;

        s_Data.ThreadCount = numThreads + 1; // +1 for main thread (index 0)
        s_Data.Running = true;

        // Initialize Fiber Pool
        s_Data.FreeFiberCount = MAX_FIBERS;
        for (u32 i = 0; i < MAX_FIBERS; ++i)
            s_Data.FreeFibers[i] = i;

        // Initialize Workers (index 0 is reserved for main thread)
        s_Data.Workers.resize(s_Data.ThreadCount);

        // Main thread setup (index 0)
        t_WorkerIndex = 0;
        t_IsMainThread = true;
        s_Data.Workers[0].ThreadIndex = 0;
        s_Data.Workers[0].SchedulerFiber = Fiber::ConvertThreadToFiber(nullptr);
        s_Data.Workers[0].CurrentFiber = &s_Data.Workers[0].SchedulerFiber;
        s_Data.Workers[0].Context.ThreadIndex = 0;
        SetCurrentContext(&s_Data.Workers[0].Context);

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

        // Free FLS
        if (s_FlsIndex != FLS_OUT_OF_INDEXES)
        {
            FlsFree(s_FlsIndex);
            s_FlsIndex = FLS_OUT_OF_INDEXES;
        }

        s_Data.Workers.clear();
        LH_CORE_INFO("JobSystem shut down.");
    }

    void ResetFrameStats()
    {
        s_Data.PeakFibers = 0;
    }

    void Execute(JobFunction function, void* data, Counter* counter, Priority priority)
    {
        if (counter) counter->Value.fetch_add(2); // Add 1 count (shifted by 1 for busy bit)

        Job job;
        job.Function = function;
        job.Data = data;
        job.CounterPtr = counter;
        job.JobIndex = 0;
        job.GroupIndex = 0;

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
                  void* data, Counter* counter, Priority priority)
    {
        if (jobCount == 0) return;

        u32 groupCount = (jobCount + groupSize - 1) / groupSize;
        if (counter) counter->Value.fetch_add(groupCount << 1);

        for (u32 i = 0; i < groupCount; ++i)
        {
            Job job;
            job.Function = function;
            job.Data = data;
            job.CounterPtr = counter;
            job.JobIndex = i * groupSize;
            job.GroupIndex = i;

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
                    {
                        JobArgs jArgs{ inlineJob.JobIndex, inlineJob.GroupIndex, inlineJob.Data };
                        inlineJob.Function(jArgs);

                        if (inlineJob.CounterPtr)
                            DecrementCounter(inlineJob.CounterPtr);
                    }
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

            // Switch back to scheduler
            Fiber::SwitchTo(s_Data.Workers[t_WorkerIndex].SchedulerFiber);

            // Resumed! Counter has reached target.
            return;
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

        // Push to ready list (so it gets picked up again)
        {
            SpinLockGuard lock(s_Data.ReadyFibersLock);
            s_Data.ReadyFibers[s_Data.ReadyFiberCount++] = currentFiber;
        }

        Fiber::SwitchTo(s_Data.Workers[t_WorkerIndex].SchedulerFiber);
    }

    bool IsBusy(const Counter* counter)
    {
        if (!counter) return false;
        u32 v = counter->Value.load(std::memory_order_acquire);
        return v > 0;
    }

    Stats GetStats()
    {
        return {
            s_Data.ThreadCount,
            MAX_FIBERS,
            s_Data.FreeFiberCount,
            s_Data.PeakFibers.load(std::memory_order_relaxed),
            0 // TODO: HighQueue size query
        };
    }

    u32 GetWorkerThreadId()
    {
        return t_WorkerIndex;
    }

    JobContext* GetCurrentJobContext()
    {
        if (s_FlsIndex == FLS_OUT_OF_INDEXES) return nullptr;
        return (JobContext*)FlsGetValue(s_FlsIndex);
    }

    void SetGlobalCommandPool(CommandAllocatorPool* pool)
    {
        s_Data.GlobalCommandPool.store(pool, std::memory_order_relaxed);
    }
}
