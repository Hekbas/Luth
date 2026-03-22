#include "luthpch.h"
#include "luth/renderer/backend/vulkan/GPUTimerPool.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"

namespace Luth
{
    void GPUTimerPool::Init(u32 maxPasses)
    {
        m_MaxPasses = maxPasses;

        auto& ctx = VulkanContext::Get();
        VkDevice device = ctx.GetDevice();
        m_TimestampPeriod = ctx.GetPhysicalDeviceProperties().limits.timestampPeriod;

        if (m_TimestampPeriod == 0.0f)
        {
            LH_CORE_WARN("GPU does not support timestamps (timestampPeriod == 0)");
            return;
        }

        VkQueryPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        poolInfo.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        poolInfo.queryCount = maxPasses * 2;  // begin + end per pass

        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            VkResult result = vkCreateQueryPool(device, &poolInfo, nullptr, &m_Pools[i]);
            LH_CORE_ASSERT(result == VK_SUCCESS, "Failed to create GPU timer query pool");
        }

        m_FrameCounter = 0;
        m_Initialized = true;
        LH_CORE_INFO("GPUTimerPool initialized: {} max passes, {:.2f} ns/tick", maxPasses, m_TimestampPeriod);
    }

    void GPUTimerPool::Shutdown()
    {
        if (!m_Initialized) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            if (m_Pools[i] != VK_NULL_HANDLE)
            {
                vkDestroyQueryPool(device, m_Pools[i], nullptr);
                m_Pools[i] = VK_NULL_HANDLE;
            }
        }
        m_Initialized = false;
    }

    void GPUTimerPool::ResetForFrame(VkCommandBuffer cmd)
    {
        if (!m_Initialized) return;

        u32 poolIndex = (u32)(m_FrameCounter % MAX_FRAMES_IN_FLIGHT);
        vkCmdResetQueryPool(cmd, m_Pools[poolIndex], 0, m_MaxPasses * 2);
    }

    void GPUTimerPool::WriteTimestamp(VkCommandBuffer cmd, u32 passIndex, bool isBegin)
    {
        if (!m_Initialized) return;
        if (passIndex >= m_MaxPasses) return;

        u32 poolIndex  = (u32)(m_FrameCounter % MAX_FRAMES_IN_FLIGHT);
        u32 queryIndex = passIndex * 2 + (isBegin ? 0 : 1);

        VkPipelineStageFlagBits2 stage = isBegin
            ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;

        vkCmdWriteTimestamp2(cmd, stage, m_Pools[poolIndex], queryIndex);
    }

    void GPUTimerPool::ReadResults(u32 passCount, std::vector<float>& outTimesMs)
    {
        outTimesMs.clear();

        if (!m_Initialized || m_FrameCounter < MAX_FRAMES_IN_FLIGHT)
        {
            // Not enough frames have elapsed to have completed results
            outTimesMs.resize(passCount, -1.0f);
            m_FrameCounter++;
            return;
        }

        // Read from the pool that was used 2 frames ago (GPU N-2 guaranteed complete)
        u32 readPoolIndex = (u32)((m_FrameCounter - 2) % MAX_FRAMES_IN_FLIGHT);
        u32 queryCount = passCount * 2;

        if (queryCount == 0 || passCount > m_MaxPasses)
        {
            outTimesMs.resize(passCount, -1.0f);
            m_FrameCounter++;
            return;
        }

        std::vector<u64> timestamps(queryCount);
        VkResult result = vkGetQueryPoolResults(
            VulkanContext::Get().GetDevice(),
            m_Pools[readPoolIndex],
            0, queryCount,
            queryCount * sizeof(u64),
            timestamps.data(),
            sizeof(u64),
            VK_QUERY_RESULT_64_BIT
        );

        outTimesMs.resize(passCount);
        if (result == VK_SUCCESS)
        {
            for (u32 i = 0; i < passCount; i++)
            {
                u64 begin = timestamps[i * 2];
                u64 end   = timestamps[i * 2 + 1];
                // Convert ticks to milliseconds: ticks * ns_per_tick / 1e6
                outTimesMs[i] = static_cast<float>((end - begin) * (double)m_TimestampPeriod / 1e6);
            }
        }
        else
        {
            // Results not ready or error — fill with -1
            for (u32 i = 0; i < passCount; i++)
                outTimesMs[i] = -1.0f;
        }

        m_FrameCounter++;
    }
}
