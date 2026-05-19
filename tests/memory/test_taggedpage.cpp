// TaggedPageAllocator — 2 MB pages from VirtualAlloc, pooled in m_FreePages.
// Each page carries a u32 tag; FreeTag(t) bulk-releases every page tagged t.
// Driven by VulkanBackend after the GPU N-2 timeline wait (V6). See
// docs/development/arch/memory.md.

#include <doctest/doctest.h>

#include "luth/memory/TaggedPageAllocator.h"

#include <atomic>
#include <thread>
#include <vector>

using Luth::Memory::TaggedPageAllocator;

namespace
{
    // Per-test RAII wrapper — TPA is a singleton, so this keeps lifecycle local.
    struct TpaScope
    {
        TpaScope()  { TaggedPageAllocator::Get().Init(); }
        ~TpaScope() { TaggedPageAllocator::Get().Shutdown(); }
    };
}

TEST_CASE("TaggedPageAllocator: basic Allocate returns aligned pointer [smoke]")
{
    TpaScope scope;
    auto& tpa = TaggedPageAllocator::Get();

    TaggedPageAllocator::ThreadCache cache;
    cache.CurrentTag = 1;
    void* p = tpa.Allocate(cache, 64, 16);
    REQUIRE(p != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(p) & 15) == 0);

    tpa.FreeTag(1);
}

TEST_CASE("TaggedPageAllocator: FreeTag returns pages to the free pool [smoke]")
{
    TpaScope scope;
    auto& tpa = TaggedPageAllocator::Get();

    TaggedPageAllocator::ThreadCache cache1;
    cache1.CurrentTag = 1;

    // First-tag alloc populates a new page from VirtualAlloc.
    void* p1 = tpa.Allocate(cache1, 64, 8);
    REQUIRE(p1 != nullptr);

    tpa.FreeTag(1);

    // After FreeTag the cache's ActivePage is stale; clear it so the next
    // alloc claims a fresh (or recycled) page from the pool.
    cache1.ActivePage = nullptr;
    cache1.CurrentTag = 2;

    void* p2 = tpa.Allocate(cache1, 64, 8);
    REQUIRE(p2 != nullptr);

    tpa.FreeTag(2);
}

TEST_CASE("TaggedPageAllocator: V6 overflow — never FreeTag, page count grows [stress]")
{
    TpaScope scope;
    auto& tpa = TaggedPageAllocator::Get();

    // V6: when the GPU stalls on N-2, FreeTag(N-2) never fires, so the allocator
    // must grow via repeated AllocatePage rather than reuse. We never call FreeTag
    // here; the allocator should claim fresh 2 MB pages each time the active
    // page fills. Cap at ~128 MB to avoid pressuring the dev machine.
    TaggedPageAllocator::ThreadCache cache;
    cache.CurrentTag = 99;

    constexpr Luth::u64 kBytesPerCall = 1 * 1024 * 1024; // 1 MB per allocation
    constexpr Luth::u32 kCalls = 128;                    // 128 MB total
    for (Luth::u32 i = 0; i < kCalls; ++i)
    {
        void* p = tpa.Allocate(cache, kBytesPerCall, 16);
        REQUIRE(p != nullptr);
    }

    // Cleanup: FreeTag releases everything we allocated.
    tpa.FreeTag(99);
}

TEST_CASE("TaggedPageAllocator: concurrent allocation across N threads [stress]")
{
    TpaScope scope;
    auto& tpa = TaggedPageAllocator::Get();

    constexpr Luth::u32 kThreads = 8;
    constexpr Luth::u32 kAllocsPerThread = 1000;

    std::atomic<Luth::u32> nullCount{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (Luth::u32 t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t]() {
            TaggedPageAllocator::ThreadCache cache;
            cache.CurrentTag = 100 + t;
            for (Luth::u32 i = 0; i < kAllocsPerThread; ++i)
            {
                void* p = tpa.Allocate(cache, 256, 8);
                if (!p) nullCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(nullCount.load() == 0);

    // FreeTag each thread's tag.
    for (Luth::u32 t = 0; t < kThreads; ++t)
    {
        tpa.FreeTag(100 + t);
    }
}
