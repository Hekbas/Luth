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
        JobContext publicContext; // The public FLS
        bool isMainThread = false;
        const char* debugName = nullptr;
        FiberContext* fiberToRecycle = nullptr;
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
    static u32 s_ActiveFibers = 0;
    static u32 s_PeakFibers = 0;

    // Thread Local State - ONLY for the Scheduler Fiber
    // We use this to know which Fiber is currently running on this OS thread.
    // This is the ONLY valid use of thread_local in the system.
    static thread_local FiberContext* s_CurrentFiber = nullptr;
    static thread_local FiberContext* s_SchedulerFiber = nullptr; 

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
        for (u32 i = 0; i < 128; ++i)
        {
            FiberContext* ctx = new FiberContext();
            ctx->fiber = Fiber::Create(FiberEntryPoint, ctx, 512 * 1024); // 512KB stack
            ctx->debugName = "Worker Fiber";
            s_FreeFibers.push_back(ctx);
            s_AllFibers.push_back(ctx);
        }

        // Convert Main Thread to Fiber (This becomes the Scheduler for Thread 0)
        s_SchedulerFiber = new FiberContext();
        s_SchedulerFiber->fiber = Fiber::ConvertThreadToFiber(nullptr);
        s_SchedulerFiber->isMainThread = true;
        s_SchedulerFiber->debugName = "Main Thread";
        s_SchedulerFiber->publicContext.ThreadIndex = 0;
        s_CurrentFiber = s_SchedulerFiber;

        // Spawn Workers
        for (u32 i = 0; i < numThreads; ++i)
        {
            s_WorkerThreads.emplace_back(std::bind(WorkerThreadEntryPoint, i + 1));
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
        
        delete s_SchedulerFiber;
    }

    void ResetFrameStats()
    {
        std::lock_guard<std::mutex> lock(s_FiberPoolLock);
        s_PeakFibers = s_ActiveFibers;
    }

    JobContext* GetCurrentJobContext()
    {
        // This is safe because s_CurrentFiber is updated on every switch
        return &s_CurrentFiber->publicContext;
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

    Stats GetStats()
    {
        Stats stats;
        stats.ThreadCount = (u32)s_WorkerThreads.size();
        
        std::lock_guard<std::mutex> fiberLock(s_FiberPoolLock);
        stats.TotalFibers = (u32)s_AllFibers.size();
        stats.FreeFibers = (u32)s_FreeFibers.size();
        stats.PeakFibers = s_PeakFibers;
        
        std::lock_guard<std::mutex> queueLock(s_QueueLock);
        stats.QueueSize = (u32)s_JobQueue.size();
        
        return stats;
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
                    // Only if we are NOT the main thread.
                    if (s_CurrentFiber != s_SchedulerFiber) 
                    {
                        // Should not happen, we are in scheduler loop
                    }
                    
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
                        
                        s_ActiveFibers++;
                        if (s_ActiveFibers > s_PeakFibers) s_PeakFibers = s_ActiveFibers;
                    }
                }

                if (fiberCtx)
                {
                    // 3. Switch to Fiber
                    fiberCtx->currentJob = job;
                    
                    // Inherit thread index for debugging/profiling
                    fiberCtx->publicContext.ThreadIndex = s_SchedulerFiber->publicContext.ThreadIndex;

                    s_CurrentFiber = fiberCtx;
                    LH_PROFILE_FIBER_ENTER(fiberCtx->debugName);
                    Fiber::SwitchTo(fiberCtx->fiber);
                    
                    // 4. Back from Fiber (Job finished or suspended)
                    LH_PROFILE_FIBER_ENTER("Scheduler");
                    s_CurrentFiber = s_SchedulerFiber;

                    // Recycle the fiber if it finished
                    if (s_SchedulerFiber->fiberToRecycle)
                    {
                        std::lock_guard<std::mutex> lock(s_FiberPoolLock);
                        s_ActiveFibers--;
                        s_FreeFibers.push_back(s_SchedulerFiber->fiberToRecycle);
                        s_SchedulerFiber->fiberToRecycle = nullptr;
                    }
                }
                else
                {
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
                job.counter->value.fetch_sub(1);
            }

            // 3. Mark for recycling (Scheduler will handle it after switch)
            s_SchedulerFiber->fiberToRecycle = ctx;
            
            // Switch back to the thread that scheduled us
            LH_PROFILE_FIBER_ENTER("Scheduler");
            Fiber::SwitchTo(s_SchedulerFiber->fiber);
            LH_PROFILE_FIBER_ENTER(ctx->debugName);
        }
    }

    static void WorkerThreadEntryPoint(u32 threadIndex)
    {
        LH_PROFILE_THREAD(fmt::format("Worker Thread {}", threadIndex).c_str());

        // Convert this thread to a fiber so we can switch FROM it
        s_SchedulerFiber = new FiberContext();
        s_SchedulerFiber->fiber = Fiber::ConvertThreadToFiber(nullptr);
        s_SchedulerFiber->debugName = "Worker Thread";
        s_SchedulerFiber->publicContext.ThreadIndex = threadIndex;
        s_CurrentFiber = s_SchedulerFiber;

        SchedulerEntryPoint(nullptr);

        delete s_SchedulerFiber;
    }

    void WaitForCounter(Counter* counter, u32 targetValue)
    {
        if (!counter) return;

        // Simple Fiber-aware wait (Help with jobs)
        while (counter->value.load() > targetValue)
        {
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
                std::this_thread::yield();
            }
        }
    }
}
