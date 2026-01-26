#pragma once

#include "luth/core/JobSystem.h"
#include "luth/core/Log.h"
#include "luth/core/Timer.h"
#include <vector>
#include <atomic>

namespace Luth::JobSystem::Tests
{
    // ===================================================================================
    // Test 1: Basic Parallel Dispatch
    // Spawns N jobs that increment an atomic counter.
    // Verifies that all jobs ran.
    // ===================================================================================
    void TestParallelDispatch()
    {
        LH_CORE_INFO("Running Test: Parallel Dispatch...");
        
        constexpr u32 JOB_COUNT = 10000;
        std::atomic<u32> counter = 0;
        JobSystem::Counter waitCounter;

        Timer timer;
        JobSystem::Dispatch(JOB_COUNT, 1, [](JobSystem::JobArgs args) {
            std::atomic<u32>* c = (std::atomic<u32>*)args.data;
            c->fetch_add(1);
            
            // Simulate tiny work
            for(int i=0; i<100; ++i) _mm_pause();
            
        }, &counter, &waitCounter);

        JobSystem::WaitForCounter(&waitCounter);
        f32 duration = timer.ElapsedMillis();

        if (counter.load() == JOB_COUNT)
        {
            LH_CORE_INFO("SUCCESS: Processed {0} jobs in {1:.2f}ms", JOB_COUNT, duration);
        }
        else
        {
            LH_CORE_ERROR("FAILURE: Expected {0}, got {1}", JOB_COUNT, counter.load());
        }
    }

    // ===================================================================================
    // Test 2: Dependency Chain (Fiber Yielding)
    // Job A spawns Job B and waits for it.
    // Verifies that Fiber A yields and resumes correctly.
    // ===================================================================================
    struct DependencyData
    {
        std::atomic<u32> value = 0;
    };

    void JobB(JobSystem::JobArgs args)
    {
        DependencyData* data = (DependencyData*)args.data;
        // Simulate work
        for(int i=0; i<1000; ++i) _mm_pause();
        data->value.store(1);
    }

    void JobA(JobSystem::JobArgs args)
    {
        DependencyData* data = (DependencyData*)args.data;
        
        JobSystem::Counter c;
        JobSystem::Execute(JobB, data, &c);
        
        // This should cause the fiber to yield
        JobSystem::WaitForCounter(&c);
        
        // Verify B finished
        if (data->value.load() == 1)
        {
            data->value.store(2); // Mark A finished
        }
    }

    void TestFiberYielding()
    {
        LH_CORE_INFO("Running Test: Fiber Yielding...");
        
        DependencyData data;
        JobSystem::Counter mainCounter;
        
        JobSystem::Execute(JobA, &data, &mainCounter);
        JobSystem::WaitForCounter(&mainCounter);
        
        if (data.value.load() == 2)
        {
            LH_CORE_INFO("SUCCESS: Fiber yielded and resumed correctly.");
        }
        else
        {
            LH_CORE_ERROR("FAILURE: Fiber dependency chain broken. Value: {0}", data.value.load());
        }
    }

    // ===================================================================================
    // Test 3: The "Million Job" Stress Test
    // Spawns 1,000,000 empty jobs to measure overhead.
    // ===================================================================================
    void TestStress()
    {
        LH_CORE_INFO("Running Test: Million Job Stress...");
        
        constexpr u32 JOB_COUNT = 1000000;
        constexpr u32 GROUP_SIZE = 1; // Force individual scheduling overhead
        
        JobSystem::Counter waitCounter;
        Timer timer;
        
        JobSystem::Dispatch(JOB_COUNT, GROUP_SIZE, [](JobSystem::JobArgs){}, nullptr, &waitCounter);
        
        JobSystem::WaitForCounter(&waitCounter);
        f32 duration = timer.ElapsedMillis();
        
        LH_CORE_INFO("SUCCESS: 1 Million Jobs in {0:.2f}ms ({1:.2f} jobs/ms)", duration, JOB_COUNT / duration);
    }

    void RunAll()
    {
        TestParallelDispatch();
        TestFiberYielding();
        TestStress();
    }
}
