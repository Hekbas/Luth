#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/jobs/SpinLock.h"
#include <vector>

namespace Luth::Memory
{
    // Tagged Page Allocator — 2 MB pages (VirtualAlloc) for frame-lifetime allocations.
    // Tag-based bulk free (FreeTag) releases all pages for a frame at once.
    // Thread-safe via per-thread ThreadCache. See docs/development/arch/memory.md.

    class TaggedPageAllocator
    {
    public:
        static constexpr u64 PAGE_SIZE = 2 * 1024 * 1024; // 2MB

        struct Page
        {
            void* Base = nullptr;
            u64 Used = 0;
            u32 Tag = 0;
            Page* Next = nullptr;
        };

        // Per-fiber cache (lives on JobContext, not TLS — see arch/fiber-system.md).
        struct ThreadCache
        {
            Page* ActivePage = nullptr;
            u32 CurrentTag = 0;
        };

        TaggedPageAllocator();
        ~TaggedPageAllocator();

        // Singleton — App owns lifecycle (paired with MemoryTracker).
        static TaggedPageAllocator& Get();

        void Init();
        void Shutdown();

        // Allocate from cache.ActivePage; claims a new page on overflow. Hot path holds no lock.
        void* Allocate(ThreadCache& cache, u64 size, u64 alignment = 8);

        // Bulk-release all pages tagged `tag`. Driven from VulkanBackend::AcquireImage
        // after the GPU N-2 timeline wait completes (V6 — see arch/fiber-system.md).
        void FreeTag(u32 tag);

    private:
        Page* AllocatePage(u32 tag);
        void ReturnPage(Page* page);

        // Held only on page-claim and FreeTag — < 100 cycles, V1-compliant.
        Luth::SpinLock m_Lock;
        std::vector<Page*> m_FreePages;
        std::vector<Page*> m_UsedPages; // ~10-20 pages per frame; linear scan in FreeTag is fine
    };
}
