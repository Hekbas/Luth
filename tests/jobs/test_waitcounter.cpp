// V1b — Lifetime hazard regression test.
//
// The Naughty-Dog lemming pattern: fan N leaf jobs from a worker fiber, wait on a
// stack-local Counter, return. The Counter's storage is reclaimed at the closing brace
// of its scope. Under MSVC ASan, that storage is poisoned at scope exit; if
// DecrementCounter's trailing fetch_sub(1) (the busy-bit clear) races past
// WaitForCounter's return, the wild write lands on the poisoned slot and ASan reports
// stack-use-after-scope.
//
// The fix at JobSystem.cpp:780-781 (commit 17cb1e3) makes WaitForCounter loop back
// after wake until the busy-bit spin path observes the cleared bit — so fetch_sub(1)
// completes before any waiter can return. This test is the canonical regression: a
// local `git revert 17cb1e3 && msbuild ... -p:Configuration=DebugASan` rebuilds with
// the bug present, and these test cases fail with ASan stack-use-after-scope inside
// the first few thousand iterations.
//
// Originating bug history: docs/development/history/v2.x/jobsystem-waitforcounter-uaf.md

#include <doctest/doctest.h>

#include "luth/jobs/JobSystem.h"
#include "support/JobSystemFixture.h"

#include <atomic>

namespace
{
    void LeafJob(Luth::JobSystem::JobArgs args)
    {
        static_cast<std::atomic<Luth::u64>*>(args.data)
            ->fetch_add(1, std::memory_order_relaxed);
    }

    // The lemming. Stack-local Counter lives only inside this function's scope; ASan
    // owns the shadow at the closing brace. Cross-worker timing (8 leaves on N workers,
    // wake from one, return on another) is what makes the race statistical — single
    // iterations almost never fire; 10K+ iterations fire reliably on the buggy code.
    void Lemming(Luth::JobSystem::JobArgs args)
    {
        auto* leaves = static_cast<std::atomic<Luth::u64>*>(args.data);

        Luth::JobSystem::Counter local;
        Luth::JobSystem::Dispatch(8, 1, LeafJob, leaves, &local, "Leaf",
                                  Luth::JobSystem::Priority::Normal);
        Luth::JobSystem::WaitForCounter(&local, 0);
    }

    void RunLemmings(Luth::u32 iterations)
    {
        constexpr Luth::u32 kLeavesPerLemming = 8;

        std::atomic<Luth::u64> leaves{0};
        Luth::JobSystem::Counter outer;
        Luth::JobSystem::Dispatch(iterations, 1, Lemming, &leaves, &outer, "Lemming",
                                  Luth::JobSystem::Priority::Normal);
        Luth::JobSystem::WaitForCounter(&outer, 0);

        CHECK(leaves.load() == static_cast<Luth::u64>(iterations) * kLeavesPerLemming);
    }
}

TEST_CASE_FIXTURE(LuthTests::JobSystemFixture,
                  "WaitForCounter UAF: stack-local Counter (V1b) (10K) [smoke]")
{
    RunLemmings(10'000);
}

TEST_CASE_FIXTURE(LuthTests::JobSystemFixture,
                  "WaitForCounter UAF: stack-local Counter (V1b) (100K) [stress]")
{
    RunLemmings(100'000);
}
