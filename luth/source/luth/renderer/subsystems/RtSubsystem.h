#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/backend/vulkan/TlasBuilder.h"

#include <vulkan/vulkan.h>

namespace Luth
{
    class RenderPipeline;
    namespace RG { class RenderGraph; }

    // Houses RT-domain state across the rt-renderer arc. Stub-only after Phase B.1; B.2 brings
    // per-frame TLAS rebuild + skinned-BLAS refit (AsyncCompute pass) + hash-based dirty skip
    // + the multi-view guard (Execute runs per RenderView; TLAS is scene-global so the second
    // view must short-circuit).
    class RtSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        // Registers the AsyncCompute pass: SkinningSubsystem::DispatchAllSkinned →
        // TlasBuilder::RefitSkinnedBLASes → TlasBuilder::BuildTlas. Snapshot is read
        // through RenderingSystem::GetActiveSnapshot() inside the execute body.
        void AddTlasBuildPass(RG::RenderGraph& rg);

        VkAccelerationStructureKHR GetTlas() const { return m_LastResult.tlas; }

    private:
        RenderPipeline* m_Pipeline      = nullptr;
        TlasBuildResult m_LastResult{};
        u64             m_LastBuildFrame = ~u64(0);  // multi-view guard sentinel
    };
}
