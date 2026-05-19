// MPMCQueue — Vyukov bounded lock-free MPMC. Capacity must be power of 2.
// V4 (see arch/version-glossary.md): every push pairs with WakeByAddressSingle;
// JobSystem workers WaitOnAddress on the gen counter and never miss a wakeup.
// This file isolates the queue itself; the JobSystem-level V4 test
// (test_v4_wakeup.cpp) covers the engine-level integration.

#include <doctest/doctest.h>

#include "luth/core/types/LuthTypes.h"
#include "luth/jobs/MPMCQueue.h"

#include <atomic>
#include <thread>
#include <vector>

using Luth::MPMCQueue;

TEST_CASE("MPMCQueue: TryPush/TryPop FIFO single-thread [smoke]")
{
    MPMCQueue<Luth::u32, 16> q;
    REQUIRE(q.TryPush(1));
    REQUIRE(q.TryPush(2));
    REQUIRE(q.TryPush(3));
    Luth::u32 v;
    REQUIRE(q.TryPop(v)); CHECK(v == 1);
    REQUIRE(q.TryPop(v)); CHECK(v == 2);
    REQUIRE(q.TryPop(v)); CHECK(v == 3);
    CHECK_FALSE(q.TryPop(v));
}

TEST_CASE("MPMCQueue: TryPush returns false when full [smoke]")
{
    MPMCQueue<Luth::u32, 4> q;
    CHECK(q.TryPush(1));
    CHECK(q.TryPush(2));
    CHECK(q.TryPush(3));
    CHECK(q.TryPush(4));
    // Capacity 4 full; further pushes refuse.
    CHECK_FALSE(q.TryPush(99));
}

TEST_CASE("MPMCQueue: N producers + M consumers preserve every item [stress]")
{
    // Frostbite-style property test. Each producer pushes a unique tag range;
    // consumers pop and accumulate. After all done, total count must match and
    // the sum must equal the expected arithmetic-series sum (no losses, no
    // duplicates — every value popped exactly once).
    constexpr Luth::u32 kProducers = 4;
    constexpr Luth::u32 kConsumers = 4;
    constexpr Luth::u32 kItemsPerProducer = 25'000;
    constexpr Luth::u32 kTotal = kProducers * kItemsPerProducer;

    MPMCQueue<Luth::u32, 4096> q;
    std::atomic<Luth::u32> popCount{0};
    std::atomic<Luth::u64> popSum{0};
    std::atomic<bool> producersDone{false};

    std::vector<std::thread> threads;
    for (Luth::u32 p = 0; p < kProducers; ++p)
    {
        threads.emplace_back([&, p]() {
            const Luth::u32 base = p * kItemsPerProducer;
            for (Luth::u32 i = 0; i < kItemsPerProducer; ++i)
            {
                const Luth::u32 value = base + i;
                while (!q.TryPush(value)) {}
            }
        });
    }
    for (Luth::u32 c = 0; c < kConsumers; ++c)
    {
        threads.emplace_back([&]() {
            while (true)
            {
                Luth::u32 v;
                if (q.TryPop(v))
                {
                    popSum.fetch_add(v, std::memory_order_relaxed);
                    popCount.fetch_add(1, std::memory_order_relaxed);
                }
                else if (producersDone.load(std::memory_order_acquire))
                {
                    // One more drain attempt after producers finish.
                    if (!q.TryPop(v)) break;
                    popSum.fetch_add(v, std::memory_order_relaxed);
                    popCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Wait for producers.
    for (Luth::u32 i = 0; i < kProducers; ++i) threads[i].join();
    producersDone.store(true, std::memory_order_release);
    for (Luth::u32 i = kProducers; i < threads.size(); ++i) threads[i].join();

    // Sum of [0, kTotal) = kTotal * (kTotal - 1) / 2
    const Luth::u64 expectedSum =
        static_cast<Luth::u64>(kTotal) * (kTotal - 1) / 2;
    CHECK(popCount.load() == kTotal);
    CHECK(popSum.load() == expectedSum);
}
