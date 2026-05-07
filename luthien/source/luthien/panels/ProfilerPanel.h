#pragma once

#include "luthien/Editor.h"
#include "luth/memory/MemoryTracker.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/jobs/JobSystem.h"
#include <vector>
#include <array>

namespace Luth
{
    // Real opportunity site: stat aggregation (frame-time history rotate, MemoryTracker
    // snapshot, JobSystem stats, GPU memory stats) all read globals and would be safe
    // on a worker fiber. The structural migration ships first; the stat-gather move
    // is future polish.
    struct ProfilerSnapshot { /* populated by future polish */ };

    class ProfilerPanel : public Panel
    {
    public:
        ProfilerPanel();
        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

    private:
        static const char* FormatBytes(i64 bytes, char* buf, size_t bufSize);

        std::vector<float> m_FrameTimeHistory;
        float m_UpdateTimer = 0.0f;

        float m_FPS = 0.0f;
        float m_FrameTime = 0.0f;
        float m_FrameTimeMin = 0.0f;
        float m_FrameTimeMax = 0.0f;
        float m_FrameTimeAvg = 0.0f;

        Memory::MemoryTracker::Snapshot m_MemSnapshot{};
        std::vector<float> m_MemoryHistory;

        GPUMemoryStats m_GPUStats{};
        Memory::GPUTaggedPageAllocator::Stats m_GpuHeapStats{};
        float m_GPUFrameTimeMs = 0.0f;

        float m_GameStageMs   = 0.0f;
        float m_RenderStageMs = 0.0f;

        int   m_TargetFPS = 60;
        float m_FrameBudgetMs = 16.67f;

        static constexpr u32 WORKER_HISTORY_FRAMES = 200;
        std::array<std::array<JobSystem::WorkerState, WORKER_HISTORY_FRAMES>, JobSystem::MAX_WORKER_THREADS> m_WorkerStateHistory{};
        u32 m_WorkerHistoryHead = 0;
        u32 m_WorkerThreadCount = 0;
        u32 m_CachedJobsExecuted = 0;
        u32 m_CachedStealSuccesses = 0;

        u32   m_LastTrimCount = 0;
        float m_TrimFeedbackTimer = 0.0f;
    };
}
