#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"

#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class FrameTargets;
    class RenderPipeline;
    struct ViewResources;

    // Owns the 3 GTAO compute layouts/pipelines/SPVs + linear-clamp sampler.
    // Per-frame: rebinds Set 0 binding 5 + GTAO main set binding 2 to the same
    // tagged-heap region in one batched write — both must stay atomic.
    class GTAOSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Per-render-stage rebind of the GTAO settings UBO.
        void UpdateUBO();

        // Stable per-view writes (sceneDepth/linDepth/rawAO/finalAO image bindings).
        // The UBO at GTAO-main binding 2 is rewritten by UpdateUBO.
        void WriteView(ViewResources& vr, FrameTargets& targets);

        // Render-graph contributions (compute passes).
        RG::ResourceHandle AddPrefilterPass(RG::RenderGraph& rg, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddMainPass(RG::RenderGraph& rg, RG::ResourceHandle linearDepth);
        RG::ResourceHandle AddDenoisePass(RG::RenderGraph& rg, RG::ResourceHandle rawAO, RG::ResourceHandle linearDepth);

        VkSampler             GetSampler()        const { return m_Sampler; }
        VkDescriptorSetLayout GetPrefilterLayout()const { return m_PrefilterDescLayout; }
        VkDescriptorSetLayout GetMainLayout()     const { return m_MainDescLayout; }
        VkDescriptorSetLayout GetDenoiseLayout()  const { return m_DenoiseDescLayout; }

    private:
        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_PrefilterPipeline;
        std::unique_ptr<VKComputePipeline> m_MainPipeline;
        std::unique_ptr<VKComputePipeline> m_DenoisePipeline;

        VkSampler             m_Sampler             = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_PrefilterDescLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_MainDescLayout      = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_DenoiseDescLayout   = VK_NULL_HANDLE;

        std::vector<u32> m_PrefilterSpv;
        std::vector<u32> m_MainSpv;
        std::vector<u32> m_DenoiseSpv;
    };
}
