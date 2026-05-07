#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/FrameData.h"

#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    // Per-frame VkQueryPools for GPU timestamp profiling. Results are read with a 2-frame latency
    // because GPU N-2 is the most recent frame guaranteed to be complete on the device side.
    class GPUTimerPool
    {
    public:
        void Init(u32 maxPasses);
        void Shutdown();

        // Reset the current frame's query pool. Call once per frame before any passes.
        void ResetForFrame(VkCommandBuffer cmd);

        // Write a timestamp before (isBegin=true) or after (isBegin=false) a pass. passIndex is a
        // 0-based counter of non-culled passes in this frame.
        void WriteTimestamp(VkCommandBuffer cmd, u32 passIndex, bool isBegin);

        // Read results from 2 frames ago. Fills outTimesMs with per-pass durations in milliseconds.
        void ReadResults(u32 passCount, std::vector<float>& outTimesMs);

    private:
        VkQueryPool m_Pools[MAX_FRAMES_IN_FLIGHT] = {};
        u32   m_MaxPasses      = 0;
        u64   m_FrameCounter   = 0;
        float m_TimestampPeriod = 0.0f;  // nanoseconds per tick
        bool  m_Initialized    = false;
    };
}
