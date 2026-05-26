#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/backend/vulkan/TlasBuilder.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"

#include <vulkan/vulkan.h>

namespace Luth
{
    class RenderPipeline;
    namespace RG { class RenderGraph; }

    // Houses RT-domain state across the rt-renderer arc. B.1 brought the no-op smoke test; B.2
    // brought per-frame TLAS rebuild + skinned-BLAS refit on AsyncCompute + hash-based dirty skip
    // + the multi-view guard. B.3 adds the persistent empty TLAS that backs Set 0 binding 6 from
    // engine boot before the first per-frame TlasBuildPass has run — once pbr.frag (via raygen)
    // statically reads binding 6, a null handle there is a VUID violation regardless of
    // PARTIALLY_BOUND. The empty TLAS is a 0-instance build: valid handle, all rayQuery rays miss.
    class RtSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        // Registers the AsyncCompute pass: SkinningSubsystem::DispatchAllSkinned →
        // TlasBuilder::RefitSkinnedBLASes → TlasBuilder::BuildTlas. Snapshot is read
        // through RenderingSystem::GetActiveSnapshot() inside the execute body.
        void AddTlasBuildPass(RG::RenderGraph& rg);

        // Returns per-frame TLAS once TlasBuildPass has populated it; otherwise the persistent
        // empty TLAS so Set 0 binding 6 is never null. Keeping them as separate fields prevents
        // the hash-skip PushDeletion in AddTlasBuildPass from accidentally destroying the
        // persistent handle when a per-frame TLAS first replaces it.
        VkAccelerationStructureKHR GetTlas() const
        {
            return m_LastResult.tlas != VK_NULL_HANDLE ? m_LastResult.tlas : m_PersistentEmptyTlas;
        }

    private:
        // Frame-0 binding-6 safety: built at Init, kept alive until Shutdown. The persistent
        // empty TLAS handle is seeded into m_LastResult.tlas so GlobalSubsystem::UpdateUBO's
        // existing `if (tlas != VK_NULL_HANDLE)` write always fires; per-frame TlasBuildPass
        // overwrites m_LastResult once the scene has meshes, but m_PersistentEmptyTlas + its
        // storage buffer stay alive (the deletion-queue retires per-frame TLAS handles fenced
        // on N+2; the persistent handle has no fence and outlives them all).
        VkAccelerationStructureKHR m_PersistentEmptyTlas      = VK_NULL_HANDLE;
        VkBuffer                   m_PersistentEmptyTlasBuf   = VK_NULL_HANDLE;
        VmaAllocation              m_PersistentEmptyTlasAlloc = nullptr;

        RenderPipeline* m_Pipeline      = nullptr;
        TlasBuildResult m_LastResult{};
        u64             m_LastBuildFrame = ~u64(0);  // multi-view guard sentinel
    };
}
