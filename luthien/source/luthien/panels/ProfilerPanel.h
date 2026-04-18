#pragma once

#include "luthien/Editor.h"
#include "luth/memory/MemoryTracker.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/jobs/JobSystem.h"
#include <vector>
#include <array>

namespace Luth
{
    class ProfilerPanel : public Panel
    {
    public:
        ProfilerPanel();
        void OnInit() override;
        void OnRender() override;

    private:
        // Helper to format bytes as B/KB/MB
        static const char* FormatBytes(i64 bytes, char* buf, size_t bufSize);

        std::vector<float> m_FrameTimeHistory;
        float m_UpdateTimer = 0.0f;

        // Cached stats for display
        float m_FPS = 0.0f;
        float m_FrameTime = 0.0f;

        // Frame time min/max/avg over history buffer
        float m_FrameTimeMin = 0.0f;
        float m_FrameTimeMax = 0.0f;
        float m_FrameTimeAvg = 0.0f;

        // Memory tracking
        Memory::MemoryTracker::Snapshot m_MemSnapshot{};
        std::vector<float> m_MemoryHistory;  // Total MB over time

        // GPU stats cache
        GPUMemoryStats m_GPUStats{};
        float m_GPUFrameTimeMs = 0.0f;

        // Target FPS budget
        int   m_TargetFPS = 60;
        float m_FrameBudgetMs = 16.67f;

        // Worker timeline history (sampled every frame)
        static constexpr u32 WORKER_HISTORY_FRAMES = 200;
        std::array<std::array<JobSystem::WorkerState, WORKER_HISTORY_FRAMES>, JobSystem::MAX_WORKER_THREADS> m_WorkerStateHistory{};
        u32 m_WorkerHistoryHead = 0;
        u32 m_WorkerThreadCount = 0;
        u32 m_CachedJobsExecuted = 0;
        u32 m_CachedStealSuccesses = 0;

        // Trim feedback
        u32   m_LastTrimCount = 0;
        float m_TrimFeedbackTimer = 0.0f;
    };
}
