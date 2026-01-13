#include "luthpch.h"
#include "VulkanWaitJob.h"
#include "TimelineSemaphore.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Log.h"

namespace Luth
{
    struct WaitJobData
    {
        TimelineSemaphore* semaphore;
        u64 targetValue;
        JobSystem::Counter* counter;
    };

    static void WaitJobFunction(JobSystem::JobArgs args)
    {
        WaitJobData* data = (WaitJobData*)args.data;
        
        u64 currentValue = data->semaphore->GetValue();

        if (currentValue >= data->targetValue)
        {
            // Condition met!
            // Decrement the counter to signal completion to whoever is waiting on this job.
            if (data->counter)
            {
                data->counter->value--;
            }
            
            // We are done with this data. 
            // Note: In a real system, we'd need to handle memory lifetime of 'data'.
            // Ideally, 'data' is allocated from a frame allocator or pool.
            // For this implementation, we assume the caller handles it or we leak it (bad).
            // TODO: Use a pool allocator for these small job structs.
            delete data;
        }
        else
        {
            // Condition NOT met.
            // Re-queue the job to run again later.
            // We don't decrement the counter yet.
            
            // Yielding strategy:
            // We just re-submit the job. The scheduler will pick it up again.
            // Ideally, we would put it in a "sleeping" list, but re-queueing works for now (busy-wait with yield).
            
            // IMPORTANT: We must NOT decrement the counter here, because the "logical" job isn't done.
            // But the JobSystem::Execute call *increments* the counter.
            // So if we call Execute again, the counter goes up.
            // We want to keep the *same* counter active.
            
            // Hack: We manually re-submit without touching the counter's public API if possible,
            // or we just pass nullptr for the counter in the recursive call, 
            // but we need to keep the original counter alive.
            
            // Correct approach with current JobSystem API:
            // The current job execution is finishing.
            // We spawn a NEW job that continues the wait.
            // The original counter should NOT be decremented by the system when this function returns?
            // The JobSystem decrements the counter automatically when a job function returns if we passed it.
            
            // Wait. The JobSystem::Execute increments. The Worker decrements when function returns.
            // So if we return, the counter goes down.
            // We want the counter to stay UP until the condition is met.
            
            // Solution: Increment the counter manually before returning, so the system's decrement cancels out.
            if (data->counter)
            {
                data->counter->value++;
            }
            
            // Re-submit
            // We use a small delay or just let it run. 
            // In a fiber system, we should probably yield to other fibers.
            // Since we are re-queuing, we are effectively yielding.
            JobSystem::Execute(WaitJobFunction, data, nullptr); // Pass nullptr so Execute doesn't increment again
        }
    }

    void VulkanWaitJob::Dispatch(TimelineSemaphore& semaphore, u64 targetValue, JobSystem::Counter* counter)
    {
        // Allocate data. 
        // TODO: Use a proper allocator. For now, new/delete is risky but functional for prototype.
        WaitJobData* data = new WaitJobData{ &semaphore, targetValue, counter };
        
        // We increment the counter here because the job system will decrement it when the job finishes.
        // But our job might "finish" (return) many times before it's actually done.
        // See logic in WaitJobFunction.
        
        if (counter)
        {
            counter->value++;
        }

        JobSystem::Execute(WaitJobFunction, data, nullptr);
    }
}
