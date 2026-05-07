#pragma once

#include "luth/core/types/LuthTypes.h"
#include <atomic>

namespace Luth::Memory
{
    // Runtime stats counter for engine-categorized allocations. Each Category holds a set of
    // lock-free atomic counters; the editor's memory panel reads coherent snapshots without
    // locking. invariant: STL and third-party heap traffic is intentionally NOT tracked here —
    // that coverage gap is described in arch/memory.md (the Tracy global new/delete hook closes
    // it for capture-time analysis).

    // ── Memory Category — tag each allocation with its subsystem ──

    enum class Category : u8
    {
        General = 0,    // Default / untagged
        Rendering,      // Vulkan wrappers, pipelines, buffers, textures (CPU side)
        Scene,          // ECS, entities, components
        Jobs,           // Fibers, deques, job data
        Resources,      // AssetManager, importers, loaded asset data
        Editor,         // ImGui, panels, editor-only
        FrameLinear,    // LinearAllocator page allocations
        FrameTagged,    // TaggedPageAllocator VirtualAlloc pages
        GPU,            // VMA allocations (GPU-resident)
        Count
    };

    // ── Per-category stats — lock-free atomics ──

    struct CategoryStats
    {
        std::atomic<i64> CurrentBytes{ 0 };   // Signed to detect underflow bugs
        std::atomic<i64> PeakBytes{ 0 };
        std::atomic<u64> TotalAllocated{ 0 };
        std::atomic<u32> AllocCount{ 0 };
        std::atomic<u32> FreeCount{ 0 };
    };

    // ── Memory Tracker — singleton with atomic counters ──

    class MemoryTracker
    {
    public:
        // Plain-data snapshot for UI reading (no atomics)
        struct Entry
        {
            i64 Current = 0;
            i64 Peak = 0;
            u64 Total = 0;
            u32 Allocs = 0;
            u32 Frees = 0;
        };

        struct Snapshot
        {
            Entry Categories[static_cast<u8>(Category::Count)]{};
            i64 TotalCurrent = 0;
            i64 TotalPeak = 0;
        };

        static void Init();
        static void Shutdown();

        static void RecordAlloc(Category cat, u64 size);
        static void RecordFree(Category cat, u64 size);

        static Snapshot GetSnapshot();
        static const char* GetCategoryName(Category cat);

    private:
        static inline CategoryStats s_Stats[static_cast<u8>(Category::Count)]{};
        static inline std::atomic<i64> s_TotalCurrent{ 0 };
        static inline std::atomic<i64> s_TotalPeak{ 0 };
    };
}
