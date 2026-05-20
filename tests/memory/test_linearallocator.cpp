// LinearAllocator — per-fiber bump allocator. Pages grow on overflow; Reset() rewinds to first page
// without freeing storage so pages get reused across frames. See docs/development/arch/memory.md.

#include <doctest/doctest.h>

#include "luth/memory/LinearAllocator.h"

#include <cstring>

using Luth::Memory::LinearAllocator;

TEST_CASE("LinearAllocator: basic Allocate returns 8-aligned pointer [smoke]")
{
    LinearAllocator alloc(4096);
    void* p = alloc.Allocate(64, 8);
    REQUIRE(p != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(p) & 7) == 0);
}

TEST_CASE("LinearAllocator: alignment requests honored [smoke]")
{
    LinearAllocator alloc(4096);
    // Force misalignment by allocating an odd-sized prefix
    alloc.Allocate(3, 1);
    for (size_t align : {16, 32, 64, 128})
    {
        void* p = alloc.Allocate(8, align);
        CHECK((reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0);
    }
}

TEST_CASE("LinearAllocator: oversized request grows page beyond default [smoke]")
{
    constexpr size_t kPage = 4096;
    LinearAllocator alloc(kPage);
    // Single allocation larger than the default page forces a custom-size page.
    constexpr size_t kBig = kPage * 4;
    void* p = alloc.Allocate(kBig, 8);
    REQUIRE(p != nullptr);

    // Direct functional check — writing the full requested size must not crash or trip ASan,
    // confirming the allocator returned a valid backing of at least kBig bytes.
    std::memset(p, 0xAB, kBig);

    // Accounting agrees: total grew to accommodate the oversize page.
    CHECK(alloc.GetTotalSize() >= kBig);
}

TEST_CASE("LinearAllocator: page growth on overflow [smoke]")
{
    constexpr size_t kPage = 4096;
    LinearAllocator alloc(kPage);
    const Luth::u64 initialTotal = alloc.GetTotalSize();

    // Fill the first page, then one more allocation triggers a second page.
    alloc.Allocate(kPage - 16, 8);
    void* p = alloc.Allocate(64, 8);
    REQUIRE(p != nullptr);
    CHECK(alloc.GetTotalSize() > initialTotal);
}

TEST_CASE("LinearAllocator: Reset rewinds without freeing pages [smoke]")
{
    constexpr size_t kPage = 4096;
    LinearAllocator alloc(kPage);
    // Allocate enough to force a second page.
    for (Luth::u32 i = 0; i < 200; ++i) alloc.Allocate(64, 8);
    const Luth::u64 totalAfterGrowth = alloc.GetTotalSize();
    REQUIRE(totalAfterGrowth >= kPage * 2);

    alloc.Reset();
    CHECK(alloc.GetUsedMemory() == 0);
    // Total size unchanged — pages retained for reuse.
    CHECK(alloc.GetTotalSize() == totalAfterGrowth);

    // Subsequent allocations reuse the retained pages.
    alloc.Allocate(64, 8);
    CHECK(alloc.GetTotalSize() == totalAfterGrowth);
}
