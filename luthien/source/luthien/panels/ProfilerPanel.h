#pragma once

#include "luthien/Editor.h"
#include "luth/memory/MemoryTracker.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/jobs/JobSystem.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace Luth
{
    // Stat aggregation (frame-time history rotate, MemoryTracker snapshot, JobSystem stats,
    // GPU memory stats) all read globals and would be safe to move onto a worker fiber.
    struct ProfilerSnapshot { /* populated when stat gather moves off the editor thread */ };

    // One row of the GPU-pass hot-list. ms is EMA-smoothed so the sort order doesn't jitter per frame.
    struct GpuPassRow { std::string name; float ms; float overdraw; };

    class ProfilerPanel : public Panel
    {
    public:
        ProfilerPanel();
        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

    private:
        static const char* FormatBytes(i64 bytes, char* buf, size_t bufSize);

        // Pinned overview + CPU/Memory/GPU tab bodies (read the cached stats below).
        void DrawOverview();
        void DrawCpuTab();
        void DrawMemoryTab();
        void DrawGpuTab();

        std::vector<float> m_FrameTimeHistory;
        float m_UpdateTimer = 0.0f;
        int   m_Tab = 0;   // 0 CPU, 1 Memory, 2 GPU

        float m_FPS = 0.0f;
        float m_FrameTime = 0.0f;
        float m_FrameTimeMin = 0.0f;
        float m_FrameTimeMax = 0.0f;
        float m_FrameTimeAvg = 0.0f;
        float m_FrameTimeP95 = 0.0f;
        float m_FrameTimeP99 = 0.0f;
        float m_OnePercentLowFps = 0.0f;   // 1000 / p99 frame time

        int   m_TargetFPS = 60;
        float m_FrameBudgetMs = 16.67f;

        // CPU / scheduler — occupancy is the per-worker time-in-state fraction over the last window,
        // computed by diffing the engine's cumulative state-nanos against the previous 10 Hz snapshot.
        JobSystem::Stats m_JobStats{};
        u64   m_PrevStateNanos[JobSystem::MAX_WORKER_THREADS][4]{};
        float m_Occupancy[JobSystem::MAX_WORKER_THREADS][4]{};
        bool  m_HavePrevNanos = false;
        float m_QueuePeak = 0.0f;   // decaying max of the global high-queue depth (instantaneous reads ~0)
        float m_DequePeak = 0.0f;   // decaying max of the busiest worker deque

        // Memory
        Memory::MemoryTracker::Snapshot m_MemSnapshot{};
        std::vector<float> m_MemoryHistory;
        GPUMemoryStats m_GPUStats{};
        Memory::GPUTaggedPageAllocator::Stats m_GpuHeapStats{};
        u32   m_LastTrimCount = 0;
        float m_TrimFeedbackTimer = 0.0f;

        // GPU
        float m_GPUFrameTimeMs = 0.0f;
        u32   m_TriangleCount = 0;
        u32   m_DrawCalls = 0;
        bool  m_BarrierRedundantOnly = false;   // barrier inspector filter
        std::vector<GpuPassRow> m_PassRows;                 // EMA-smoothed, sorted (stable order)
        std::unordered_map<std::string, float> m_PassEma;   // per-pass smoothed ms, keyed by name
    };
}
