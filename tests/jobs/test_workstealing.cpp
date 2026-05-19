// WorkStealingDeque — Chase-Lev (SPAA 2005). Single-owner: only its owning thread
// may call Push/TryPop (LIFO from bottom). Any thread may TrySteal (FIFO from top).
// Resize doubles capacity in the owner's Push path; old buffers are intentionally
// leaked because thieves may still hold pointers to them (frame-lifetime, moot).

#include <doctest/doctest.h>

#include "luth/core/types/LuthTypes.h"
#include "luth/jobs/WorkStealingDeque.h"

#include <atomic>
#include <thread>
#include <unordered_set>
#include <vector>

using Luth::WorkStealingDeque;

TEST_CASE("WorkStealingDeque: owner Push then TryPop LIFO [smoke]")
{
    WorkStealingDeque<Luth::u32> d;
    d.Push(1);
    d.Push(2);
    d.Push(3);
    Luth::u32 v;
    REQUIRE(d.TryPop(v)); CHECK(v == 3);
    REQUIRE(d.TryPop(v)); CHECK(v == 2);
    REQUIRE(d.TryPop(v)); CHECK(v == 1);
    CHECK_FALSE(d.TryPop(v));
}

TEST_CASE("WorkStealingDeque: owner Push triggers resize past initial capacity [smoke]")
{
    WorkStealingDeque<Luth::u32> d(/*initialCapacity*/16);
    constexpr Luth::u32 kCount = 1024; // forces several doublings
    for (Luth::u32 i = 0; i < kCount; ++i) d.Push(i);

    // Pop everything back — LIFO order. Every push must reappear exactly once.
    Luth::u64 sum = 0;
    Luth::u32 popped = 0;
    Luth::u32 v;
    while (d.TryPop(v))
    {
        sum += v;
        ++popped;
    }
    CHECK(popped == kCount);
    const Luth::u64 expected =
        static_cast<Luth::u64>(kCount) * (kCount - 1) / 2;
    CHECK(sum == expected);
}

TEST_CASE("WorkStealingDeque: owner Pop racing N thieves Steal preserves every item [stress]")
{
    // Property: each pushed item is observed by exactly one of (owner-Pop, any-Steal).
    // Owner runs in main thread; thieves are worker threads. Owner pushes a batch,
    // drains via TryPop, while thieves drain via TrySteal in parallel.
    WorkStealingDeque<Luth::u32> d;
    constexpr Luth::u32 kThieves = 4;
    constexpr Luth::u32 kItems = 50'000;

    std::atomic<bool> ownerDone{false};

    // Pre-push everything so thieves have a juicy steal target.
    for (Luth::u32 i = 0; i < kItems; ++i) d.Push(i);

    std::vector<std::vector<Luth::u32>> thiefHauls(kThieves);
    std::vector<std::thread> thieves;
    for (Luth::u32 t = 0; t < kThieves; ++t)
    {
        thieves.emplace_back([&, t]() {
            Luth::u32 v;
            while (!ownerDone.load(std::memory_order_acquire))
            {
                if (d.TrySteal(v)) thiefHauls[t].push_back(v);
            }
            // Final drain attempt after owner stops.
            while (d.TrySteal(v)) thiefHauls[t].push_back(v);
        });
    }

    // Owner's pop loop.
    std::vector<Luth::u32> ownerHaul;
    {
        Luth::u32 v;
        while (d.TryPop(v)) ownerHaul.push_back(v);
    }
    ownerDone.store(true, std::memory_order_release);
    for (auto& th : thieves) th.join();

    // Merge all observations.
    std::unordered_set<Luth::u32> seen;
    seen.reserve(kItems);
    Luth::u32 total = 0;
    for (Luth::u32 v : ownerHaul) { ++total; seen.insert(v); }
    for (auto& haul : thiefHauls)
    {
        for (Luth::u32 v : haul) { ++total; seen.insert(v); }
    }
    CHECK(total == kItems);
    CHECK(seen.size() == kItems);
}
