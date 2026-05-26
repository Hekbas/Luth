#pragma once

#include "luth/core/types/LuthTypes.h"
#include <vulkan/vulkan.h>

typedef struct VmaAllocation_T* VmaAllocation;

namespace Luth
{
    class VKRayTracingPipeline;

    // Per-region shader-group counts. Layout order in the SBT and in the pipeline's
    // group array is canonical: [raygen, ...miss, ...hit, ...callable]. Total must
    // equal pipeline.GetGroupCount(). At least one raygen group is required by spec;
    // empty miss/hit/callable regions are legal when the corresponding stage isn't
    // invoked by the pipeline (per VK_KHR_ray_tracing_pipeline).
    struct RtSbtCounts
    {
        u32 raygenCount   = 1;
        u32 missCount     = 0;
        u32 hitCount      = 0;
        u32 callableCount = 0;
    };

    // Persistent VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR buffer holding the
    // shader-group handles for one VKRayTracingPipeline. Lifetime mirrors the
    // pipeline — rebuild when the pipeline rebuilds; do not touch per-frame.
    // Per CLAUDE.md memory cornerstone: SBT is persistent → VKBuffer+VMA, not
    // GPUTaggedPageAllocator (which is for per-frame allocations).
    class RtShaderBindingTable
    {
    public:
        RtShaderBindingTable(const VKRayTracingPipeline& pipeline, const RtSbtCounts& counts);
        ~RtShaderBindingTable();

        RtShaderBindingTable(const RtShaderBindingTable&) = delete;
        RtShaderBindingTable& operator=(const RtShaderBindingTable&) = delete;

        const VkStridedDeviceAddressRegionKHR& GetRaygenRegion()   const { return m_Raygen; }
        const VkStridedDeviceAddressRegionKHR& GetMissRegion()     const { return m_Miss; }
        const VkStridedDeviceAddressRegionKHR& GetHitRegion()      const { return m_Hit; }
        const VkStridedDeviceAddressRegionKHR& GetCallableRegion() const { return m_Callable; }

        VkBuffer GetBuffer() const { return m_Buffer; }

    private:
        VkStridedDeviceAddressRegionKHR m_Raygen{};
        VkStridedDeviceAddressRegionKHR m_Miss{};
        VkStridedDeviceAddressRegionKHR m_Hit{};
        VkStridedDeviceAddressRegionKHR m_Callable{};
        VkBuffer      m_Buffer     = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
    };
}
