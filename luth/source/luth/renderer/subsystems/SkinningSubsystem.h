#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"

#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class Mesh;
    class RenderPipeline;
    struct RenderSnapshot;
    namespace RG { class RenderGraph; }

    // Owns the per-vertex skinning compute pipeline that writes deformed positions into each
    // skinned mesh's persistent deformed-positions buffer (the input to its BLAS refit). Bone
    // matrices come from BoneMatrixBuffer's existing SSBO at set 0; per-mesh input/output
    // addresses ride through push-constant BDAs so no per-mesh descriptor set is needed.
    //
    // The dispatch loop iterates RenderSnapshot::meshes filtering on isSkinned. Routed to
    // QueueFamily::AsyncCompute by the caller so it overlaps with the rest of the graphics
    // frame. Cross-queue handoff to the consumer (RtSubsystem's TlasBuildPass) is covered by
    // the per-submit timeline semaphore wait + RG's TOP_OF_PIPE substitution for cross-queue
    // barriers (see arch/multi-queue.md).
    class SkinningSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Registered into the render graph by RtSubsystem::AddTlasBuildPass (B.2.D wires the call;
        // B.2.C ships the subsystem dormant — Init creates the pipeline so build verifies it).
        void AddSkinningPass(RG::RenderGraph& rg, const RenderSnapshot& snapshot);

    private:
        void Dispatch(VkCommandBuffer cmd, const Mesh& mesh, u32 boneOffset) const;

        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_ComputePipeline;
        std::vector<u32> m_Spv;
    };
}
