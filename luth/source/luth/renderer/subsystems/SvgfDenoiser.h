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
    struct SvgfSettings;

    // Which signal this instance denoises. Selects the ViewResources image/descriptor set (svgf* /
    // svgfGi* / svgfSpec* / svgfDiSpec*) + the SvgfSettings instance + the RG/debug pass names. Di/Gi
    // denoise a demodulated diffuse irradiance; Reflections denoises the RT specular radiance (rt-renderer
    // D.1) via a SPECULAR reproject variant (svgf_spec_reproject.comp, hit-distance virtual reprojection,
    // b3 = slim roughness). DiSpecular (#154) denoises the ReSTIR-DI demodulated specular with the ordinary
    // MOTION reproject (svgf_reproject.comp, b3 = slim motion) — direct point-light specular is
    // surface-attached, not a reflection's virtual image. see arch/rendering-pipeline.md
    enum class DenoiserChannel { Di, Gi, Reflections, DiSpecular };

    // Custom SVGF (Schied 2017) diffuse denoiser. Consumes a ReSTIR pass's demodulated irradiance
    // (DI or GI per channel) and returns a denoised image the GeometryPass reads + the lighting set
    // binds. Disabled → a no-op pass-through (copies input → output) preserving the output contract;
    // reproject → moments → à-trous chain when enabled. see arch/rendering-pipeline.md
    class SvgfDenoiser : public IDenoiser
    {
    public:
        explicit SvgfDenoiser(DenoiserChannel channel = DenoiserChannel::Di) : m_Channel(channel) {}

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

        // Channel-selected SvgfSettings instance + RG/debug pass names (the ViewResources image/set
        // selection lives in a file-local Resolve() in the .cpp).
        const SvgfSettings& Settings() const;
        const char*         PassName(int which) const;  // 0=reproject 1=moments 2=atrous 3=passthrough

        DenoiserChannel m_Channel = DenoiserChannel::Di;
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
