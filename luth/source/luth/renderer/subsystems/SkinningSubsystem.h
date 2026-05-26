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

        // Per-frame dispatch loop — binds the compute pipeline + Set 0 (BoneMatrixBuffer SSBO) once,
        // then iterates snapshot.meshes filtering on isSkinned + non-null skinned BLAS, pushes the
        // per-mesh constants, and dispatches one workgroup-per-64-verts compute. Caller (RtSubsystem)
        // emits the compute-write → AS-build-read barrier on the deformed-VBs afterward.
        void DispatchAllSkinned(VkCommandBuffer cmd, const RenderSnapshot& snapshot) const;

    private:
        void Dispatch(VkCommandBuffer cmd, const Mesh& mesh, u32 boneOffset) const;

        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_ComputePipeline;
        std::vector<u32> m_Spv;
    };
}
