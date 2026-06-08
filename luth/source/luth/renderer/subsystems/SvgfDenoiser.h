#pragma once

#include "luth/renderer/subsystems/IDenoiser.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"

#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class FrameTargets;
    class RenderPipeline;
    struct ViewResources;

    // Custom SVGF (Schied 2017) diffuse denoiser. Consumes ReSTIR DI's demodulated irradiance and
    // returns a denoised image the GeometryPass reads + the lighting set binds. The first cut ships a
    // no-op pass-through (copies DI → output) that exercises the full wiring; reproject / variance /
    // à-trous land incrementally on top, keeping the output contract fixed. see arch/rendering-pipeline.md
    class SvgfDenoiser : public IDenoiser
    {
    public:
        void Init(RenderPipeline& pipeline) override;
        void Shutdown() override;
        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv) override;
        void AllocateViewSets(ViewResources& vr) override;
        void WriteView(ViewResources& vr, FrameTargets& targets) override;
        RG::ResourceHandle AddPasses(RG::RenderGraph& rg, const DenoiseInputs& in) override;
        bool IsEnabled() const override;

    private:
        // enabled + full pipeline → reproject → moments → à-trous ×N chain; disabled → raw copy (the
        // A/B). Both write svgfDenoised and return its handle; an invalid input handle returns invalid.
        RG::ResourceHandle AddDenoiseChain(RG::RenderGraph& rg, const DenoiseInputs& in);
        RG::ResourceHandle AddPassthroughPass(RG::RenderGraph& rg, const DenoiseInputs& in);

        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_PassthroughPipeline;
        std::unique_ptr<VKComputePipeline> m_ReprojectPipeline;
        std::unique_ptr<VKComputePipeline> m_MomentsPipeline;
        std::unique_ptr<VKComputePipeline> m_AtrousPipeline;

        VkSampler             m_Sampler        = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_PassLayout      = VK_NULL_HANDLE;  // passthrough set: b0 DI in, b1 out
        VkDescriptorSetLayout m_ReprojectLayout = VK_NULL_HANDLE;  // reproject set: b0-b3 in, b4-b9 history
        VkDescriptorSetLayout m_MomentsLayout   = VK_NULL_HANDLE;  // moments set: b0-b1 hist, b2-b3 samplers, b4 out
        VkDescriptorSetLayout m_AtrousLayout    = VK_NULL_HANDLE;  // à-trous set: b0 in, b1-b2 samplers, b3 out, b4 denoised

        std::vector<u32> m_PassthroughSpv;
        std::vector<u32> m_ReprojectSpv;
        std::vector<u32> m_MomentsSpv;
        std::vector<u32> m_AtrousSpv;
    };
}
