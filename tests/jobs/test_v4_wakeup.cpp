// V4 — WaitOnAddress Lost Wakeup hazard.
//
// Invariant: idle workers WaitOnAddress on the HighQueue's generation counter; every queue push pairs
// with WakeByAddressSingle. If a wake is ever lost, the worker keeps sleeping and the pushed job never
// runs. We can't directly observe sleeping-state, but if any dispatch+wait cycle BLOCKS FOREVER, the
// wake mechanism is broken. This test does many quick dispatch+wait cycles with brief gaps for workers
// to go idle between them; if any cycle hangs, the test never returns and CI/manual timeout catches it.

#include <doctest/doctest.h>

#include "luth/jobs/JobSystem.h"
#include "support/JobSystemFixture.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
    void TinyJob(Luth::JobSystem::JobArgs args)
    {
        static_cast<std::atomic<Luth::u32>*>(args.data)->fetch_add(1, std::memory_order_relaxed);
    }
}

TEST_CASE_FIXTURE(LuthTests::JobSystemFixture, "V4 wake-after-idle: many dispatch/wait cycles with gaps [smoke]")
{
    // After WaitForCounter returns, the main thread sleeps briefly so workers hit the idle path
    // (WaitOnAddress on the gen counter). Then we dispatch and wait again. A lost wakeup would
    // block this second WaitForCounter.
    constexpr Luth::u32 kCycles = 200;

    std::atomic<Luth::u32> totalExecuted{0};

    for (Luth::u32 i = 0; i < kCycles; ++i)
    {
        Luth::JobSystem::Counter c;
        // 4 jobs per cycle — small enough to distribute across cores, not enough to keep all
        // workers busy until the next iteration.
        Luth::JobSystem::Dispatch(4, 1, TinyJob, &totalExecuted, &c, "V4Tiny", Luth::JobSystem::Priority::High);
        Luth::JobSystem::WaitForCounter(&c, 0);

        // Brief gap so workers actually enter the idle/sleep path.
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    CHECK(totalExecuted.load() == kCycles * 4);
}
