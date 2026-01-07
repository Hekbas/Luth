#include "luthpch.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Profiler.h"
#include "luth/core/Log.h"

#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>

namespace Luth::JobSystem
{
    struct Job
    {
        JobFunction function;
        Counter* counter;
        u32 start;
        u32 end;
    };

    // Internal State
    static std::vector<std::thread> s_WorkerThreads;
    static std::deque<Job> s_JobQueue;
    static std::mutex s_QueueMutex;
    static std::condition_variable s_WakeCondition;
    static std::atomic<bool> s_Running = false;

    // Forward declaration
    static void WorkerLoop(u32 threadIndex);

    void Init(u32 numThreads)
    {
        if (s_Running) return;

        s_Running = true;
        
        // If 0, use hardware concurrency - 1 (leave one for main thread)
        if (numThreads == 0)
            numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);

        LH_CORE_INFO("Initializing JobSystem with {0} worker threads", numThreads);

        s_WorkerThreads.reserve(numThreads);
        for (u32 i = 0; i < numThreads; ++i)
        {
            s_WorkerThreads.emplace_back(std::bind(WorkerLoop, i));
        }
    }

    void Shutdown()
    {
        if (!s_Running) return;

        s_Running = false;
        s_WakeCondition.notify_all();

        for (auto& thread : s_WorkerThreads)
        {
            if (thread.joinable())
                thread.join();
        }

        s_WorkerThreads.clear();
        s_JobQueue.clear();
    }

    void Execute(const JobFunction& job, Counter* counter)
    {
        if (counter)
            counter->value++;

        {
            std::lock_guard<std::mutex> lock(s_QueueMutex);
            s_JobQueue.push_back({ job, counter, 0, 1 });
        }
        s_WakeCondition.notify_one();
    }

    void Dispatch(u32 jobCount, u32 groupSize, const JobFunction& job, Counter* counter)
    {
        if (jobCount == 0 || groupSize == 0) return;

        u32 groupCount = (jobCount + groupSize - 1) / groupSize;

        if (counter)
            counter->value += groupCount;

        {
            std::lock_guard<std::mutex> lock(s_QueueMutex);
            for (u32 i = 0; i < groupCount; ++i)
            {
                u32 start = i * groupSize;
                u32 end = std::min(start + groupSize, jobCount);
                s_JobQueue.push_back({ job, counter, start, end });
            }
        }
        s_WakeCondition.notify_all();
    }

    bool IsBusy(const Counter* counter)
    {
        return counter->value.load() > 0;
    }

    // The magic function: Help work while waiting
    void WaitForCounter(Counter* counter, u32 targetValue)
    {
        if (!counter) return;

        // While waiting, help execute jobs from the queue
        while (counter->value.load() > targetValue)
        {
            Job job;
            bool foundJob = false;

            {
                // Try to steal a job
                std::unique_lock<std::mutex> lock(s_QueueMutex);
                if (!s_JobQueue.empty())
                {
                    job = s_JobQueue.front();
                    s_JobQueue.pop_front();
                    foundJob = true;
                }
            }

            if (foundJob)
            {
                LH_PROFILE_SCOPE("Job_Execute_Stolen");
                // Execute the job
                JobArgs args{ job.start, 0 }; // TODO: Pass group index properly
                
                // Loop for grouped jobs
                for (u32 i = job.start; i < job.end; ++i)
                {
                    args.jobIndex = i;
                    job.function(args);
                }

                if (job.counter)
                    job.counter->value--;
            }
            else
            {
                // No jobs to steal, just yield/sleep briefly
                // In a fiber system, we would switch context here.
                std::this_thread::yield();
            }
        }
    }

    static void WorkerLoop(u32 threadIndex)
    {
        LH_PROFILE_TAG("Thread", "Worker");
        
        while (s_Running)
        {
            Job job;
            
            {
                std::unique_lock<std::mutex> lock(s_QueueMutex);
                s_WakeCondition.wait(lock, [] { return !s_JobQueue.empty() || !s_Running; });

                if (!s_Running && s_JobQueue.empty())
                    return;

                job = s_JobQueue.front();
                s_JobQueue.pop_front();
            }

            {
                LH_PROFILE_SCOPE("Job_Execute");
                
                JobArgs args{ 0, 0 };
                for (u32 i = job.start; i < job.end; ++i)
                {
                    args.jobIndex = i;
                    job.function(args);
                }

                if (job.counter)
                    job.counter->value--;
            }
        }
    }
}
