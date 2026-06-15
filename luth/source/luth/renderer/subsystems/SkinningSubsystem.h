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

    // Owns the per-vertex skinning compute pipeline that writes deformed vertices (pos/normal/tangent
    // + passthrough UV, interleaved Vertex layout) into each skinned mesh's persistent deformed buffer
    // — the SINGLE source both the raster vertex shaders (by gl_VertexIndex) and the RT BLAS refit +
    // geometry table read. Bone matrices come from BoneMatrixBuffer's SSBO at set 0; per-mesh
    // input/output addresses ride through push-constant BDAs so no per-mesh descriptor set is needed.
    //
    // AddDeformPass runs the dispatch as the FIRST graphics-queue pass each frame, so same-queue raster
    // geometry (gA) reads the deformed buffer after one hand-rolled barrier; cross-queue consumers
    // (refit + RT on async-compute, GeometryPass on gB) ride the gA→compute→gB timeline semaphores. The
    // RG buffer-read states have no vertex-shader variant, so the deform→raster dependency is a hand-
    // rolled global barrier, not builder.ReadBuffer. see arch/multi-queue.md
    class SkinningSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Adds the per-frame deform pass to the render graph as the FIRST (graphics-queue) pass so the
        // deformed buffers are ready before any raster geometry pass fetches them by gl_VertexIndex.
        // Runs the dispatch loop + one global compute-write→vertex/fragment/AS/compute-read barrier.
        // see arch/multi-queue.md
        void AddDeformPass(RG::RenderGraph& rg);

    private:
        // Dispatch loop — binds the compute pipeline + Set 0 (BoneMatrixBuffer SSBO) once, then iterates
        // snapshot.meshes filtering on isSkinned + non-null skinned BLAS, pushes per-mesh constants, and
        // dispatches one workgroup-per-64-verts. AddDeformPass emits the deformed-buffer barrier after.
        void DispatchAllSkinned(VkCommandBuffer cmd, const RenderSnapshot& snapshot) const;
        void Dispatch(VkCommandBuffer cmd, const Mesh& mesh, u32 boneOffset, u32 frameAbs) const;

        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_ComputePipeline;
        std::vector<u32> m_Spv;
    };
}
