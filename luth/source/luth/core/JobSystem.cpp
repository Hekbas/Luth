#include "luthpch.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Fiber.h"
#include "luth/core/Profiler.h"
#include "luth/core/Log.h"
#include "luth/core/AdaptiveMutex.h"

#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <array>
#include <chrono>

namespace Luth::JobSystem
{
    // ===================================================================================
    // Internal Structures
    // ===================================================================================

    enum class JobPriority : u8
    {
        High = 0,
        Normal = 1,
        Low = 2,
        Count = 3
    };

    struct Job
    {
        JobFunction function;
        void* data;
        Counter* counter;
        u32 start;
        u32 end;
        JobPriority priority;
        
        // Debug Info
        std::chrono::steady_clock::time_point startTime;
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
    static std::thread s_WatchdogThread;
    static std::atomic<bool> s_Running = false;

    // Job Queues (Priority Based)
    static std::deque<Job> s_JobQueues[(int)JobPriority::Count];
    static std::mutex s_StandardQueueLock;
    static std::condition_variable s_WakeCondition;

    // Fiber Pool
    static std::vector<FiberContext*> s_FreeFibers;
    static std::vector<FiberContext*> s_AllFibers; // To delete them at shutdown
    static std::mutex s_FiberPoolLock;
    static u32 s_ActiveFibers = 0;
    static u32 s_PeakFibers = 0;

    // Global Context Pointers (Set by App/Renderer)
    static Memory::TaggedPageAllocator* s_GlobalAllocator = nullptr;
    static const FrameParams* s_GlobalParams = nullptr;
    static CommandAllocatorPool* s_GlobalCommandPool = nullptr;
    static std::mutex s_ContextLock;

    // Thread Local State - ONLY for the Scheduler Fiber
    static thread_local FiberContext* s_CurrentFiber = nullptr;
    static thread_local FiberContext* s_SchedulerFiber = nullptr; 
    
    // Active Jobs Tracking (for Watchdog)
    struct ThreadStatus
    {
        std::atomic<bool> isActive;
        std::atomic<u64> jobStartTime; // Epoch time

        // Explicit constructor to satisfy std::vector requirements
        ThreadStatus() : isActive(false), jobStartTime(0) {}
        
        // Copy constructor (needed for vector resize, though we shouldn't copy atomics usually)
        // Since we only resize once at init, we can implement a dummy copy or move.
        ThreadStatus(const ThreadStatus& other) 
        {
            isActive.store(other.isActive.load());
            jobStartTime.store(other.jobStartTime.load());
        }
    };
    static std::vector<ThreadStatus> s_ThreadStatus;

    // ===================================================================================
    // Forward Declarations
    // ===================================================================================

    static void WorkerThreadEntryPoint(u32 threadIndex);
    static void FiberEntryPoint(void* args);
    static void SchedulerEntryPoint(void* args);
    static void WatchdogEntryPoint();

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

        // Initialize Thread Status
        // +1 for Main Thread
        // std::vector resize requires copy/move constructor for elements.
        // std::atomic is not copyable.
        // We implemented a custom copy constructor for ThreadStatus to handle this.
        s_ThreadStatus.resize(numThreads + 1);

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
        
        // Spawn Watchdog
        s_WatchdogThread = std::thread(WatchdogEntryPoint);
    }

    void Shutdown()
    {
        if (!s_Running) return;
        s_Running = false;
        s_WakeCondition.notify_all();

        for (auto& thread : s_WorkerThreads)
            if (thread.joinable()) thread.join();

        s_WorkerThreads.clear();
        
        if (s_WatchdogThread.joinable()) s_WatchdogThread.join();

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

    void SetGlobalFrameContext(Memory::TaggedPageAllocator* allocator, const FrameParams* params)
    {
        std::lock_guard<std::mutex> lock(s_ContextLock);
        s_GlobalAllocator = allocator;
        s_GlobalParams = params;
    }

    void SetGlobalCommandPool(CommandAllocatorPool* commandPool)
    {
        std::lock_guard<std::mutex> lock(s_ContextLock);
        s_GlobalCommandPool = commandPool;
    }

    JobContext* GetCurrentJobContext()
    {
        return &s_CurrentFiber->publicContext;
    }

    // Helper to push job to correct queue
    static void PushJob(const Job& job)
    {
        std::lock_guard<std::mutex> lock(s_StandardQueueLock);
        s_JobQueues[(int)job.priority].push_back(job);
    }

    void Execute(JobFunction function, void* data, Counter* counter)
    {
        if (counter) counter->value++;

        // Default priority Normal for now. 
        Job job{ function, data, counter, 0, 1, JobPriority::Normal };
        
        PushJob(job);
        s_WakeCondition.notify_one();
    }

    void Dispatch(u32 jobCount, u32 groupSize, JobFunction function, void* data, Counter* counter)
    {
        if (jobCount == 0 || groupSize == 0) return;

        u32 groupCount = (jobCount + groupSize - 1) / groupSize;

        if (counter) counter->value += groupCount;

        {
            std::lock_guard<std::mutex> lock(s_StandardQueueLock);
            for (u32 i = 0; i < groupCount; ++i)
            {
                u32 start = i * groupSize;
                u32 end = std::min(start + groupSize, jobCount);
                s_JobQueues[(int)JobPriority::Normal].push_back({ function, data, counter, start, end, JobPriority::Normal });
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
        
        std::lock_guard<std::mutex> queueLock(s_StandardQueueLock);
        stats.QueueSize = 0;
        for(int i=0; i<(int)JobPriority::Count; ++i)
            stats.QueueSize += (u32)s_JobQueues[i].size();
        
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

            // 1. Try to get a job (Priority Order)
            {
                std::unique_lock<std::mutex> lock(s_StandardQueueLock);
                
                // Check queues from High to Low
                for (int i = 0; i < (int)JobPriority::Count; ++i)
                {
                    if (!s_JobQueues[i].empty())
                    {
                        job = s_JobQueues[i].front();
                        s_JobQueues[i].pop_front();
                        found = true;
                        break;
                    }
                }

                if (!found)
                {
                    // If no jobs, wait (sleep the OS thread)
                    // Only if we are NOT the main thread.
                    if (s_CurrentFiber != s_SchedulerFiber) 
                    {
                        // Should not happen
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
                    
                    // Copy Global Context to Fiber
                    // This ensures every fiber has access to the current frame's allocators
                    {
                        std::lock_guard<std::mutex> lock(s_ContextLock);
                        fiberCtx->publicContext.Allocator = s_GlobalAllocator;
                        fiberCtx->publicContext.Params = s_GlobalParams;
                        fiberCtx->publicContext.CommandPool = s_GlobalCommandPool;
                    }

                    s_CurrentFiber = fiberCtx;
                    
                    // Update Watchdog Status
                    u32 threadIdx = s_SchedulerFiber->publicContext.ThreadIndex;
                    if (threadIdx < s_ThreadStatus.size())
                    {
                        s_ThreadStatus[threadIdx].jobStartTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        s_ThreadStatus[threadIdx].isActive = true;
                    }

                    LH_PROFILE_FIBER_ENTER(fiberCtx->debugName);
                    Fiber::SwitchTo(fiberCtx->fiber);
                    
                    // 4. Back from Fiber (Job finished or suspended)
                    LH_PROFILE_FIBER_ENTER("Scheduler");
                    s_CurrentFiber = s_SchedulerFiber;
                    
                    // Clear Watchdog Status
                    if (threadIdx < s_ThreadStatus.size())
                    {
                        s_ThreadStatus[threadIdx].isActive = false;
                    }

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
            LH_PROFILE_FIBER_LEAVE; // Leave fiber context before switching
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
                std::unique_lock<std::mutex> lock(s_StandardQueueLock);
                
                // Check queues from High to Low
                for (int i = 0; i < (int)JobPriority::Count; ++i)
                {
                    if (!s_JobQueues[i].empty())
                    {
                        job = s_JobQueues[i].front();
                        s_JobQueues[i].pop_front();
                        found = true;
                        break;
                    }
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

    static void WatchdogEntryPoint()
    {
        LH_PROFILE_THREAD("Job Watchdog");

        while (s_Running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            u64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            for (size_t i = 0; i < s_ThreadStatus.size(); ++i)
            {
                if (s_ThreadStatus[i].isActive)
                {
                    u64 start = s_ThreadStatus[i].jobStartTime;
                    if (now - start > 500) // 500ms threshold
                    {
                        // Only warn once per stuck job? 
                        // For now, just log.
                        // LH_CORE_WARN("Watchdog: Thread {0} stuck on job for {1} ms", i, now - start);
                        
                        // Note: This might spam if we have long running jobs (e.g. asset loading).
                        // We should probably have a way to mark jobs as "Long Running".
                    }
                }
            }
        }
    }
}
