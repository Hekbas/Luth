#include "luthpch.h"
#include "luth/memory/MemoryTracker.h"
#include "luth/core/diagnostics/Log.h"

namespace Luth::Memory
{
    void MemoryTracker::Init()
    {
        // Zero all counters (static inline already zero-initialized, but explicit for re-init)
        for (u8 i = 0; i < static_cast<u8>(Category::Count); ++i)
        {
            s_Stats[i].CurrentBytes.store(0, std::memory_order_relaxed);
            s_Stats[i].PeakBytes.store(0, std::memory_order_relaxed);
            s_Stats[i].TotalAllocated.store(0, std::memory_order_relaxed);
            s_Stats[i].AllocCount.store(0, std::memory_order_relaxed);
            s_Stats[i].FreeCount.store(0, std::memory_order_relaxed);
        }
        s_TotalCurrent.store(0, std::memory_order_relaxed);
        s_TotalPeak.store(0, std::memory_order_relaxed);

        LH_CORE_INFO("MemoryTracker initialized");
    }

    void MemoryTracker::Shutdown()
    {
        LH_CORE_INFO("=== Memory Tracker Shutdown Summary ===");

        bool leaksDetected = false;
        for (u8 i = 0; i < static_cast<u8>(Category::Count); ++i)
        {
            i64 current = s_Stats[i].CurrentBytes.load(std::memory_order_relaxed);
            u32 allocs  = s_Stats[i].AllocCount.load(std::memory_order_relaxed);
            u32 frees   = s_Stats[i].FreeCount.load(std::memory_order_relaxed);
            i64 peak    = s_Stats[i].PeakBytes.load(std::memory_order_relaxed);

            if (current != 0)
            {
                leaksDetected = true;
                LH_CORE_WARN("  LEAK [{0}]: {1} bytes remaining ({2} allocs, {3} frees, peak {4} bytes)",
                    GetCategoryName(static_cast<Category>(i)), current, allocs, frees, peak);
            }
            else if (allocs > 0)
            {
                LH_CORE_INFO("  [{0}]: balanced ({1} allocs, {2} frees, peak {3} bytes)",
                    GetCategoryName(static_cast<Category>(i)), allocs, frees, peak);
            }
        }

        i64 totalCurrent = s_TotalCurrent.load(std::memory_order_relaxed);
        i64 totalPeak    = s_TotalPeak.load(std::memory_order_relaxed);

        if (leaksDetected)
            LH_CORE_WARN("  TOTAL: {0} bytes leaked (peak {1} bytes)", totalCurrent, totalPeak);
        else
            LH_CORE_INFO("  TOTAL: No leaks detected (peak {0} bytes)", totalPeak);

        LH_CORE_INFO("=======================================");
    }

    void MemoryTracker::RecordAlloc(Category cat, u64 size)
    {
        auto& s = s_Stats[static_cast<u8>(cat)];

        i64 newVal = s.CurrentBytes.fetch_add(static_cast<i64>(size), std::memory_order_relaxed) + static_cast<i64>(size);
        s.TotalAllocated.fetch_add(size, std::memory_order_relaxed);
        s.AllocCount.fetch_add(1, std::memory_order_relaxed);

        // Update per-category peak (CAS loop)
        i64 peak = s.PeakBytes.load(std::memory_order_relaxed);
        while (newVal > peak)
        {
            if (s.PeakBytes.compare_exchange_weak(peak, newVal, std::memory_order_relaxed))
                break;
        }

        // Update global total
        i64 globalNew = s_TotalCurrent.fetch_add(static_cast<i64>(size), std::memory_order_relaxed) + static_cast<i64>(size);
        i64 globalPeak = s_TotalPeak.load(std::memory_order_relaxed);
        while (globalNew > globalPeak)
        {
            if (s_TotalPeak.compare_exchange_weak(globalPeak, globalNew, std::memory_order_relaxed))
                break;
        }
    }

    void MemoryTracker::RecordFree(Category cat, u64 size)
    {
        auto& s = s_Stats[static_cast<u8>(cat)];

        s.CurrentBytes.fetch_sub(static_cast<i64>(size), std::memory_order_relaxed);
        s.FreeCount.fetch_add(1, std::memory_order_relaxed);

        s_TotalCurrent.fetch_sub(static_cast<i64>(size), std::memory_order_relaxed);
    }

    MemoryTracker::Snapshot MemoryTracker::GetSnapshot()
    {
        Snapshot snap{};
        for (u8 i = 0; i < static_cast<u8>(Category::Count); ++i)
        {
            snap.Categories[i].Current = s_Stats[i].CurrentBytes.load(std::memory_order_relaxed);
            snap.Categories[i].Peak    = s_Stats[i].PeakBytes.load(std::memory_order_relaxed);
            snap.Categories[i].Total   = s_Stats[i].TotalAllocated.load(std::memory_order_relaxed);
            snap.Categories[i].Allocs  = s_Stats[i].AllocCount.load(std::memory_order_relaxed);
            snap.Categories[i].Frees   = s_Stats[i].FreeCount.load(std::memory_order_relaxed);
        }
        snap.TotalCurrent = s_TotalCurrent.load(std::memory_order_relaxed);
        snap.TotalPeak    = s_TotalPeak.load(std::memory_order_relaxed);
        return snap;
    }

    const char* MemoryTracker::GetCategoryName(Category cat)
    {
        switch (cat)
        {
            case Category::General:     return "General";
            case Category::Rendering:   return "Rendering";
            case Category::Scene:       return "Scene";
            case Category::Jobs:        return "Jobs";
            case Category::Resources:   return "Resources";
            case Category::Editor:      return "Editor";
            case Category::FrameLinear: return "Frame Linear";
            case Category::FrameTagged: return "Frame Tagged";
            case Category::GPU:         return "GPU";
            default:                    return "Unknown";
        }
    }
}
