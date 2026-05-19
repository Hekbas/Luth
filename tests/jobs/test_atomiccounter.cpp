// AtomicCounter — Increment/Decrement protocol + busy bit serialization.
//
// Value layout: Bit 0 = busy flag, Bits 1-31 = count. Increment shifts by 1 to
// keep the busy bit clear. Decrement routes through the same wake path as a
// completing job, including the busy-bit serialization at count 0.

#include <doctest/doctest.h>

#include "luth/jobs/AtomicCounter.h"
#include "support/JobSystemFixture.h"

#include <atomic>
#include <thread>

using Luth::JobSystem::AtomicCounter;

TEST_CASE("AtomicCounter: default Value is 0 [smoke]")
{
    AtomicCounter c;
    CHECK(c.Value.load() == 0);
}

TEST_CASE("AtomicCounter: explicit-init shifts count into bits 1+ [smoke]")
{
    AtomicCounter c(5);
    // 5 << 1 = 10. Bit 0 (busy) clear. Count = 5 in bits 1-31.
    CHECK(c.Value.load() == 10);
    CHECK((c.Value.load() & 1) == 0);
    CHECK((c.Value.load() >> 1) == 5);
}

TEST_CASE_FIXTURE(LuthTests::JobSystemFixture,
                  "AtomicCounter: Increment(N) + N Decrements lands at 0 [smoke]")
{
    AtomicCounter c;
    constexpr Luth::u32 N = 1000;
    c.Increment(N);
    CHECK((c.Value.load() >> 1) == N);

    c.Decrement(N);
    CHECK(c.Value.load() == 0);
}

TEST_CASE_FIXTURE(LuthTests::JobSystemFixture,
                  "AtomicCounter: concurrent Increment/Decrement balance [stress]")
{
    AtomicCounter c;
    constexpr Luth::u32 kThreads = 8;
    constexpr Luth::u32 kIterations = 10'000;

    // Pre-load so Decrement has count to draw from.
    c.Increment(kThreads * kIterations);
    CHECK((c.Value.load() >> 1) == kThreads * kIterations);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (Luth::u32 t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&c]() {
            for (Luth::u32 i = 0; i < kIterations; ++i)
            {
                c.Decrement();
            }
        });
    }
    for (auto& t : threads) t.join();

    CHECK(c.Value.load() == 0);
}
