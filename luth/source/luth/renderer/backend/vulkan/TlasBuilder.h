#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/types/LuthMath.h"
#include "luth/core/UUID.h"
#include "VulkanAllocator.h"

#include <span>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace Luth
{
    struct MeshDrawSnapshot;

    // Per-frame TLAS build helper. Hash-based dirty short-circuit (reuses last frame's handle when
    // the instance set is unchanged); the prior TLAS + its backing storage are kept alive across
    // frames in that case; caller defers PushDeletion until an actual rebuild replaces them.
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
        u64                        blasReadyGen  = 0;     // bumped when a deferred BLAS first-builds (H1)

        // Per-frame bindless geometry table, built in lockstep with the packed instances so
        // instanceCustomIndex indexes it. Host-visible SSBO, deref'd via BDA in restir_gi_initial.comp.
        // Shares the TLAS lifetime: kept alive across hash-skipped frames, freed on rebuild. see arch/rendering-pipeline.md
        VkBuffer                   geomTableBuffer = VK_NULL_HANDLE;
        VmaAllocation              geomTableAlloc  = nullptr;
        VkDeviceAddress            geomTableBDA    = 0;
    };

    class TlasBuilder
    {
    public:
        // Per-frame TLAS rebuild from a pre-captured snapshot.
        //   `cmd`        - open command buffer, queue supports compute.
        //   `instances`  - per-frame mesh draw snapshot (resolves Model->Mesh->BLAS internally).
        //   `frameAbs`   - absolute render-frame index (used for tagged-heap scratch tagging).
        //   `prev`       - last frame's result, returned unchanged on hash match.
        // Caller responsibilities:
        //   - On hash mismatch: PushDeletion(prev.tlas + prev.storageBuffer + prev.geomTableBuffer + allocs).
        //   - On hash match:    do NOT delete prev; the same handles are reused (Set 0 b6 + GI geom table).
        //   - The instance buffer used as build input is allocated here + PushDeletion-d immediately
        //     (retires N+2 frames out).
        // `materialSlotMap` resolves each instance's materialUUID -> Material-SSBO slot for the
        // geometry table (built in the same packed-instance loop). Defaults to slot 0 (white) on miss.
        // `blasReadyGen` is folded into the reuse guard so a BLAS that first-builds late (identical instance
        // hash) still forces one rebuild that gathers it, instead of staying skipped until the mesh moves.
        static TlasBuildResult BuildTlas(VkCommandBuffer cmd,
                                         std::span<const MeshDrawSnapshot> instances,
                                         u32 frameAbs,
                                         const TlasBuildResult& prev,
                                         const std::unordered_map<UUID, u32, UUIDHash>& materialSlotMap,
                                         u64 blasReadyGen);

        // Batched skinned-BLAS refit. Walks `instances` filtering for isSkinned + non-null skinned BLAS, packs one
        // VkAccelerationStructureBuildGeometryInfoKHR per mesh, all sharing a single tagged-heap scratch allocation
        // sliced into non-overlapping per-mesh sub-regions (per NVIDIA "all BLAS build calls need unique scratch"
        // rule), then issues ONE vkCmdBuildAccelerationStructuresKHR(N, infos, ranges) call. Caller must have
        // already populated each mesh's deformed-VB via the skinning compute pass + emitted the
        // compute-write -> AS-build-read memory barrier.
        // Per-entry mode: a deformable BLAS whose build has not been recorded yet gets a MODE_BUILD
        // (its first, over the deform's CURR region); the rest MODE_UPDATE. Gated on the source VB/IB
        // upload. Returns the number of first-builds recorded (a null->ready transition the TLAS folds in).
        static u32 RefitSkinnedBLASes(VkCommandBuffer cmd,
                                      std::span<const MeshDrawSnapshot> instances,
                                      u32 frameAbs);
    };
}
