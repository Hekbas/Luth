#pragma once

#include "luth/core/LuthTypes.h"
#include <vector>
#include <mutex>
#include <atomic>

namespace Luth::Memory
{
    // ===================================================================================
    // Tagged Page Allocator
    // ===================================================================================
    // Allocates memory in 2MB pages (VirtualAlloc).
    // Designed for high-frequency, frame-lifetime allocations.
    // Supports "Tagging" allocations to free entire groups (e.g., Frame N) at once.
    // Thread-safe via per-thread caching.
    
    class TaggedPageAllocator
    {
    public:
        static constexpr u64 PAGE_SIZE = 2 * 1024 * 1024; // 2MB

        struct Page
        {
            void* Base = nullptr;
            u64 Used = 0;
            u32 Tag = 0;
            Page* Next = nullptr; // Linked list for free/used lists
        };

        // Per-thread cache to avoid global lock contention
        struct ThreadCache
        {
            Page* ActivePage = nullptr;
            u32 CurrentTag = 0;
        };

        TaggedPageAllocator();
        ~TaggedPageAllocator();

        void Init();
        void Shutdown();

        // Allocate memory from the current thread's active page.
        // If the page is full, it requests a new one from the global pool.
        void* Allocate(ThreadCache& cache, u64 size, u64 alignment = 8);

        // Free all pages associated with a specific tag.
        // This is typically called when the GPU finishes a frame.
        void FreeTag(u32 tag);

        // Helper to get the global instance (if singleton pattern is desired, though usually passed via JobContext)
        // static TaggedPageAllocator& Get(); 

    private:
        Page* AllocatePage(u32 tag);
        void ReturnPage(Page* page);

        std::mutex m_Lock;
        std::vector<Page*> m_FreePages; // Pool of available pages
        std::vector<Page*> m_UsedPages; // Pages currently in use (tracked for shutdown/debug)
        
        // We need a way to track pages by tag efficiently.
        // A simple vector of pages might be slow to search.
        // But FreeTag is relatively infrequent (once per frame per buffered frame).
        // Optimization: Linked list of pages per tag? Or just iterate m_UsedPages?
        // Given we have ~3 frames in flight and maybe 10-20 pages per frame, iteration is fine.
        // Actually, we can store pages in a "Tag Bucket" if needed.
        // For now, linear scan of m_UsedPages is O(N) where N is total active pages.
        // N is small (e.g., 100MB used = 50 pages). Scan is fast.
    };
}
