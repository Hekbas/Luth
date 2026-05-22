#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"

#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class FrameTargets;
    class RenderPipeline;
    struct ViewResources;
    struct SlimGBufferOutput;

    // Owns the PostProcess descriptor layout/sampler, the bloom extract +
    // bloom blur + tonemap-composite pipelines, and the per-frame PP UBO
    // upload (rebound to all 4 PP descriptor sets in one batched write).
    class PostProcessSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Per-render-stage rebind of the shared PostProcess UBO (binding 2 of all 4 PP sets).
        void UpdateUBO();

        // Stable per-view writes (sceneColor + bloom textures); UBO at binding 2 is rebound by UpdateUBO.
        void WriteView(ViewResources& vr, FrameTargets& targets);

        // Render-graph contributions.
        RG::ResourceHandle AddBloomPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);
        RG::ResourceHandle AddCompositePass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle bloomResult);
        // Live slim G-buffer viz — bypasses tonemap. Mode = SlimNormal/Roughness/Motion/MaterialID,
        // scale is motion magnification (unused for other modes). Runs after composite, writes LDR.
        // slimGB carries the producer-side RG handles from SlimGBufferPass; re-importing the same
        // VkImages would create aliased RG resources the barrier solver can't reconcile.
        RG::ResourceHandle AddSlimVizPass(RG::RenderGraph& rg, RG::ResourceHandle ldrInput,
                                          const SlimGBufferOutput& slimGB, u32 mode, float scale);

        VkDescriptorSetLayout GetDescSetLayout()        const { return m_DescSetLayout; }
        VkDescriptorSetLayout GetSlimVizDescSetLayout() const { return m_SlimVizDescSetLayout; }
        const std::vector<u32>& GetFullscreenVertSpv() const { return m_FullscreenVertSpv; }

    private:
        void BuildPipelines();

        RenderPipeline* m_Pipeline = nullptr;

        VkSampler             m_Sampler              = VK_NULL_HANDLE;
        VkSampler             m_NearestSampler       = VK_NULL_HANDLE; // for integer slim matID binding
        VkDescriptorSetLayout m_DescSetLayout        = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_SlimVizDescSetLayout = VK_NULL_HANDLE;

        std::unique_ptr<VKPipeline> m_BloomExtractPipeline;
        std::unique_ptr<VKPipeline> m_BloomBlurPipeline;
        std::unique_ptr<VKPipeline> m_PostProcessPipeline;
        std::unique_ptr<VKPipeline> m_SlimVizPipeline;

        std::vector<u32> m_FullscreenVertSpv;
        std::vector<u32> m_BloomExtractFragSpv;
        std::vector<u32> m_BloomBlurFragSpv;
        std::vector<u32> m_PostProcessFragSpv;
        std::vector<u32> m_SlimVizFragSpv;
    };
}
