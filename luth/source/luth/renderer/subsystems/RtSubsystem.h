#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/backend/vulkan/TlasBuilder.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
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

    // Houses RT-domain state: per-frame TLAS rebuild + skinned-BLAS refit on AsyncCompute with
    // hash-based dirty skip and a multi-view guard, plus the sun-shadow surface:
    //   - Persistent empty TLAS at Init (Set 0 binding 6 never null; rt_sun_shadows.comp reads it
    //     statically and PARTIALLY_BOUND is conservatively interpreted by validation).
    //   - Sun-shadow pass: rt_sun_shadows.comp (rayQuery-in-compute) on VKComputePipeline, cutout
    //     alpha-tested via material_bindings_rt.slang (Set 3 Material + Set 4 bindless). No SBT/miss surface.
    //   - AddRtSunShadowsPass on AsyncCompute (writes per-view R8 shadow mask). Consumed by
    //     pbr.frag (Set 3 binding 4) when ShadowingMode::RtShadows is active.
    class RtSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Registers the AsyncCompute pass: SkinningSubsystem::DispatchAllSkinned ->
        // TlasBuilder::RefitSkinnedBLASes -> TlasBuilder::BuildTlas. Snapshot is read through
        // RenderingSystem::GetActiveSnapshot() inside the execute body.
        void AddTlasBuildPass(RG::RenderGraph& rg);

        // Registers the RT sun-shadow compute pass on AsyncCompute. Imports the per-view sunShadowMask
        // + reads SceneDepth + SlimNormal; the shader reads TLAS via static descriptor binding
        // (set 0 binding 6) so no RG declaration of TLAS is needed at this pass; the cross-pass barrier
        // (AS-build -> AS-read) is inline in the execute body, same pattern as the BLAS-refit ->
        // TLAS-build barrier in TlasBuildPass. Returns the imported shadow mask handle for downstream
        // Read(...) by GeometryPass. sceneDepth + slimNormal must be Read so RG transitions them to
        // SHADER_READ_ONLY_OPTIMAL before the dispatch samples them (descriptor write declared that layout).
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

        // Per-frame geometry-table BDA (restir_gi_initial.comp push constant). Built in lockstep with
        // the TLAS, so it pairs with whatever GetTlas() returns from the same m_LastResult. Zero before
        // the first real build (only the empty TLAS exists -> all rays miss -> table never deref'd).
        VkDeviceAddress GetGeometryTableBDA() const { return m_LastResult.geomTableBDA; }

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

        // Sun-shadow compute pipeline + per-pass descriptor layout. Pipeline layout is [Global, Light,
        // m_ShadowPassSetLayout, Material, bindless]: Sets 0/1/2 plus Set 3/4 for the cutout alpha-test.
        // The Light layout (PBR's Set 3) remaps to Set 1 per-pipeline-layout, not a global change; the same
        // VkDescriptorSet binds at different set indices for each pipeline.
        VkDescriptorSetLayout m_ShadowPassSetLayout = VK_NULL_HANDLE;
        VkSampler             m_ShadowPassSampler   = VK_NULL_HANDLE;  // linear clamp-to-edge for depth + normal

        std::unique_ptr<VKComputePipeline> m_SunShadowsPipeline;
        std::vector<u32> m_ShadowSpv;  // cached for hot-reload pipeline rebuild

        RenderPipeline* m_Pipeline      = nullptr;
        TlasBuildResult m_LastResult{};
        u64             m_BlasReadyGeneration = 0;  // ++ when a deferred BLAS first-builds; forces one TLAS rebuild (H1)
        u64             m_LastBuildFrame = ~u64(0);  // TLAS is scene-global; guard short-circuits the
                                                     // second view's rebuild. RT shadow trace is per-view
                                                     // (each view's depth/camera/mask differ) so no guard.
    };
}
