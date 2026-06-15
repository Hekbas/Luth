#pragma once

#include "luth/core/types/LuthTypes.h"
#include "VulkanAllocator.h"

#include <memory>
#include <vulkan/vulkan.h>

namespace Luth
{
    class Mesh;

    // RAII wrapper for a single VkAccelerationStructureKHR + its persistent backing VkBuffer.
    // Used for both BLAS (per-mesh, owned by Mesh) and TLAS (per-frame, owned by RtSubsystem).
    // For skinned BLAS it additionally owns the persistent "deformed vertex" buffer (per-frame
    // compute output in the interleaved Vertex layout — AS-build input on refit AND the RT
    // geometry-table source, so ray hits read post-skin normals/tangents, not bind pose; the
    // skinning compute reads the source SkinnedVertex VB directly, no separate skin-input copy).
    // Dtor pushes every owned VkBuffer + AS handle into VulkanContext::PushDeletion so they
    // retire N+2 frames out — safe against in-flight cmd buffers referencing the AS in a
    // build / traceRays call.
    class VKAccelerationStructure
    {
    public:
        VKAccelerationStructure() = default;
        ~VKAccelerationStructure();

        VKAccelerationStructure(const VKAccelerationStructure&) = delete;
        VKAccelerationStructure& operator=(const VKAccelerationStructure&) = delete;

        VkAccelerationStructureKHR GetHandle()        const { return m_Handle; }
        VkDeviceAddress            GetDeviceAddress() const { return m_DeviceAddress; }
        bool                       IsSkinned()        const { return m_IsSkinned; }

        // Skinned-only — null/0 for static BLAS. The deformed buffer is double-buffered (curr/prev
        // regions of m_DeformedRegionBytes each) so raster motion vectors can read the previous frame's
        // positions; the region alternates by frame parity — region 0 == CURR on frame 0, matching the
        // initial build at offset 0. Skinning writes + AS-build/geom-table read the CURR region.
        VkDeviceAddress GetDeformedBdaCurr(u32 frameAbs) const
            { return m_DeformedBda + static_cast<VkDeviceAddress>(frameAbs & 1u) * m_DeformedRegionBytes; }
        VkDeviceAddress GetDeformedBdaPrev(u32 frameAbs) const
            { return m_DeformedBda + static_cast<VkDeviceAddress>(~frameAbs & 1u) * m_DeformedRegionBytes; }
        u32             GetVertexCount()     const { return m_VertexCount; }
        u64             GetUpdateScratchSize() const { return m_UpdateScratchSize; }

        // Per-mesh static BLAS factory. Synchronous main-thread ImmediateSubmit on the graphics
        // queue (graphics families always advertise VK_QUEUE_COMPUTE_BIT per spec, which is what
        // vkCmdBuildAccelerationStructuresKHR requires). PREFER_FAST_TRACE flag per NVIDIA RTX
        // best practices — static BLAS optimizes for ray-trace performance, build cost is paid once.
        // Gates on UploadContext::WaitForUpload of the VB/IB upload fences before recording the
        // build — VB/IB upload runs on a separate submission chain (transfer queue), so the
        // graphics-queue draws' implicit serialize does NOT cover this build path.
        static std::shared_ptr<VKAccelerationStructure> CreateStaticBLAS(const Mesh& mesh);

        // Per-mesh skinned BLAS factory. Allocates a persistent double-buffered deformed-positions
        // buffer and builds the AS over its (zero-init) curr region with ALLOW_UPDATE |
        // PREFER_FAST_TRACE — the first per-frame skinning compute + Refit fills the real positions
        // before any consumer reads the BLAS. The skinning compute reads the mesh's SkinnedVertex VB
        // directly; this waits on the VB + IB upload fences before the initial build.
        static std::shared_ptr<VKAccelerationStructure> CreateSkinnedBLAS(const Mesh& mesh);

        // In-place refit (MODE_UPDATE_KHR). Requires the BLAS was originally built with
        // ALLOW_UPDATE_BIT_KHR + same primitiveCount/geometry layout/vertex format/index format.
        // The deformed positions buffer must already be populated by the skinning compute on `cmd`
        // (or a prior submission); caller is responsible for the ComputeWrite → AS-build barrier
        // (render graph emits this when both passes share resource handles).
        // scratchBda must be at least GetUpdateScratchSize() bytes, aligned to the
        // minAccelerationStructureScratchOffsetAlignment from VulkanContext::GetAsProperties().
        void Refit(VkCommandBuffer cmd, VkDeviceAddress scratchBda) const;

    private:
        VkAccelerationStructureKHR m_Handle          = VK_NULL_HANDLE;
        VkBuffer                   m_StorageBuffer   = VK_NULL_HANDLE;
        VmaAllocation              m_StorageAlloc    = nullptr;
        VkDeviceAddress            m_DeviceAddress   = 0;

        // Skinned-only.
        VkBuffer        m_DeformedBuffer   = VK_NULL_HANDLE;
        VmaAllocation   m_DeformedAlloc    = nullptr;
        VkDeviceAddress m_DeformedBda      = 0;
        VkDeviceSize    m_DeformedRegionBytes = 0;  // per-region size; buffer is 2x this (curr + prev)
        u32             m_VertexCount      = 0;
        u32             m_PrimitiveCount   = 0;
        u64             m_UpdateScratchSize = 0;
        bool            m_IsSkinned        = false;
    };
}
