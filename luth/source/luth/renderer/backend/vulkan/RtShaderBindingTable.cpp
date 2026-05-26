#include "luthpch.h"
#include "RtShaderBindingTable.h"
#include "VulkanRayTracingPipeline.h"
#include "VulkanContext.h"
#include "VulkanAllocator.h"
#include "luth/core/diagnostics/Log.h"

#include <vma/vk_mem_alloc.h>
#include <cstring>

namespace Luth
{
    namespace
    {
        // Round size up to the nearest multiple of alignment. Both must be > 0;
        // alignment is power-of-two on every known driver but the math doesn't require it.
        inline u32 AlignUp(u32 size, u32 align) { return (size + align - 1) & ~(align - 1); }
    }

    RtShaderBindingTable::RtShaderBindingTable(const VKRayTracingPipeline& pipeline, const RtSbtCounts& counts)
    {
        auto& ctx = VulkanContext::Get();

        const u32 totalGroups = counts.raygenCount + counts.missCount + counts.hitCount + counts.callableCount;
        if (totalGroups != pipeline.GetGroupCount())
        {
            LH_CORE_CRITICAL("RtShaderBindingTable: group-count mismatch (counts sum={}, pipeline={})",
                             totalGroups, pipeline.GetGroupCount());
            return;
        }
        if (counts.raygenCount < 1)
        {
            LH_CORE_CRITICAL("RtShaderBindingTable: raygenCount must be >= 1");
            return;
        }

        const auto& props = ctx.GetRtPipelineProperties();
        const u32 handleSize       = props.shaderGroupHandleSize;
        const u32 handleAlign      = props.shaderGroupHandleAlignment;
        const u32 baseAlign        = props.shaderGroupBaseAlignment;
        const u32 alignedHandleSz  = AlignUp(handleSize, handleAlign); // stride within a region

        // Per-region size = stride × count, then aligned to baseAlignment so the next region starts clean.
        const VkDeviceSize raygenSize   = AlignUp((u32)(alignedHandleSz * counts.raygenCount),   baseAlign);
        const VkDeviceSize missSize     = AlignUp((u32)(alignedHandleSz * counts.missCount),     baseAlign);
        const VkDeviceSize hitSize      = AlignUp((u32)(alignedHandleSz * counts.hitCount),      baseAlign);
        const VkDeviceSize callableSize = AlignUp((u32)(alignedHandleSz * counts.callableCount), baseAlign);
        const VkDeviceSize totalSize    = raygenSize + missSize + hitSize + callableSize;

        // Persistently mapped HOST_VISIBLE buffer — small (a few hundred bytes at most for typical
        // pipelines) and rebuilt only with the pipeline. Staging copy would add complexity without
        // perf gain at this size.
        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size  = totalSize;
        bi.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR
                 | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                 | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        void* mapped = nullptr;
        m_Allocation = VulkanAllocator::AllocateMappedSequentialBuffer(bi, m_Buffer, &mapped);
        if (!m_Allocation || !mapped)
        {
            LH_CORE_CRITICAL("RtShaderBindingTable: AllocateMappedSequentialBuffer failed (size={})", totalSize);
            return;
        }

        // Copy group handles into the mapped buffer with handle-alignment stride.
        // Pipeline group handles are tightly packed (handleSize × groupCount); copy each
        // handle to its strided slot in the SBT.
        const u8* src     = pipeline.GetGroupHandles().data();
        u8*       dst     = static_cast<u8*>(mapped);
        VkDeviceSize regionOffset = 0;
        u32 srcGroupIndex = 0;

        auto copyRegion = [&](u32 groupCount, VkDeviceSize regionSize) {
            u8* base = dst + regionOffset;
            for (u32 g = 0; g < groupCount; ++g)
            {
                std::memcpy(base + (size_t)g * alignedHandleSz,
                            src  + (size_t)(srcGroupIndex + g) * handleSize,
                            handleSize);
            }
            regionOffset  += regionSize;
            srcGroupIndex += groupCount;
        };
        copyRegion(counts.raygenCount,   raygenSize);
        copyRegion(counts.missCount,     missSize);
        copyRegion(counts.hitCount,      hitSize);
        copyRegion(counts.callableCount, callableSize);

        VulkanAllocator::FlushSlice(m_Allocation, 0, totalSize);

        // Cache device address + per-region stride structs ready for vkCmdTraceRaysKHR.
        VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        addrInfo.buffer = m_Buffer;
        const VkDeviceAddress base = vkGetBufferDeviceAddress(ctx.GetDevice(), &addrInfo);

        VkDeviceSize off = 0;
        if (counts.raygenCount > 0) {
            // Spec: raygen region's size must equal its stride (only one raygen invoked per traceRays).
            m_Raygen.deviceAddress = base + off;
            m_Raygen.stride        = alignedHandleSz;
            m_Raygen.size          = alignedHandleSz;
        }
        off += raygenSize;
        if (counts.missCount > 0) {
            m_Miss.deviceAddress = base + off;
            m_Miss.stride        = alignedHandleSz;
            m_Miss.size          = alignedHandleSz * counts.missCount;
        }
        off += missSize;
        if (counts.hitCount > 0) {
            m_Hit.deviceAddress = base + off;
            m_Hit.stride        = alignedHandleSz;
            m_Hit.size          = alignedHandleSz * counts.hitCount;
        }
        off += hitSize;
        if (counts.callableCount > 0) {
            m_Callable.deviceAddress = base + off;
            m_Callable.stride        = alignedHandleSz;
            m_Callable.size          = alignedHandleSz * counts.callableCount;
        }
    }

    RtShaderBindingTable::~RtShaderBindingTable()
    {
        if (!m_Buffer || !m_Allocation) return;
        VulkanContext::Get().PushDeletion([b = m_Buffer, a = m_Allocation]() {
            VulkanAllocator::FreeBuffer(b, a);
        });
    }
}
