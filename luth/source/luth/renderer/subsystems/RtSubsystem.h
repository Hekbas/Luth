#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/backend/vulkan/TlasBuilder.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanRayTracingPipeline.h"
#include "luth/renderer/backend/vulkan/RtShaderBindingTable.h"
#include "luth/renderer/rendergraph/RenderGraphResources.h"

#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace Luth
{
    class RenderPipeline;
    class FrameTargets;
    struct ViewResources;
    namespace RG { class RenderGraph; }

    // Houses RT-domain state across the rt-renderer arc. B.1 brought the no-op smoke test; B.2
    // brought per-frame TLAS rebuild + skinned-BLAS refit on AsyncCompute + hash-based dirty skip
    // + the multi-view guard. B.3 adds:
    //   - Persistent empty TLAS at Init (Set 0 binding 6 never null — rt_sun_shadows.rgen reads
    //     it statically and PARTIALLY_BOUND is conservatively interpreted by validation).
    //   - Production RT shadow pipeline: rt_sun_shadows.rgen + rt_sun_shadows.rmiss, SBT with
    //     {raygen=1, miss=1, hit=0, callable=0}. First production exercise of B.1's
    //     VKRayTracingPipeline + RtShaderBindingTable primitives.
    //   - AddRtSunShadowsPass on AsyncCompute (writes per-view R8 shadow mask). Consumed by
    //     pbr.frag (Set 3 binding 4) when ShadowingMode::RtShadows is active.
    class RtSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Registers the AsyncCompute pass: SkinningSubsystem::DispatchAllSkinned →
        // TlasBuilder::RefitSkinnedBLASes → TlasBuilder::BuildTlas. Snapshot is read
        // through RenderingSystem::GetActiveSnapshot() inside the execute body.
        void AddTlasBuildPass(RG::RenderGraph& rg);

        // Registers the RT sun-shadow raygen pass on AsyncCompute. Imports the per-view
        // sunShadowMask + reads SceneDepth + SlimNormal; the raygen reads TLAS via static
        // descriptor binding (set 0 binding 6) so no RG declaration of TLAS is needed at this
        // pass — the cross-pass barrier (AS-build → AS-read) is inline in the execute body,
        // same pattern as the BLAS-refit → TLAS-build barrier in TlasBuildPass.
        // Returns the imported shadow mask handle for downstream Read(...) by GeometryPass.
        // sceneDepth + slimNormal must be Read so RG transitions them to SHADER_READ_ONLY_OPTIMAL
        // before the raygen samples them (descriptor write declared that layout).
        RG::ResourceHandle AddRtSunShadowsPass(RG::RenderGraph& rg,
                                               RG::ResourceHandle sceneDepth,
                                               RG::ResourceHandle slimNormal);

        // Per-view pass-local descriptor set writer. Binds:
        //   set 2 binding 0 = SceneDepth sampler (linear clamp)
        //   set 2 binding 1 = SlimNormal sampler (RG16F oct, linear clamp)
        //   set 2 binding 2 = sunShadowMask storage image (R8, GENERAL layout)
        // Called from ViewResources::AllocateViewResources + on resize via EnsureViewResources.
        void WriteShadowPassView(ViewResources& vr, FrameTargets& targets);

        VkDescriptorSetLayout GetShadowPassLayout() const { return m_ShadowPassSetLayout; }

        // Returns per-frame TLAS once TlasBuildPass has populated it; otherwise the persistent
        // empty TLAS so Set 0 binding 6 is never null. Keeping them as separate fields prevents
        // the hash-skip PushDeletion in AddTlasBuildPass from accidentally destroying the
        // persistent handle when a per-frame TLAS first replaces it.
        VkAccelerationStructureKHR GetTlas() const
        {
            return m_LastResult.tlas != VK_NULL_HANDLE ? m_LastResult.tlas : m_PersistentEmptyTlas;
        }

    private:
        void BuildShadowPipeline();

        // Frame-0 binding-6 safety: built at Init, kept alive until Shutdown. The persistent
        // empty TLAS handle is seeded into m_LastResult.tlas so GlobalSubsystem::UpdateUBO's
        // existing `if (tlas != VK_NULL_HANDLE)` write always fires; per-frame TlasBuildPass
        // overwrites m_LastResult once the scene has meshes, but m_PersistentEmptyTlas + its
        // storage buffer stay alive (the deletion-queue retires per-frame TLAS handles fenced
        // on N+2; the persistent handle has no fence and outlives them all).
        VkAccelerationStructureKHR m_PersistentEmptyTlas      = VK_NULL_HANDLE;
        VkBuffer                   m_PersistentEmptyTlasBuf   = VK_NULL_HANDLE;
        VmaAllocation              m_PersistentEmptyTlasAlloc = nullptr;

        // RT shadow pipeline + per-pass descriptor layout. Pipeline layout is
        // [GlobalSetLayout, LightSetLayout, m_ShadowPassSetLayout] — Set 0/1/2 in the RT pipeline.
        // The remapping of the existing Light layout from PBR's Set 3 to RT's Set 1 is per-pipeline-layout,
        // not a global change; the same VkDescriptorSet binds at different set indices for each pipeline.
        VkDescriptorSetLayout m_ShadowPassSetLayout = VK_NULL_HANDLE;
        VkSampler             m_ShadowPassSampler   = VK_NULL_HANDLE;  // linear clamp-to-edge for depth + normal

        std::unique_ptr<VKRayTracingPipeline> m_SunShadowsPipeline;
        std::unique_ptr<RtShaderBindingTable> m_SunShadowsSBT;
        std::vector<u32> m_RaygenSpv;  // cached for hot-reload pipeline rebuild
        std::vector<u32> m_MissSpv;

        RenderPipeline* m_Pipeline      = nullptr;
        TlasBuildResult m_LastResult{};
        u64             m_LastBuildFrame = ~u64(0);  // TLAS is scene-global — guard short-circuits the
                                                     // second view's rebuild. RT shadow trace is per-view
                                                     // (each view's depth/camera/mask differ) so no guard.
    };
}
