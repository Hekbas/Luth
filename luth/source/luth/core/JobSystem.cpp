#include "luthpch.h"
#include "JobSystem.h"
#include "Fiber.h"
#include "Log.h"

#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

// ===================================================================================
// Internal Implementation Details
// ===================================================================================

namespace Luth::JobSystem
{
    // -------------------------------------------------------------------------------
    // Data Structures
    // -------------------------------------------------------------------------------

    struct Job
    {
        JobFunction Function = nullptr;
        void* Data = nullptr;
        Counter* CounterPtr = nullptr;
        u32 JobIndex = 0;
        u32 GroupIndex = 0;
    };

    // Thread-Local State
    static thread_local u32 t_ThreadID = 0;
    static thread_local Fiber* t_CurrentFiber = nullptr;
    static thread_local Fiber t_MainThreadFiber; // The fiber representing the OS thread entry point
    static thread_local JobContext t_JobContext; // The actual storage for the context

    // Global State
    struct SchedulerData
    {
        std::vector<std::thread> WorkerThreads;
        std::atomic<bool> Running = false;
        u32 ThreadCount = 0;

        // Fiber Pool
        std::vector<Fiber> FiberPool;
        std::vector<u32> FreeFibers;
        std::mutex FiberPoolMutex;

        // Global Queue (High Priority / Overflow)
        // TODO: Replace with Lock-Free MPMC
        std::deque<Job> GlobalQueue;
        std::mutex GlobalQueueMutex;
        std::condition_variable WakeCondition; // Only used for initial wake-up, not per-job
        
        // Stats
        std::atomic<u32> PeakFibers = 0;
    };

    static SchedulerData s_Data;

    // -------------------------------------------------------------------------------
    // Fiber Management
    // -------------------------------------------------------------------------------

    static Fiber* AllocateFiber()
    {
        std::lock_guard<std::mutex> lock(s_Data.FiberPoolMutex);
        
        if (s_Data.FreeFibers.empty())
        {
            // Expand pool
            // TODO: Better expansion strategy
            LH_CORE_ERROR("Fiber Pool Exhausted! (This should be dynamic)");
            return nullptr; 
        }

        u32 index = s_Data.FreeFibers.back();
        s_Data.FreeFibers.pop_back();
        
        u32 used = (u32)s_Data.FiberPool.size() - (u32)s_Data.FreeFibers.size();
        u32 currentPeak = s_Data.PeakFibers.load();
        if (used > currentPeak) s_Data.PeakFibers.store(used);

        return &s_Data.FiberPool[index];
    }

    static void FreeFiber(Fiber* fiber)
    {
        // Calculate index from pointer
        u64 index = fiber - s_Data.FiberPool.data();
        
        std::lock_guard<std::mutex> lock(s_Data.FiberPoolMutex);
        s_Data.FreeFibers.push_back((u32)index);
    }

    // -------------------------------------------------------------------------------
    // Worker Loop
    // -------------------------------------------------------------------------------

    static void FiberEntryPoint(void* args)
    {
        Job* jobPtr = (Job*)args;
        
        // Execute the job
        if (jobPtr && jobPtr->Function)
        {
            JobArgs jArgs{ jobPtr->JobIndex, jobPtr->GroupIndex, jobPtr->Data };
            jobPtr->Function(jArgs);
            
            // Decrement counter if present
            if (jobPtr->CounterPtr)
            {
                // Atomic decrement
                u32 prev = jobPtr->CounterPtr->Value.fetch_sub(1);
                if (prev == 1)
                {
                    // We hit zero. Wake up waiting fibers.
                    // Lock-Free Stack Pop All
                    Fiber* waitingFiber = jobPtr->CounterPtr->WaitingListHead.exchange(nullptr);
                    while (waitingFiber)
                    {
                        Fiber* next = waitingFiber->NextWaiting;
                        
                        // Re-queue the waiting fiber as a "continuation job"
                        // For now, we just push it back to the global queue to be picked up
                        // In a real implementation, we might want to prioritize it
                        
                        // TODO: This part needs the Scheduler to support "Resuming Fibers"
                        // Current simplified model: We don't have a "Resume" job type yet.
                        // We need to add the fiber back to a "Ready List".
                        
                        waitingFiber = next;
                    }
                }
            }
        }

        // Job Complete. Return to Scheduler.
        // We need to switch back to the "Scheduler Fiber" (which is usually the thread's main loop)
        // BUT, in this architecture, the thread *is* the scheduler.
        // So we mark this fiber as free and pick a new job.
        
        // For Phase 1.1, we will just switch back to the MainThreadFiber of this thread
        // This is a simplification. Real workers stay in fibers.
        Fiber::SwitchTo(t_MainThreadFiber);
    }

    static void WorkerThreadEntryPoint(u32 threadID)
    {
        t_ThreadID = threadID;
        t_JobContext.ThreadIndex = threadID;
        
        // Convert this OS thread to a Fiber so we can switch away from it
        t_MainThreadFiber = Fiber::ConvertThreadToFiber(nullptr);
        t_CurrentFiber = &t_MainThreadFiber;

        while (s_Data.Running)
        {
            Job job;
            bool found = false;

            // 1. Try Global Queue
            {
                std::lock_guard<std::mutex> lock(s_Data.GlobalQueueMutex);
                if (!s_Data.GlobalQueue.empty())
                {
                    job = s_Data.GlobalQueue.front();
                    s_Data.GlobalQueue.pop_front();
                    found = true;
                }
            }

            if (found)
            {
                // Get a fiber
                Fiber* fiber = AllocateFiber();
                if (fiber)
                {
                    // Setup Fiber
                    // We can't easily reset the entry point of an existing Windows Fiber without recreating it
                    // OR using a trampoline.
                    // For Phase 1, we will assume the fiber is fresh or we use a trampoline.
                    // Actually, Windows Fibers maintain their state. To reuse them, they must have yielded,
                    // or we must delete/recreate them, OR they loop internally.
                    
                    // CORRECT APPROACH: The Fiber Function itself should be a loop:
                    // void FiberLoop() { while(1) { Run(Job); Yield(); } }
                    // But for now, let's just implement the switching mechanism.
                    
                    // TODO: Implement Fiber Reuse Strategy
                    
                    // Execute Job Inline for now to fix the hang (Phase 1.1)
                    // We are not switching fibers yet because we haven't implemented the trampoline
                    JobArgs jArgs{ job.JobIndex, job.GroupIndex, job.Data };
                    job.Function(jArgs);
                    
                    if (job.CounterPtr)
                    {
                         u32 prev = job.CounterPtr->Value.fetch_sub(1);
                    }
                    
                    FreeFiber(fiber);
                }
            }
            else
            {
                // Sleep / Yield
                // std::this_thread::yield(); // BUSY WAIT CAUSES 100% CPU
                
                // Better wait strategy for idle workers
                std::unique_lock<std::mutex> lock(s_Data.GlobalQueueMutex);
                s_Data.WakeCondition.wait(lock, []{ return !s_Data.GlobalQueue.empty() || !s_Data.Running; });
            }
        }
    }

    // -------------------------------------------------------------------------------
    // API Implementation
    // -------------------------------------------------------------------------------

    void Init(u32 numThreads)
    {
        if (numThreads == 0) numThreads = std::thread::hardware_concurrency() - 1;
        s_Data.ThreadCount = numThreads;
        s_Data.Running = true;

        // Initialize Fiber Pool (Pre-allocate)
        s_Data.FiberPool.resize(1024); // 1024 fibers
        for (u32 i = 0; i < 1024; ++i)
        {
            // We don't create the OS fiber yet, we do it on demand or here
            // Let's create them here for simplicity
            // NOTE: CreateFiber requires a function. We need a generic trampoline.
            // s_Data.FiberPool[i] = Fiber::Create(FiberTrampoline, &s_Data.FiberPool[i]);
            s_Data.FreeFibers.push_back(i);
        }

        // Spawn Workers
        for (u32 i = 0; i < numThreads; ++i)
        {
            s_Data.WorkerThreads.emplace_back(WorkerThreadEntryPoint, i + 1);
        }
        
        // Init Main Thread
        t_ThreadID = 0;
        t_JobContext.ThreadIndex = 0;
        t_MainThreadFiber = Fiber::ConvertThreadToFiber(nullptr);
        t_CurrentFiber = &t_MainThreadFiber;
        
        LH_CORE_INFO("JobSystem Initialized with {0} worker threads.", numThreads);
    }

    void Shutdown()
    {
        s_Data.Running = false;
        s_Data.WakeCondition.notify_all(); // Wake up all threads to exit
        
        for (auto& t : s_Data.WorkerThreads)
        {
            if (t.joinable()) t.join();
        }
        s_Data.WorkerThreads.clear();
        
        // Destroy Fibers
        for (auto& f : s_Data.FiberPool)
        {
            Fiber::Destroy(f);
        }
    }

    void ResetFrameStats()
    {
        s_Data.PeakFibers = 0;
    }

    void Execute(JobFunction function, void* data, Counter* counter)
    {
        if (counter) counter->Value.fetch_add(1);

        Job job;
        job.Function = function;
        job.Data = data;
        job.CounterPtr = counter;
        job.JobIndex = 0;
        job.GroupIndex = 0;

        {
            std::lock_guard<std::mutex> lock(s_Data.GlobalQueueMutex);
            s_Data.GlobalQueue.push_back(job);
        }
        s_Data.WakeCondition.notify_one();
    }

    void Dispatch(u32 jobCount, u32 groupSize, JobFunction function, void* data, Counter* counter)
    {
        if (jobCount == 0) return;

        u32 groupCount = (jobCount + groupSize - 1) / groupSize;
        
        if (counter) counter->Value.fetch_add(groupCount);

        {
            std::lock_guard<std::mutex> lock(s_Data.GlobalQueueMutex);
            for (u32 i = 0; i < groupCount; ++i)
            {
                Job job;
                job.Function = function;
                job.Data = data;
                job.CounterPtr = counter;
                job.JobIndex = i * groupSize; // Base index
                job.GroupIndex = i;
                s_Data.GlobalQueue.push_back(job);
            }
        }
        s_Data.WakeCondition.notify_all();
    }

    void WaitForCounter(Counter* counter, u32 targetValue)
    {
        if (!counter) return;

        // Main Thread Wait Strategy (Phase 1)
        // We are on the main thread, waiting for workers to finish.
        // We must help!
        
        while (counter->Value.load() > targetValue)
        {
            // Try to help (run local jobs)
            Job job;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(s_Data.GlobalQueueMutex);
                if (!s_Data.GlobalQueue.empty())
                {
                    job = s_Data.GlobalQueue.front();
                    s_Data.GlobalQueue.pop_front();
                    found = true;
                }
            }

            if (found)
            {
                // Execute inline (on main thread stack) for now, since we are not fully fiber-switched on main thread yet
                // This is safe for Phase 1 as long as we don't recurse too deep
                JobArgs jArgs{ job.JobIndex, job.GroupIndex, job.Data };
                job.Function(jArgs);
                
                if (job.CounterPtr)
                {
                     u32 prev = job.CounterPtr->Value.fetch_sub(1);
                     // Note: We don't handle waking fibers here because we are just helping
                }
            }
            else
            {
                // No work to do, just yield
                std::this_thread::yield();
            }
        }
    }

    void YieldFiber()
    {
        // Switch back to scheduler/main fiber
        // TODO: Implement
    }

    bool IsBusy(const Counter* counter)
    {
        return counter && counter->Value.load() > 0;
    }

    Stats GetStats()
    {
        u32 queueSize = 0;
        {
            std::lock_guard<std::mutex> lock(s_Data.GlobalQueueMutex);
            queueSize = (u32)s_Data.GlobalQueue.size();
        }
        return { s_Data.ThreadCount, (u32)s_Data.FiberPool.size(), (u32)s_Data.FreeFibers.size(), s_Data.PeakFibers.load(), queueSize };
    }

    u32 GetWorkerThreadId()
    {
        return t_ThreadID;
    }
    
    void ExecuteMainThreadLoop()
    {
        // TODO: Implement the main thread message pump + job stealing loop
    }

    JobContext* GetCurrentJobContext()
    {
        return &t_JobContext;
    }

    void SetGlobalCommandPool(CommandAllocatorPool* pool)
    {
        t_JobContext.CommandPool = pool;
    }
}
