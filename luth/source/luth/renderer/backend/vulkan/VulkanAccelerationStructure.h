#pragma once

#include "luth/core/types/LuthTypes.h"
#include "VulkanAllocator.h"

#include <memory>
#include <span>
#include <vulkan/vulkan.h>

namespace Luth
{
    class Mesh;
    struct SkinnedVertex;

    // RAII wrapper for a single VkAccelerationStructureKHR + its persistent backing VkBuffer.
    // Used for both BLAS (per-mesh, owned by Mesh) and TLAS (per-frame, owned by RtSubsystem).
    // For skinned BLAS it additionally owns the per-mesh "skin input" buffer (tight-packed
    // position + bone IDs + weights, fed to the skinning compute) and the per-frame
    // "deformed positions" buffer (compute output, AS-build input on refit).
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

        // Skinned-only — null/0 for static BLAS.
        VkDeviceAddress GetSkinInputBda()    const { return m_SkinInputBda; }
        VkDeviceAddress GetDeformedBda()     const { return m_DeformedBda; }
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

        // Per-mesh skinned BLAS factory. Allocates a tight-packed skin-input buffer (pos + boneIDs
        // + weights per vertex) and a persistent deformed-positions buffer. Builds the AS over the
        // (zero-init) deformed buffer with ALLOW_UPDATE | PREFER_FAST_TRACE — the first per-frame
        // skinning compute + Refit fills the real positions before any consumer reads the BLAS.
        // skinnedVerts must be the same data already in the source VB (caller owns the lifetime).
        static std::shared_ptr<VKAccelerationStructure> CreateSkinnedBLAS(
            const Mesh& mesh,
            std::span<const SkinnedVertex> skinnedVerts);

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
        VkBuffer        m_SkinInputBuffer  = VK_NULL_HANDLE;
        VmaAllocation   m_SkinInputAlloc   = nullptr;
        VkDeviceAddress m_SkinInputBda     = 0;
        VkBuffer        m_DeformedBuffer   = VK_NULL_HANDLE;
        VmaAllocation   m_DeformedAlloc    = nullptr;
        VkDeviceAddress m_DeformedBda      = 0;
        u32             m_VertexCount      = 0;
        u32             m_PrimitiveCount   = 0;
        u64             m_UpdateScratchSize = 0;
        bool            m_IsSkinned        = false;
    };

    // Compute-side skin input. Matches GLSL `SkinInput` in skinning.comp (std430: vec4 pos at 0,
    // ivec4 boneIDs at 16, vec4 boneWeights at 32; total 48 bytes, 16-byte aligned). Position uses
    // vec4 to dodge the std430 vec3-padded-to-vec4 trap — .w is unused (set to 1.0 at extraction so
    // the shader can multiply boneMatrix * pos directly). Keep field order locked to GLSL.
    struct SkinComputeInput
    {
        Vec4  Position;    // .xyz = position, .w = 1.0
        IVec4 BoneIDs;
        Vec4  BoneWeights;
    };
    static_assert(sizeof(SkinComputeInput) == 48,
        "SkinComputeInput layout must stay locked to GLSL std430 layout in skinning.comp");
}
