#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/pipeline/PipelineManager.h"

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace Luth
{
    class RenderPipeline;
    struct ViewResources;

    // Owns the transparent tier — the pass slot after skybox + volumetric composite where
    // Transparent/Fade draws land (GeometryPass renders opaque + cutout only). Sorted mode:
    // per-view back-to-front indirect draws with pbr_transparent.frag (corrected inputs: rayQuery
    // sun shadow, cluster lights, fragment-depth froxel fog — never the opaque-coupled screen-space
    // buffers). OIT mode (PPLL store + resolve) layers on in the material-system arc's M.4 effort.
    // invariant: Init() before RenderPipeline's BuildPipelines block — Set 6 joins geoLayouts there.
    class TransparencySubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void BuildPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);
        void Shutdown();

        // Handles pbr_transparent.frag (returns true); pbr.vert / pbr_skinned.vert reloads
        // invalidate the cached variants but return false so GeometrySubsystem still owns them.
        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Set 6 b0 ← parity-picked resolved fog atlas (the volumetric composite's b1 rule).
        void WritePerFrame(ViewResources& vr, u32 frameAbs);

        // Set 6 b1/b2 (heads + nodes, all cycled slots) + the resolve set ← the view's OIT
        // resources. Called from AllocateViewResources + on resize/budget reallocation.
        void WriteOitView(ViewResources& vr);

        // Reserved Garlic tag range for per-view OIT node pools — disjoint from ReSTIR DI
        // (0xFFFF0000+) and GI (0xFFFF8000+); outside the per-frame FreeTag(N-2) sweep.
        u32 NextNodePoolTag() { return m_NextNodePoolTag++; }

        // Contributes the transparent pass(es) after the volumetric composite. sceneColor/entityID/
        // sceneDepth are GeometryPass-chain handles (same nodes — never re-imported); fogResolved is
        // the post-resolve atlas handle (invalid when volumetric is off → fog flag cleared).
        // Returns the sceneColor handle downstream passes consume.
        RG::ResourceHandle AddPasses(RG::RenderGraph& rg,
                                     RG::ResourceHandle sceneColor,
                                     RG::ResourceHandle entityID,
                                     RG::ResourceHandle sceneDepth,
                                     RG::ResourceHandle fogResolved,
                                     RG::BufferHandle indirectBufferHandle);

        VkDescriptorSetLayout GetSetLayout()        const { return m_TransparentSetLayout; }
        VkDescriptorSetLayout GetResolveSetLayout() const { return m_ResolveSetLayout; }

    private:
        RG::ResourceHandle AddSortedPass(RG::RenderGraph& rg,
                                         RG::ResourceHandle sceneColor,
                                         RG::ResourceHandle entityID,
                                         RG::ResourceHandle sceneDepth,
                                         RG::ResourceHandle fogResolved,
                                         RG::BufferHandle indirectBufferHandle);

        // Matches pbr_transparent_shading.glsl's push-constant block (16 B, FRAGMENT).
        struct TransparentPC
        {
            u64 geomTable    = 0;  // geometry-table BDA for the shadow ray's cutout alpha test
            u32 flags        = 0;  // bit0 = fog atlas valid this frame
            u32 nodeCapacity = 0;  // OIT store only
        };
        static_assert(sizeof(TransparentPC) == 16, "must match the shader push-constant block");

        RenderPipeline* m_Pipeline = nullptr;

        // Set 6 (transparent pass-local): b0 fog atlas sampler3D (UAB, parity rewrite), b1 OIT heads
        // storage image + b2 OIT nodes SSBO (UAB + partially-bound — written when the PPLL lands;
        // the sorted pipeline never statically uses them).
        VkDescriptorSetLayout m_TransparentSetLayout = VK_NULL_HANDLE;
        // OIT resolve pass-local (Set 1 of the fullscreen pipeline): b0 heads, b1 nodes.
        VkDescriptorSetLayout m_ResolveSetLayout = VK_NULL_HANDLE;

        PipelineManager  m_SortedPm;
        PipelineManager  m_SortedSkinnedPm;
        std::vector<u32> m_TransparentFragSpv;

        u32 m_NextNodePoolTag = 0xFFFFC000u;
    };
}
