// V2 — Main Thread Starvation hazard.
//
// Invariant: the main thread (worker index 0) busy-spins on counters but never enters the worker steal loop.
// If it did, glfwPollEvents() would block whenever the scheduler picked up a heavy job. This test dispatches
// N jobs and records which worker index ran each one; the main thread's index (0) must not appear.

#include <doctest/doctest.h>

#include "luth/jobs/JobSystem.h"
#include "support/JobSystemFixture.h"

#include <atomic>
#include <vector>

namespace
{
    struct V2Data
    {
        std::vector<Luth::u32> threadIds;
    };

    void RecordTid(Luth::JobSystem::JobArgs args)
    {
        auto* d = static_cast<V2Data*>(args.data);
        d->threadIds[args.jobIndex] = Luth::JobSystem::GetWorkerThreadId();
    }
}

TEST_CASE_FIXTURE(LuthTests::JobSystemFixture, "V2 main thread never executes worker jobs [smoke]")
{
    constexpr Luth::u32 N = 10'000;
    V2Data data;
    data.threadIds.assign(N, ~Luth::u32{0});

    Luth::JobSystem::Counter c;
    Luth::JobSystem::Dispatch(N, 1, RecordTid, &data, &c, "RecordTID", Luth::JobSystem::Priority::Normal);
    Luth::JobSystem::WaitForCounter(&c, 0);

    // Main is index 0. If it stole a job, some slot would be 0.
    Luth::u32 mainSlotCount = 0;
    Luth::u32 unsetCount = 0;
    for (Luth::u32 tid : data.threadIds)
    {
        if (tid == 0) ++mainSlotCount;
        if (tid == ~Luth::u32{0}) ++unsetCount;
    }

    CHECK(mainSlotCount == 0);
    CHECK(unsetCount == 0);
}
