#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/types/LuthMath.h"
#include "VulkanAllocator.h"

#include <span>
#include <vulkan/vulkan.h>

namespace Luth
{
    struct MeshDrawSnapshot;

    // Per-frame TLAS build helper. Hash-based dirty short-circuit (reuses last frame's handle when
    // the instance set is unchanged); the prior TLAS + its backing storage are kept alive across
    // frames in that case — caller defers PushDeletion until an actual rebuild replaces them.
    // Build runs on the caller's command buffer; queue must support VK_QUEUE_COMPUTE_BIT (graphics
    // family qualifies; AsyncCompute family qualifies on discrete GPUs).
    struct TlasBuildResult
    {
        VkAccelerationStructureKHR tlas          = VK_NULL_HANDLE;
        VkBuffer                   storageBuffer = VK_NULL_HANDLE;
        VmaAllocation              storageAlloc  = nullptr;
        u64                        instanceHash  = 0;
        u32                        instanceCount = 0;
        bool                       reused        = false; // true => prior result returned unchanged
    };

    class TlasBuilder
    {
    public:
        // Per-frame TLAS rebuild from a pre-captured snapshot.
        //   `cmd`        — open command buffer, queue supports compute.
        //   `instances`  — per-frame mesh draw snapshot (resolves Model→Mesh→BLAS internally).
        //   `frameAbs`   — absolute render-frame index (used for tagged-heap scratch tagging).
        //   `prev`       — last frame's result, returned unchanged on hash match.
        // Caller responsibilities:
        //   - On hash mismatch: PushDeletion(prev.tlas + prev.storageBuffer + prev.storageAlloc).
        //   - On hash match:    do NOT delete prev — the same handle is reused for Set 0 binding 6.
        //   - The instance buffer used as build input is allocated here + PushDeletion-d
        //     immediately (retires N+2 frames out).
        static TlasBuildResult BuildTlas(VkCommandBuffer cmd,
                                         std::span<const MeshDrawSnapshot> instances,
                                         u32 frameAbs,
                                         const TlasBuildResult& prev);

        // Batched skinned-BLAS refit. Walks `instances` filtering for isSkinned + non-null skinned
        // BLAS, packs one VkAccelerationStructureBuildGeometryInfoKHR per mesh, all sharing a
        // single tagged-heap scratch allocation sliced into non-overlapping per-mesh sub-regions
        // (per NVIDIA "all BLAS build calls need unique scratch" rule), then issues ONE
        // vkCmdBuildAccelerationStructuresKHR(N, infos, ranges) call. Caller must have already
        // populated each mesh's deformed-VB via the skinning compute pass + emitted the
        // compute-write → AS-build-read memory barrier.
        static void RefitSkinnedBLASes(VkCommandBuffer cmd,
                                       std::span<const MeshDrawSnapshot> instances,
                                       u32 frameAbs);
    };
}
