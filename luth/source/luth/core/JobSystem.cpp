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
#include <emmintrin.h> // For _mm_pause

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
        
        // Ready Fibers (Fibers that were waiting and are now ready to resume)
        std::deque<Fiber*> ReadyFibers;
        std::mutex ReadyFibersMutex;

        // Global Contexts (Propagated to workers)
        std::atomic<CommandAllocatorPool*> GlobalCommandPool = nullptr;

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
            LH_CORE_WARN("Fiber Pool Expanding! Current Size: {0}", s_Data.FiberPool.size());
            
            // TODO: Better expansion strategy
            // For now, we return nullptr if exhausted, but we should expand
            // But FiberPool is a vector of objects. If we resize, pointers invalidate!
            // We store Fiber* in waiting lists.
            // So we CANNOT resize FiberPool vector if fibers are in use.
            // We must use a deque or list of chunks.
            // Or pre-allocate enough.
            
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

    // ... (Rest of the file remains same) ...
    // I will just update AllocateFiber to log if expanding (commented out)
    // and keep the rest.

    // -------------------------------------------------------------------------------
    // Worker Loop
    // -------------------------------------------------------------------------------

    static void FiberEntryPoint(void* args)
    {
        Job* jobPtr = (Job*)args;
        
        // Update Context from Global State
        // This ensures the fiber has access to the current frame's resources
        t_JobContext.CommandPool = s_Data.GlobalCommandPool.load(std::memory_order_relaxed);
        
        // Execute the job
        if (jobPtr && jobPtr->Function)
        {
            JobArgs jArgs{ jobPtr->JobIndex, jobPtr->GroupIndex, jobPtr->Data };
            jobPtr->Function(jArgs);
            
            // Decrement counter if present
            if (jobPtr->CounterPtr)
            {
                // Atomic decrement with Busy Bit logic (Bit 0 = Busy, Bits 1..31 = Count)
                // We shift the count by 1.
                u32 old = jobPtr->CounterPtr->Value.load();
                while(true)
                {
                    // If Busy (Odd), we must wait for it to clear to avoid race conditions
                    if ((old & 1) == 1)
                    {
                        _mm_pause();
                        old = jobPtr->CounterPtr->Value.load();
                        continue;
                    }

                    if (old < 2) break; // Should not happen if logic is correct (0 is free)

                    if (old == 2) // Last job (Count 1 -> 0)
                    {
                        // Try to set Busy Bit (Value 1)
                        if (jobPtr->CounterPtr->Value.compare_exchange_weak(old, 1)) break;
                    }
                    else // old > 2
                    {
                        // Decrement count by 1 (Value - 2)
                        if (jobPtr->CounterPtr->Value.compare_exchange_weak(old, old - 2)) break;
                    }
                }

                if (old == 2)
                {
                    // We hit zero count. Wake up waiting fibers.
                    // Acquire Lock
                    while (jobPtr->CounterPtr->Lock.test_and_set(std::memory_order_acquire)) { _mm_pause(); }
                    
                    Fiber* waitingFiber = jobPtr->CounterPtr->WaitingListHead;
                    jobPtr->CounterPtr->WaitingListHead = nullptr;
                    
                    jobPtr->CounterPtr->Lock.clear(std::memory_order_release);

                    while (waitingFiber)
                    {
                        Fiber* next = waitingFiber->NextWaiting;
                        
                        // Add to Ready List
                        {
                            std::lock_guard<std::mutex> lock(s_Data.ReadyFibersMutex);
                            s_Data.ReadyFibers.push_back(waitingFiber);
                        }
                        s_Data.WakeCondition.notify_one();
                        
                        waitingFiber = next;
                    }
                    
                    // Clear Busy Bit (Value 1 -> 0)
                    jobPtr->CounterPtr->Value.fetch_sub(1);
                }
            }
        }

        // Mark as finished so the scheduler knows to free it
        t_CurrentFiber->IsFinished = true;

        // Job Complete. Return to Scheduler.
        // We need to switch back to the "Scheduler Fiber" (which is usually the thread's main loop)
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
            // Update Context for the worker thread itself (in case it runs inline or needs it)
            t_JobContext.CommandPool = s_Data.GlobalCommandPool.load(std::memory_order_relaxed);

            Job job;
            Fiber* readyFiber = nullptr;
            bool foundJob = false;
            bool foundFiber = false;

            // 1. Check Ready Fibers (High Priority Resumption)
            {
                std::lock_guard<std::mutex> lock(s_Data.ReadyFibersMutex);
                if (!s_Data.ReadyFibers.empty())
                {
                    readyFiber = s_Data.ReadyFibers.front();
                    s_Data.ReadyFibers.pop_front();
                    foundFiber = true;
                }
            }

            if (foundFiber)
            {
                // Resume Fiber
                t_CurrentFiber = readyFiber;
                Fiber::SwitchTo(*readyFiber);
                t_CurrentFiber = &t_MainThreadFiber; // Back from fiber
                
                // If the fiber finished (it switched back to us), we free it.
                if (readyFiber->IsFinished)
                {
                    delete (Job*)readyFiber->Args;
                    FreeFiber(readyFiber);
                }
                else if (readyFiber->WaitCounter)
                {
                    // The fiber yielded because it's waiting.
                    // It is already in the counter's wait list (added by WaitForCounter).
                }
                
                continue;
            }

            // 2. Try Global Queue
            {
                std::lock_guard<std::mutex> lock(s_Data.GlobalQueueMutex);
                if (!s_Data.GlobalQueue.empty())
                {
                    job = s_Data.GlobalQueue.front();
                    s_Data.GlobalQueue.pop_front();
                    foundJob = true;
                }
            }

            if (foundJob)
            {
                // Get a fiber
                Fiber* fiber = AllocateFiber();
                if (fiber)
                {
                    // Setup Fiber
                    // Destroy old fiber handle if it exists to reset stack/registers
                    if (fiber->Handle) Fiber::Destroy(*fiber);
                    
                    // Allocate job on heap to pass safely to fiber
                    Job* heapJob = new Job(job);
                    
                    // Use Move Assignment for Fiber
                    *fiber = Fiber::Create(FiberEntryPoint, heapJob);
                    
                    t_CurrentFiber = fiber;
                    Fiber::SwitchTo(*fiber);
                    t_CurrentFiber = &t_MainThreadFiber;
                    
                    // Cleanup
                    // NOTE: We do NOT delete heapJob here blindly.
                    // If the fiber yielded, heapJob must remain valid.
                    
                    // If the fiber finished, we free it.
                    if (fiber->IsFinished)
                    {
                        delete (Job*)fiber->Args;
                        FreeFiber(fiber);
                    }
                }
            }
            else
            {
                // Sleep / Yield
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
        if (counter) counter->Value.fetch_add(2); // Add 1 count (shifted by 1)

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
        
        if (counter) counter->Value.fetch_add(groupCount << 1); // Add groupCount (shifted by 1)

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

        u32 target = targetValue << 1;

        // Check if we are in a fiber or main thread
        // If main thread (and not converted to fiber yet properly), we must busy wait/help
        if (t_CurrentFiber == &t_MainThreadFiber)
        {
             while (true)
            {
                u32 v = counter->Value.load();
                if (v <= target && (v & 1) == 0) return;

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
                    JobArgs jArgs{ job.JobIndex, job.GroupIndex, job.Data };
                    job.Function(jArgs);
                    
                    if (job.CounterPtr)
                    {
                        // Inline decrement logic (same as FiberEntryPoint)
                        u32 old = job.CounterPtr->Value.load();
                        while(true)
                        {
                            if ((old & 1) == 1) { _mm_pause(); old = job.CounterPtr->Value.load(); continue; }
                            if (old < 2) break;
                            if (old == 2) { if (job.CounterPtr->Value.compare_exchange_weak(old, 1)) break; }
                            else { if (job.CounterPtr->Value.compare_exchange_weak(old, old - 2)) break; }
                        }

                        if (old == 2)
                        {
                             while (job.CounterPtr->Lock.test_and_set(std::memory_order_acquire)) { _mm_pause(); }
                             
                             Fiber* waitingFiber = job.CounterPtr->WaitingListHead;
                             job.CounterPtr->WaitingListHead = nullptr;
                             
                             job.CounterPtr->Lock.clear(std::memory_order_release);

                             while (waitingFiber)
                             {
                                 Fiber* next = waitingFiber->NextWaiting;
                                 {
                                     std::lock_guard<std::mutex> lock(s_Data.ReadyFibersMutex);
                                     s_Data.ReadyFibers.push_back(waitingFiber);
                                 }
                                 s_Data.WakeCondition.notify_one();
                                 waitingFiber = next;
                             }
                             job.CounterPtr->Value.fetch_sub(1);
                        }
                    }
                }
                else
                {
                    std::this_thread::yield();
                }
            }
            return;
        }

        // We are in a worker fiber. Yield!
        while (true)
        {
            u32 v = counter->Value.load();
            if (v <= target && (v & 1) == 0) return;

            // If Busy, wait for it to clear
            if ((v & 1) == 1)
            {
                std::this_thread::yield();
                continue;
            }

            // Add to wait list (Protected by SpinLock)
            while (counter->Lock.test_and_set(std::memory_order_acquire)) { _mm_pause(); }
            
            // Double check value inside lock
            v = counter->Value.load();
            if (v <= target && (v & 1) == 0)
            {
                counter->Lock.clear(std::memory_order_release);
                return;
            }
            
            // If Busy, we must back off because the decrementer might have already grabbed the list
            if ((v & 1) == 1)
            {
                counter->Lock.clear(std::memory_order_release);
                std::this_thread::yield();
                continue;
            }

            t_CurrentFiber->NextWaiting = counter->WaitingListHead;
            counter->WaitingListHead = t_CurrentFiber;
            
            counter->Lock.clear(std::memory_order_release);

            // Set wait state
            t_CurrentFiber->WaitCounter = counter;
            t_CurrentFiber->WaitTarget = targetValue;

            // Switch back to scheduler
            Fiber::SwitchTo(t_MainThreadFiber);
            return; // Resumed
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
        s_Data.GlobalCommandPool.store(pool, std::memory_order_relaxed);
    }
}
