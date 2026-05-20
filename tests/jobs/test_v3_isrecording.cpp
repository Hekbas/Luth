// V3 — VkCommandBuffer Thread Violation hazard.
//
// Invariant: RecordingScope sets JobContext::IsRecording = true for its lifetime; Fiber::SwitchTo asserts
// !IsRecording so yielding during a Vulkan command-buffer recording aborts in debug. We can't catch the
// abort from doctest (it's an assert() → process abort, not a C++ throw), so this test validates the
// PREDICATE STATE TRANSITIONS that the assertion gates on. If the assertion ever fires in production, the
// state machine is broken — but our job here is to verify the state machine itself works correctly.

#include <doctest/doctest.h>

#include "luth/jobs/JobSystem.h"
#include "support/JobSystemFixture.h"

#include <atomic>

namespace
{
    struct V3Data
    {
        std::atomic<bool> outsideOk{false};
        std::atomic<bool> insideOk{false};
        std::atomic<bool> afterScopeOk{false};
    };

    void V3Job(Luth::JobSystem::JobArgs args)
    {
        auto* d = static_cast<V3Data*>(args.data);
        Luth::JobSystem::JobContext* ctx = Luth::JobSystem::GetCurrentJobContext();
        REQUIRE(ctx != nullptr);

        d->outsideOk.store(!ctx->IsRecording);
        {
            Luth::JobSystem::RecordingScope scope(ctx);
            d->insideOk.store(ctx->IsRecording);
        }
        d->afterScopeOk.store(!ctx->IsRecording);
    }
}

TEST_CASE_FIXTURE(LuthTests::JobSystemFixture, "V3 RecordingScope predicate transitions [smoke]")
{
    V3Data data;
    Luth::JobSystem::Counter c;
    Luth::JobSystem::Execute(V3Job, &data, &c, "V3Record", Luth::JobSystem::Priority::Normal);
    Luth::JobSystem::WaitForCounter(&c, 0);

    CHECK(data.outsideOk.load());
    CHECK(data.insideOk.load());
    CHECK(data.afterScopeOk.load());
}
