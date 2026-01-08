#include "luthpch.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Fiber.h"
#include "luth/core/Profiler.h"
#include "luth/core/Log.h"

#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <array>

namespace Luth::JobSystem
{
    // ===================================================================================
    // Internal Structures
    // ===================================================================================

    struct Job
    {
        JobFunction function;
        void* data;
        Counter* counter;
        u32 start;
        u32 end;
    };

    struct FiberContext
    {
        Fiber fiber;
        Job currentJob;
        bool isMainThread = false;
    };

    // We use a fixed pool of counters to avoid allocation during runtime
    constexpr u32 MAX_COUNTERS = 1024;
    struct CounterData
    {
        std::atomic<u32> value = 0;
        std::vector<FiberContext*> waitingFibers; // Fibers waiting on this counter
        std::mutex lock;
    };

    // ===================================================================================
    // Global State
    // ===================================================================================

    static std::vector<std::thread> s_WorkerThreads;
    static std::atomic<bool> s_Running = false;

    // Job Queue
    static std::deque<Job> s_JobQueue;
    static std::mutex s_QueueLock;
    static std::condition_variable s_WakeCondition;

    // Fiber Pool
    static std::vector<FiberContext*> s_FreeFibers;
    static std::vector<FiberContext*> s_AllFibers; // To delete them at shutdown
    static std::mutex s_FiberPoolLock;

    // Thread Local State
    static thread_local FiberContext* s_CurrentFiber = nullptr;
    static thread_local FiberContext* s_ThreadFiber = nullptr; // The "OS Thread" fiber (scheduler)

    // ===================================================================================
    // Forward Declarations
    // ===================================================================================

    static void WorkerThreadEntryPoint(u32 threadIndex);
    static void FiberEntryPoint(void* args);
    static void SchedulerEntryPoint(void* args);

    // ===================================================================================
    // Implementation
    // ===================================================================================

    void Init(u32 numThreads)
    {
        if (s_Running) return;
        s_Running = true;

        if (numThreads == 0)
            numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);

        LH_CORE_INFO("Initializing Fiber JobSystem with {0} threads", numThreads);

        // Create Fiber Pool
        // We need enough fibers to cover all threads + waiting jobs.
        // 128 is a safe starting number for a "Swarm" demo.
        for (u32 i = 0; i < 128; ++i)
        {
            FiberContext* ctx = new FiberContext();
            ctx->fiber = Fiber::Create(FiberEntryPoint, ctx);
            s_FreeFibers.push_back(ctx);
            s_AllFibers.push_back(ctx);
        }

        // Convert Main Thread to Fiber
        s_ThreadFiber = new FiberContext();
        s_ThreadFiber->fiber = Fiber::ConvertThreadToFiber(nullptr);
        s_ThreadFiber->isMainThread = true;
        s_CurrentFiber = s_ThreadFiber;

        // Spawn Workers
        for (u32 i = 0; i < numThreads; ++i)
        {
            s_WorkerThreads.emplace_back(std::bind(WorkerThreadEntryPoint, i));
        }
    }

    void Shutdown()
    {
        if (!s_Running) return;
        s_Running = false;
        s_WakeCondition.notify_all();

        for (auto& thread : s_WorkerThreads)
            if (thread.joinable()) thread.join();

        s_WorkerThreads.clear();

        // Cleanup Fibers
        for (auto* ctx : s_AllFibers)
        {
            Fiber::Destroy(ctx->fiber);
            delete ctx;
        }
        s_AllFibers.clear();
        s_FreeFibers.clear();
        
        // Cleanup Main Thread Fiber wrapper
        // Note: We don't destroy the main thread fiber handle, OS does it? 
        // Actually ConvertThreadToFiber requires ConvertFiberToThread to undo? 
        // Or just let it die.
        delete s_ThreadFiber;
    }

    void Execute(JobFunction function, void* data, Counter* counter)
    {
        if (counter) counter->value++;

        {
            std::lock_guard<std::mutex> lock(s_QueueLock);
            s_JobQueue.push_back({ function, data, counter, 0, 1 });
        }
        s_WakeCondition.notify_one();
    }

    void Dispatch(u32 jobCount, u32 groupSize, JobFunction function, void* data, Counter* counter)
    {
        if (jobCount == 0 || groupSize == 0) return;

        u32 groupCount = (jobCount + groupSize - 1) / groupSize;

        if (counter) counter->value += groupCount;

        {
            std::lock_guard<std::mutex> lock(s_QueueLock);
            for (u32 i = 0; i < groupCount; ++i)
            {
                u32 start = i * groupSize;
                u32 end = std::min(start + groupSize, jobCount);
                s_JobQueue.push_back({ function, data, counter, start, end });
            }
        }
        s_WakeCondition.notify_all();
    }

    bool IsBusy(const Counter* counter)
    {
        return counter && counter->value.load() > 0;
    }

    // ===================================================================================
    // Fiber Logic
    // ===================================================================================

    // This runs on the "Thread Fiber" (Scheduler)
    static void SchedulerEntryPoint(void* args)
    {
        while (s_Running)
        {
            Job job;
            bool found = false;

            // 1. Try to get a job
            {
                std::unique_lock<std::mutex> lock(s_QueueLock);
                if (!s_JobQueue.empty())
                {
                    job = s_JobQueue.front();
                    s_JobQueue.pop_front();
                    found = true;
                }
                else
                {
                    // If no jobs, wait (sleep the OS thread)
                    // Only if we are NOT the main thread. Main thread shouldn't sleep here usually.
                    // But if Main calls WaitForCounter, it enters here.
                    if (s_CurrentFiber != s_ThreadFiber) 
                    {
                        // We are a worker fiber returning to scheduler? 
                        // No, Scheduler IS s_ThreadFiber.
                    }
                    
                    // If we are a worker thread, we wait.
                    // If we are main thread, we yield?
                    // For simplicity, we use condition variable.
                    s_WakeCondition.wait(lock);
                }
            }

            if (found)
            {
                // 2. Get a free fiber
                FiberContext* fiberCtx = nullptr;
                {
                    std::lock_guard<std::mutex> lock(s_FiberPoolLock);
                    if (!s_FreeFibers.empty())
                    {
                        fiberCtx = s_FreeFibers.back();
                        s_FreeFibers.pop_back();
                    }
                }

                if (fiberCtx)
                {
                    // 3. Switch to Fiber
                    fiberCtx->currentJob = job;
                    s_CurrentFiber = fiberCtx;
                    Fiber::SwitchTo(fiberCtx->fiber);
                    
                    // 4. Back from Fiber (Job finished or suspended)
                    s_CurrentFiber = s_ThreadFiber;
                }
                else
                {
                    // No fibers available! This is bad.
                    // We should probably run the job on the thread stack as fallback?
                    // Or spin wait.
                    LH_CORE_ERROR("Fiber pool exhausted!");
                }
            }
        }
    }

    // This runs inside a Fiber
    static void FiberEntryPoint(void* args)
    {
        FiberContext* ctx = (FiberContext*)args;

        while (true)
        {
            // 1. Execute Job
            Job& job = ctx->currentJob;
            
            JobArgs jobArgs{ 0, 0, job.data };
            for (u32 i = job.start; i < job.end; ++i)
            {
                jobArgs.jobIndex = i;
                job.function(jobArgs);
            }

            // 2. Decrement Counter
            if (job.counter)
            {
                u32 prev = job.counter->value.fetch_sub(1);
                if (prev == 1)
                {
                    // Counter reached 0. Wake up waiting fibers?
                    // We don't have a central wait list yet.
                    // In this simple implementation, waiting fibers are just polling or 
                    // we need to implement the "Waiting List" logic.
                }
            }

            // 3. Return to Scheduler (Recycle Fiber)
            {
                std::lock_guard<std::mutex> lock(s_FiberPoolLock);
                s_FreeFibers.push_back(ctx);
            }
            
            // Switch back to the thread that scheduled us
            Fiber::SwitchTo(s_ThreadFiber->fiber);
        }
    }

    static void WorkerThreadEntryPoint(u32 threadIndex)
    {
        // Convert this thread to a fiber so we can switch FROM it
        s_ThreadFiber = new FiberContext();
        s_ThreadFiber->fiber = Fiber::ConvertThreadToFiber(nullptr);
        s_CurrentFiber = s_ThreadFiber;

        SchedulerEntryPoint(nullptr);

        delete s_ThreadFiber;
    }

    void WaitForCounter(Counter* counter, u32 targetValue)
    {
        if (!counter) return;

        // Simple Fiber-aware wait:
        // While waiting, run other jobs.
        // If we are in a Fiber, we could suspend.
        // But since we don't have a "Waiting List" implementation yet, 
        // we will just help the scheduler.
        
        // Note: This implementation effectively turns WaitForCounter into "HelpWithJobs".
        // It doesn't strictly "Suspend" the fiber in the sense of putting it aside.
        // It keeps the current fiber active and runs nested jobs.
        // This is safe but can overflow the stack if recursion is too deep.
        // True Fiber Wait requires swapping out the current fiber.
        
        while (counter->value.load() > targetValue)
        {
            // Try to run a job from the queue
            Job job;
            bool found = false;
            {
                std::unique_lock<std::mutex> lock(s_QueueLock);
                if (!s_JobQueue.empty())
                {
                    job = s_JobQueue.front();
                    s_JobQueue.pop_front();
                    found = true;
                }
            }

            if (found)
            {
                // Execute directly on this stack (Nested)
                // This avoids needing a new fiber for the helper work
                JobArgs args{ 0, 0, job.data };
                for (u32 i = job.start; i < job.end; ++i)
                {
                    args.jobIndex = i;
                    job.function(args);
                }

                if (job.counter) job.counter->value--;
            }
            else
            {
                // No jobs? Yield.
                std::this_thread::yield();
            }
        }
    }
}
