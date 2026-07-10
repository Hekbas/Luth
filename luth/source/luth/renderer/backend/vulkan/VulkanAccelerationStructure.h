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
    // For a DEFORMABLE BLAS (skinned or static wind-deformable) it additionally owns the persistent
    // "deformed vertex" buffer (per-frame compute output in the interleaved Vertex layout; AS-build
    // input on refit AND the RT geometry-table source, so ray hits read post-deform normals/tangents,
    // not bind pose). The deform compute (skinning.slang / deform.slang) reads the source VB directly.
    // Dtor pushes every owned VkBuffer + AS handle into VulkanContext::PushDeletion so they retire N+2 frames
    // out, safe against in-flight cmd buffers referencing the AS in a build / traceRays call.
    class VKAccelerationStructure
    {
    public:
        VKAccelerationStructure() = default;
        ~VKAccelerationStructure();

        VKAccelerationStructure(const VKAccelerationStructure&) = delete;
        VKAccelerationStructure& operator=(const VKAccelerationStructure&) = delete;

        VkAccelerationStructureKHR GetHandle()        const { return m_Handle; }
        VkDeviceAddress            GetDeviceAddress() const { return m_DeviceAddress; }
        bool                       IsDeformable()     const { return m_IsDeformable; }

        // Deformable-only: null/0 for a static (non-deformable) BLAS. The deformed buffer is double-buffered
        // (curr/prev regions of m_DeformedRegionBytes each) so raster motion vectors can read the previous
        // frame's positions; the region alternates by frame parity, with region 0 == CURR on frame 0 to match
        // the initial build at offset 0. The deform writes + AS-build/geom-table read CURR.
        VkDeviceAddress GetDeformedBdaCurr(u32 frameAbs) const
            { return m_DeformedBda + static_cast<VkDeviceAddress>(frameAbs & 1u) * m_DeformedRegionBytes; }
        VkDeviceAddress GetDeformedBdaPrev(u32 frameAbs) const
            { return m_DeformedBda + static_cast<VkDeviceAddress>(~frameAbs & 1u) * m_DeformedRegionBytes; }
        u32             GetVertexCount()     const { return m_VertexCount; }
        u64             GetUpdateScratchSize() const { return m_UpdateScratchSize; }

        // Deferred-build readiness. The AS object exists (valid device address) before its build is
        // recorded, so IsBuildRecorded() (not GetDeviceAddress) is the TLAS-inclusion predicate.
        bool IsBuildRecorded()     const { return m_BuildRecorded; }
        u32  GetBuildFrameAbs()    const { return m_BuildFrameAbs; }
        u64  GetBuildScratchSize() const { return m_BuildScratchSize; }
        // Marks a deferred BLAS built at `frameAbs`. Used by the batched RefitSkinnedBLASes first-build path
        // (which packs its own build info); RecordBuild sets these directly for the static-drain path.
        void MarkBuildRecorded(u32 frameAbs) { m_BuildRecorded = true; m_BuildFrameAbs = frameAbs; }

        // Per-mesh static BLAS factory. Synchronous main-thread ImmediateSubmit on the graphics queue (graphics
        // families always advertise VK_QUEUE_COMPUTE_BIT per spec, which is what vkCmdBuildAccelerationStructuresKHR
        // requires). PREFER_FAST_TRACE flag per NVIDIA RTX best practices: static BLAS optimizes for ray-trace
        // performance, build cost is paid once. Gates on UploadContext::WaitForUpload of the VB/IB upload fences
        // before recording the build; VB/IB upload runs on a separate submission chain (transfer queue), so the
        // graphics-queue draws' implicit serialize does NOT cover this build path.
        static std::shared_ptr<VKAccelerationStructure> CreateStaticBLAS(const Mesh& mesh);

        // Per-mesh DEFORMABLE BLAS factory (skinned OR static wind-deformable). Allocates a persistent
        // double-buffered deformed-positions buffer and builds the AS over its (zero-init) curr region with
        // ALLOW_UPDATE | PREFER_FAST_TRACE; the first per-frame deform compute + Refit fills the real positions
        // before any consumer reads the BLAS. The deform compute reads the mesh's source VB directly; this
        // waits on the VB + IB upload fences before the initial build.
        static std::shared_ptr<VKAccelerationStructure> CreateDeformableBLAS(const Mesh& mesh);

        // In-place refit (MODE_UPDATE_KHR). Requires the BLAS was originally built with
        // ALLOW_UPDATE_BIT_KHR + same primitiveCount/geometry layout/vertex format/index format.
        // The deformed positions buffer must already be populated by the deform compute on `cmd`
        // (or a prior submission); caller is responsible for the ComputeWrite -> AS-build barrier
        // (render graph emits this when both passes share resource handles).
        // scratchBda must be at least GetUpdateScratchSize() bytes, aligned to the
        // minAccelerationStructureScratchOffsetAlignment from VulkanContext::GetAsProperties().
        void Refit(VkCommandBuffer cmd, VkDeviceAddress scratchBda) const;

        // Records the initial MODE_BUILD onto `cmd`, reconstructed from the recipe captured at creation
        // (no Mesh needed, so a deferred pass can drive it). scratchBda must be >= GetBuildScratchSize()
        // and aligned to minAccelerationStructureScratchOffsetAlignment. Deformable reads its CURR
        // deformed region for `frameAbs`; static reads its fixed source VB. Sets IsBuildRecorded().
        void RecordBuild(VkCommandBuffer cmd, VkDeviceAddress scratchBda, u32 frameAbs);

        // Drains pending static BLAS builds onto `cmd` (the async-compute AS pass): records the MODE_BUILD
        // for each queued static BLAS whose VB/IB upload fence has retired (non-blocking poll), batching one
        // device-local scratch. A mesh evicted before its build cancels via weak_ptr expiry. Returns the
        // number of builds recorded (a null->ready transition the TLAS must fold in). see arch/rendering-pipeline.md
        static u32 DrainPendingStaticBuilds(VkCommandBuffer cmd, u32 frameAbs);

    private:
        VkAccelerationStructureKHR m_Handle          = VK_NULL_HANDLE;
        VkBuffer                   m_StorageBuffer   = VK_NULL_HANDLE;
        VmaAllocation              m_StorageAlloc    = nullptr;
        VkDeviceAddress            m_DeviceAddress   = 0;

        // Deformable-only.
        VkBuffer        m_DeformedBuffer   = VK_NULL_HANDLE;
        VmaAllocation   m_DeformedAlloc    = nullptr;
        VkDeviceAddress m_DeformedBda      = 0;
        VkDeviceSize    m_DeformedRegionBytes = 0;  // per-region size; buffer is 2x this (curr + prev)
        u32             m_VertexCount      = 0;
        u32             m_PrimitiveCount   = 0;
        u64             m_UpdateScratchSize = 0;
        bool            m_IsDeformable     = false;

        // Deferred initial build. The object is created up-front; RecordBuild replays this recipe onto a
        // command buffer later (the async-compute AS pass), so no Mesh / VB shared_ptr is retained.
        bool                                 m_BuildRecorded     = false;
        u32                                  m_BuildFrameAbs     = ~0u;   // frameAbs of RecordBuild; ~0u = built at load
        VkDeviceAddress                      m_BuildVbBda        = 0;     // static source VB (deformable uses CURR region)
        VkDeviceAddress                      m_BuildIbBda        = 0;
        u32                                  m_BuildVertexStride = 0;
        u32                                  m_BuildMaxVertex    = 0;
        u64                                  m_BuildScratchSize  = 0;     // MODE_BUILD scratch (aligned)
        VkGeometryFlagsKHR                   m_GeomFlags         = 0;
        VkBuildAccelerationStructureFlagsKHR m_BuildFlags        = 0;
    };
}
