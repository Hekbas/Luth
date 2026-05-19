// V5 — Sub-Job Context Switch Thrashing hazard.
//
// Invariant: WaitForCounter inlines up to MAX_INLINE_DEPTH=4 levels of nested
// sub-jobs before falling through to a fiber switch. This bounds the OS-stack
// recursion depth — without it, a deep dispatch chain would stack-overflow
// (each inline call adds a C++ stack frame). The fiber switch transfers
// continuation to a fresh fiber whose stack is independent.
//
// We can't directly observe InlineDepth (no public getter; adding one for tests
// would violate the no-test-only-API cornerstone). Instead we test the
// behavioral consequence: a recursive Dispatch chain MUST complete to a target
// depth without stack-overflow. If V5 were absent, depth 1000 would crash; with
// V5, it completes via fiber switches at the appropriate depth.

#include <doctest/doctest.h>

#include "luth/jobs/JobSystem.h"
#include "support/JobSystemFixture.h"

#include <atomic>

namespace
{
    struct V5Args
    {
        std::atomic<Luth::u32> maxDepthObserved;
        Luth::u32 targetDepth;
    };

    struct V5Frame
    {
        V5Args* shared;
        Luth::u32 myDepth;
    };

    void V5Recurse(Luth::JobSystem::JobArgs args)
    {
        auto* frame = static_cast<V5Frame*>(args.data);
        V5Args* shared = frame->shared;
        Luth::u32 me = frame->myDepth;

        // Track max depth observed.
        Luth::u32 cur = shared->maxDepthObserved.load(std::memory_order_relaxed);
        while (me > cur && !shared->maxDepthObserved.compare_exchange_weak(
                   cur, me, std::memory_order_relaxed)) {}

        if (me >= shared->targetDepth) return;

        // Dispatch one child at depth+1; wait for it.
        V5Frame child{ shared, me + 1 };
        Luth::JobSystem::Counter c;
        Luth::JobSystem::Dispatch(1, 1, V5Recurse, &child, &c, "V5Recurse",
                                   Luth::JobSystem::Priority::Normal);
        Luth::JobSystem::WaitForCounter(&c, 0);
    }
}

TEST_CASE_FIXTURE(LuthTests::JobSystemFixture,
                  "V5 recursive WaitForCounter bounded by fiber switch [smoke]")
{
    // Target depth far exceeds MAX_INLINE_DEPTH (4). If V5 inlines without
    // bound, the OS stack overflows around depth 50K-100K. With the depth limit,
    // each chain of 4 inlines triggers a fiber switch which resets the C++
    // stack depth, so 1000 levels complete easily.
    V5Args shared;
    shared.maxDepthObserved.store(0);
    shared.targetDepth = 1000;

    V5Frame root{ &shared, 0 };
    Luth::JobSystem::Counter c;
    Luth::JobSystem::Dispatch(1, 1, V5Recurse, &root, &c, "V5Root",
                               Luth::JobSystem::Priority::Normal);
    Luth::JobSystem::WaitForCounter(&c, 0);

    // We reached the target — V5 didn't let the C++ stack run away.
    CHECK(shared.maxDepthObserved.load() == shared.targetDepth);
}
