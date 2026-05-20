// SpinLock — V1: pure spin, no yield, < 100-cycle critical sections only. TTAS pattern
// (test-and-test-and-set) + _mm_pause to reduce cache thrash.

#include <doctest/doctest.h>

#include "luth/core/types/LuthTypes.h"
#include "luth/jobs/SpinLock.h"

#include <atomic>
#include <thread>
#include <vector>

using Luth::SpinLock;
using Luth::SpinLockGuard;

TEST_CASE("SpinLock: basic Lock/Unlock + TryLock state [smoke]")
{
    SpinLock lock;
    CHECK(lock.TryLock());
    // Already held — TryLock from same thread must fail (spin-lock is not reentrant).
    CHECK_FALSE(lock.TryLock());
    lock.Unlock();
    CHECK(lock.TryLock());
    lock.Unlock();
}

TEST_CASE("SpinLock: contention preserves critical-section count [stress]")
{
    SpinLock lock;
    constexpr Luth::u32 kThreads = 8;
    constexpr Luth::u32 kIncrementsPerThread = 50'000;

    // Plain int (not atomic) — every increment MUST happen under lock for the final value to be
    // correct. Any lost update = SpinLock contract violation.
    Luth::u64 counter = 0;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (Luth::u32 t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&]() {
            for (Luth::u32 i = 0; i < kIncrementsPerThread; ++i)
            {
                SpinLockGuard guard(lock);
                ++counter;
            }
        });
    }
    for (auto& t : threads) t.join();

    const Luth::u64 expected = static_cast<Luth::u64>(kThreads) * kIncrementsPerThread;
    CHECK(counter == expected);
}
