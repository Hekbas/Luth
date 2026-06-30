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
    struct WindSettings;
    namespace RG { class RenderGraph; }

    // Owns the per-frame deform pass: two compute pipelines that write post-deform vertices (pos/normal/
    // tangent + passthrough UV, interleaved Vertex layout) into each deformable mesh's persistent
    // deformed buffer — the SINGLE source both the raster vertex shaders (by gl_VertexIndex) and the RT
    // BLAS refit + geometry table read. skinning.slang deforms skinned meshes (bones at set 0);
    // deform.slang applies global wind to static wind-deformable meshes (no bones, no descriptor set).
    // Per-mesh input/output addresses ride through push-constant BDAs.
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

        // Static wind-deformable counterpart — deform.slang reads the Vertex VB + global wind instead of
        // skinning. Iterates isDeformable && !isSkinned; no bones, no descriptor set. Same deformed-buffer
        // output, so these meshes route through the same deformed pipelines + BLAS refit as skinned.
        void DispatchAllDeformable(VkCommandBuffer cmd, const RenderSnapshot& snapshot,
                                   const WindSettings& wind, f32 time) const;

        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_ComputePipeline;   // skinning.slang
        std::vector<u32> m_Spv;
        std::unique_ptr<VKComputePipeline> m_DeformPipeline;    // deform.slang (static wind-deformable)
        std::vector<u32> m_DeformSpv;
    };
}
