#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/FrameData.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <atomic>

namespace Luth
{
    // Per-frame VkQueryPools for GPU timestamp profiling. Results are read with a 2-frame latency
    // because GPU N-2 is the most recent frame guaranteed to be complete on the device side.
    // Also owns an optional pipeline-statistics pool (graphics passes only) — see the stats methods.
    class GPUTimerPool
    {
    public:
        // Values read back per pipeline-stats query, in ascending-bit order of the enabled flags.
        static constexpr u32 k_StatsValues = 6;
        // The graphics counters this pool records (ascending-bit order matches ReadStats' mapping).
        // A secondary cmd buffer spanned by a stats query must echo these in its inheritance info.
        static constexpr VkQueryPipelineStatisticFlags k_StatsFlags =
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT   |
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT      |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT       |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;

        void Init(u32 maxPasses);
        void Shutdown();

        // Reset the current frame's query pools. Call once per frame before any passes.
        void ResetForFrame(VkCommandBuffer cmd);

        // Write a timestamp before (isBegin=true) or after (isBegin=false) a pass. passIndex is a
        // 0-based counter of non-culled passes in this frame.
        void WriteTimestamp(VkCommandBuffer cmd, u32 passIndex, bool isBegin);

        // Read results from 2 frames ago. Fills outTimesMs with per-pass durations in milliseconds.
        void ReadResults(u32 passCount, std::vector<float>& outTimesMs);

        // ── Pipeline statistics (graphics passes only; async-compute queues can't run graphics stat
        // queries). Begin/End bracket the pass on the graphics primary and span its secondary via
        // inheritedQueries. Runtime-toggled + off by default; no-op when the device lacks support.
        bool StatsSupported() const { return m_StatsSupported; }
        void BeginStats(VkCommandBuffer cmd, u32 passIndex);
        void EndStats(VkCommandBuffer cmd, u32 passIndex);
        // Read 2-frame-old per-pass stats. MUST be called before ReadResults (shares the frame counter
        // ReadResults advances). Passes with no recorded query (compute, or stats off) get valid=false.
        void ReadStats(u32 passCount, std::vector<RG::GpuPipelineStats>& out);

        static void SetStatsEnabled(bool e) { s_StatsEnabled.store(e, std::memory_order_relaxed); }
        static bool StatsEnabled()          { return s_StatsEnabled.load(std::memory_order_relaxed); }

    private:
        VkQueryPool m_Pools[MAX_FRAMES_IN_FLIGHT] = {};
        VkQueryPool m_StatsPools[MAX_FRAMES_IN_FLIGHT] = {};
        u32   m_MaxPasses      = 0;
        u64   m_FrameCounter   = 0;
        float m_TimestampPeriod = 0.0f;
        bool  m_Initialized    = false;
        bool  m_StatsSupported = false;

        static inline std::atomic<bool> s_StatsEnabled{ false };
    };
}
