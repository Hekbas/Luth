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
        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_PassthroughPipeline;

        VkSampler             m_Sampler   = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_PassLayout = VK_NULL_HANDLE;  // passthrough set: b0 DI in, b1 output

        std::vector<u32> m_PassthroughSpv;
    };
}
