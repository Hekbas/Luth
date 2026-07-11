#include "luthpch.h"
#include "luth/renderer/subsystems/SvgfDenoiser.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/settings/SvgfSettings.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/core/FrameData.h"

namespace Luth
{
    namespace {
        // gbufferScale/dispatchW/dispatchH let a channel run at a working resolution below the full
        // G-buffer (half-res GI). scale==1 + dispatch==full is the identity path for full-res channels.
        // Mirrors svgf_reproject.slang's push_constant (7 floats + 3 ints = 40 B); the spec variant
        // shares the layout (confidenceScale unused there - its input alpha is hitDist).
        struct SvgfReprojectPC {
            f32 alphaColor;
            f32 alphaMoments;
            f32 historyCap;
            f32 depthThreshold;
            f32 normalThreshold;
            i32 gbufferScale;
            i32 dispatchW;
            i32 dispatchH;
            f32 antiFireflySigma;   // 3x3 mean + k*sigma clamp on the incoming sample; 0 = off
            f32 confidenceScale;    // reservoir-confidence history-cap shortening; 0 = off / spec variant
        };
        static_assert(sizeof(SvgfReprojectPC) == 40, "SvgfReprojectPC must match svgf_reproject.slang push_constant");

        // Mirrors svgf_moments.slang's push_constant (2 floats + 3 ints = 20 B).
        struct SvgfMomentsPC {
            f32 phiDepth;
            f32 phiNormal;
            i32 gbufferScale;
            i32 dispatchW;
            i32 dispatchH;
        };
        static_assert(sizeof(SvgfMomentsPC) == 20, "SvgfMomentsPC must match svgf_moments.slang push_constant");

        // Mirrors svgf_atrous.slang's push_constant (2 ints + 3 floats + 3 ints + 1 float = 36 B).
        struct SvgfAtrousPC {
            i32 stepSize;
            i32 writeFinal;
            f32 phiColor;
            f32 phiNormal;
            f32 phiDepth;
            i32 gbufferScale;
            i32 dispatchW;
            i32 dispatchH;
            f32 phiRough;   // roughness edge-stop; spec channels only, diffuse channels pass 0
        };
        static_assert(sizeof(SvgfAtrousPC) == 36, "SvgfAtrousPC must match svgf_atrous.slang push_constant");

        // Channel-selected pointers into ViewResources: DI uses the svgf* fields, GI the svgfGi*
        // (flat parallel set, mirroring the S0 restirDI/restirGiDI split). All array fields are
        // length-2; denoised/noisy are single. Resolved once per method; cheap pointer fixups.
        struct ChannelRefs {
            std::shared_ptr<Texture>* colorHist;   // [2]
            std::shared_ptr<Texture>* moments;     // [2]
            std::shared_ptr<Texture>* geom;        // [2]
            std::shared_ptr<Texture>* atrous;      // [2]
            std::shared_ptr<Texture>* denoised;    // single
            std::shared_ptr<Texture>* noisy;       // single: restirDI / restirGiDI (denoiser input)
            VkDescriptorSet*          passthroughSet;
            VkDescriptorSet*          reprojectSet; // [2]
            VkDescriptorSet*          momentsSet;   // [2]
            VkDescriptorSet*          atrousSet;    // [2]
        };
        ChannelRefs Resolve(DenoiserChannel ch, ViewResources& vr) {
            if (ch == DenoiserChannel::Reflections)
            {
                // Half-res reflections: a-trous final + passthrough write svgfSpecHalf (a bilateral upscale
                // resolves it into the full svgfSpecDenoised). Detect from the history extent vs the full
                // denoised image; mirrors the DiSpecular/Gi half detection, no setting plumbing.
                const bool specHalf = vr.svgfSpecHalf && vr.svgfSpecColorHist[0] && vr.svgfSpecDenoised
                    && std::static_pointer_cast<VKTexture>(vr.svgfSpecColorHist[0])->GetWidth()
                       < std::static_pointer_cast<VKTexture>(vr.svgfSpecDenoised)->GetWidth();
                return { vr.svgfSpecColorHist, vr.svgfSpecMoments, vr.svgfSpecGeom, vr.svgfSpecAtrous,
                         specHalf ? &vr.svgfSpecHalf : &vr.svgfSpecDenoised, &vr.reflRadiance,
                         &vr.svgfSpecPassthroughDescSet, vr.svgfSpecReprojectDescSet,
                         vr.svgfSpecMomentsDescSet, vr.svgfSpecAtrousDescSet };
            }
            if (ch == DenoiserChannel::DiSpecular)
            {
                const bool diSpecHalf = vr.svgfDiSpecHalf && vr.svgfDiSpecColorHist[0] && vr.svgfDiSpecDenoised
                    && std::static_pointer_cast<VKTexture>(vr.svgfDiSpecColorHist[0])->GetWidth()
                       < std::static_pointer_cast<VKTexture>(vr.svgfDiSpecDenoised)->GetWidth();
                return { vr.svgfDiSpecColorHist, vr.svgfDiSpecMoments, vr.svgfDiSpecGeom, vr.svgfDiSpecAtrous,
                         diSpecHalf ? &vr.svgfDiSpecHalf : &vr.svgfDiSpecDenoised, &vr.restirDISpec,
                         &vr.svgfDiSpecPassthroughDescSet, vr.svgfDiSpecReprojectDescSet,
                         vr.svgfDiSpecMomentsDescSet, vr.svgfDiSpecAtrousDescSet };
            }
            if (ch == DenoiserChannel::Gi)
            {
                // Half-res GI: the chain runs below full res, so the a-trous final + passthrough write the
                // half svgfGiHalf (a bilateral upscale resolves it into the full svgfGiDenoised). Detect
                // from the history extent vs the full-res denoised image; no setting plumbing needed.
                const bool giHalf = vr.svgfGiHalf && vr.svgfGiColorHist[0] && vr.svgfGiDenoised
                    && std::static_pointer_cast<VKTexture>(vr.svgfGiColorHist[0])->GetWidth()
                       < std::static_pointer_cast<VKTexture>(vr.svgfGiDenoised)->GetWidth();
                return { vr.svgfGiColorHist, vr.svgfGiMoments, vr.svgfGiGeom, vr.svgfGiAtrous,
                         giHalf ? &vr.svgfGiHalf : &vr.svgfGiDenoised, &vr.restirGiDI,
                         &vr.svgfGiPassthroughDescSet, vr.svgfGiReprojectDescSet,
                         vr.svgfGiMomentsDescSet, vr.svgfGiAtrousDescSet };
            }
            const bool diHalf = vr.svgfDiHalf && vr.svgfColorHist[0] && vr.svgfDenoised
                && std::static_pointer_cast<VKTexture>(vr.svgfColorHist[0])->GetWidth()
                   < std::static_pointer_cast<VKTexture>(vr.svgfDenoised)->GetWidth();
            return { vr.svgfColorHist, vr.svgfMoments, vr.svgfGeom, vr.svgfAtrous,
                     diHalf ? &vr.svgfDiHalf : &vr.svgfDenoised, &vr.restirDI,
                     &vr.svgfPassthroughDescSet, vr.svgfReprojectDescSet,
                     vr.svgfMomentsDescSet, vr.svgfAtrousDescSet };
        }
    }

    const SvgfSettings& SvgfDenoiser::Settings() const
    {
        auto& sys = m_Pipeline->GetSystem();
        if (m_Channel == DenoiserChannel::Reflections) return sys.GetSvgfSpecSettings();
        if (m_Channel == DenoiserChannel::DiSpecular)  return sys.GetSvgfDiSpecSettings();
        return m_Channel == DenoiserChannel::Gi ? sys.GetSvgfGiSettings() : sys.GetSvgfSettings();
    }

    const char* SvgfDenoiser::PassName(int which) const
    {
        static const char* di[] = { "SvgfReproject", "SvgfMoments", "SvgfAtrous", "SvgfPassthrough" };
        static const char* gi[] = { "SvgfGiReproject", "SvgfGiMoments", "SvgfGiAtrous", "SvgfGiPassthrough" };
        static const char* sp[] = { "SvgfSpecReproject", "SvgfSpecMoments", "SvgfSpecAtrous", "SvgfSpecPassthrough" };
        static const char* ds[] = { "SvgfDiSpecReproject", "SvgfDiSpecMoments", "SvgfDiSpecAtrous", "SvgfDiSpecPassthrough" };
        if (m_Channel == DenoiserChannel::Reflections) return sp[which];
        if (m_Channel == DenoiserChannel::DiSpecular)  return ds[which];
        return (m_Channel == DenoiserChannel::Gi ? gi : di)[which];
    }

    bool SvgfDenoiser::IsEnabled() const
    {
        return m_Pipeline && Settings().enabled;
    }

    void SvgfDenoiser::Init(RenderPipeline& pipeline)
    {
        LH_PROFILE_FUNCTION();
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear clamp-to-edge: matches the ReSTIR/RT pass samplers for SceneDepth/SlimNormal.
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_Sampler);

        // Passthrough set (pass-local): b0 demodulated-DI sampler, b1 output storage image.
        {
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 2; ci.pBindings = b;
            vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_PassLayout);
        }

        // Reproject set (pass-local): b0-b3 current-frame samplers (DI, depth, normal, motion);
        // b4-b6 prev history storage (color+variance, moments+histLen, geom); b7-b9 curr history
        // storage; b10 slim matID sampler (motion variant's material history gate; the spec variant's
        // shader leaves it undeclared). History stays GENERAL (storage), so no UAB / per-frame rewrite;
        // the two sets are pre-built per parity and bound by frameAbs & 1. The denoised output moved to
        // the a-trous final level, so the reproject no longer binds it.
        {
            VkDescriptorSetLayoutBinding b[11]{};
            for (u32 i = 0; i < 11; ++i)
            {
                b[i].binding         = i;
                b[i].descriptorCount = 1;
                b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
                b[i].descriptorType  = (i < 4 || i == 10) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                          : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            }
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 11; ci.pBindings = b;
            vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_ReprojectLayout);
        }

        // Moments set (pass-local): b0 colorHist[curr] storage, b1 moments[curr] storage, b2 depth
        // sampler, b3 normal sampler, b4 svgfAtrous[0] storage (a-trous level-0 input).
        {
            VkDescriptorSetLayoutBinding b[5]{};
            const VkDescriptorType types[5] = {
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            };
            for (u32 i = 0; i < 5; ++i)
            {
                b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                b[i].descriptorType = types[i];
            }
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 5; ci.pBindings = b;
            vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_MomentsLayout);
        }

        // A-trous set (pass-local): b0 svgfAtrous[IN] storage, b1 depth sampler, b2 normal sampler,
        // b3 svgfAtrous[OUT] storage, b4 svgfDenoised storage (final level), b5 slim roughness sampler
        // (spec channels' edge-stop; diffuse channels disable via phiRough 0). Two sets by iter parity.
        {
            VkDescriptorSetLayoutBinding b[6]{};
            const VkDescriptorType types[6] = {
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            };
            for (u32 i = 0; i < 6; ++i)
            {
                b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                b[i].descriptorType = types[i];
            }
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 6; ci.pBindings = b;
            vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_AtrousLayout);
        }

        // Reflections denoises a specular signal -> a SPECULAR reproject variant (hit-distance virtual
        // reprojection); same layout/pcRange as the diffuse reproject. Moments/a-trous/passthrough shared.
        const char* reprojShader = (m_Channel == DenoiserChannel::Reflections)
            ? "shaders/svgf_spec_reproject.slang" : "shaders/svgf_reproject.slang";
        if (auto sh = ShaderLibrary::LoadEngine("shaders/svgf_passthrough.slang"))
            m_PassthroughSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine(reprojShader))
            m_ReprojectSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/svgf_moments.slang"))
            m_MomentsSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/svgf_atrous.slang"))
            m_AtrousSpv = sh->GetSpirV();
        if (m_PassthroughSpv.empty() || m_ReprojectSpv.empty() || m_MomentsSpv.empty() || m_AtrousSpv.empty())
        {
            LH_LOG(Renderer, error, "SvgfDenoiser: failed to load svgf_passthrough/reproject/moments/atrous.comp SPIR-V");
            return;
        }

        const std::vector<VkDescriptorSetLayout> passLayouts = { m_PassLayout };
        m_PassthroughPipeline = std::make_unique<VKComputePipeline>(
            m_PassthroughSpv, passLayouts, std::vector<VkPushConstantRange>{});

        VkDescriptorSetLayout globalLayout = m_Pipeline->GetGlobal().GetSetLayout();

        // Reproject binds Set 0 (global UBO: nearZ/farZ/viewportSize) + the pass-local set.
        const std::vector<VkDescriptorSetLayout> reprojLayouts = { globalLayout, m_ReprojectLayout };
        VkPushConstantRange reprojPc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfReprojectPC) };
        m_ReprojectPipeline = std::make_unique<VKComputePipeline>(
            m_ReprojectSpv, reprojLayouts, std::vector<VkPushConstantRange>{ reprojPc });

        const std::vector<VkDescriptorSetLayout> momentsLayouts = { globalLayout, m_MomentsLayout };
        VkPushConstantRange momentsPc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfMomentsPC) };
        m_MomentsPipeline = std::make_unique<VKComputePipeline>(
            m_MomentsSpv, momentsLayouts, std::vector<VkPushConstantRange>{ momentsPc });

        const std::vector<VkDescriptorSetLayout> atrousLayouts = { globalLayout, m_AtrousLayout };
        VkPushConstantRange atrousPc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfAtrousPC) };
        m_AtrousPipeline = std::make_unique<VKComputePipeline>(
            m_AtrousSpv, atrousLayouts, std::vector<VkPushConstantRange>{ atrousPc });
    }

    void SvgfDenoiser::Shutdown()
    {
        LH_PROFILE_FUNCTION();
        VkDevice device = VulkanContext::Get().GetDevice();
        m_PassthroughPipeline.reset();
        m_ReprojectPipeline.reset();
        m_MomentsPipeline.reset();
        m_AtrousPipeline.reset();
        if (m_Sampler)         vkDestroySampler(device, m_Sampler, nullptr);
        if (m_PassLayout)      vkDestroyDescriptorSetLayout(device, m_PassLayout, nullptr);
        if (m_ReprojectLayout) vkDestroyDescriptorSetLayout(device, m_ReprojectLayout, nullptr);
        if (m_MomentsLayout)   vkDestroyDescriptorSetLayout(device, m_MomentsLayout, nullptr);
        if (m_AtrousLayout)    vkDestroyDescriptorSetLayout(device, m_AtrousLayout, nullptr);
        m_Sampler        = VK_NULL_HANDLE;
        m_PassLayout      = VK_NULL_HANDLE;
        m_ReprojectLayout = VK_NULL_HANDLE;
        m_MomentsLayout   = VK_NULL_HANDLE;
        m_AtrousLayout    = VK_NULL_HANDLE;
        m_PassthroughSpv.clear();
        m_ReprojectSpv.clear();
        m_MomentsSpv.clear();
        m_AtrousSpv.clear();
        m_Pipeline = nullptr;
    }

    bool SvgfDenoiser::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        LH_PROFILE_FUNCTION();
        if (!m_Pipeline) return false;

        // Defer the old pipeline's destruction; an in-flight frame may still bind it. PushDeletion
        // drains it MAX_FRAMES_IN_FLIGHT frames later (no vkDeviceWaitIdle).
        if (name == "svgf_passthrough.slang" && m_PassLayout != VK_NULL_HANDLE)
        {
            if (m_PassthroughPipeline)
                VulkanContext::Get().PushDeletion([p = m_PassthroughPipeline.release()]() { delete p; });
            m_PassthroughSpv = spv;
            const std::vector<VkDescriptorSetLayout> layouts = { m_PassLayout };
            m_PassthroughPipeline = std::make_unique<VKComputePipeline>(
                m_PassthroughSpv, layouts, std::vector<VkPushConstantRange>{});
            return true;
        }
        const char* myReproj = (m_Channel == DenoiserChannel::Reflections)
            ? "svgf_spec_reproject.slang" : "svgf_reproject.slang";
        if (name == myReproj && m_ReprojectLayout != VK_NULL_HANDLE)
        {
            if (m_ReprojectPipeline)
                VulkanContext::Get().PushDeletion([p = m_ReprojectPipeline.release()]() { delete p; });
            m_ReprojectSpv = spv;
            const std::vector<VkDescriptorSetLayout> layouts = {
                m_Pipeline->GetGlobal().GetSetLayout(), m_ReprojectLayout };
            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfReprojectPC) };
            m_ReprojectPipeline = std::make_unique<VKComputePipeline>(
                m_ReprojectSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
            return true;
        }
        if (name == "svgf_moments.slang" && m_MomentsLayout != VK_NULL_HANDLE)
        {
            if (m_MomentsPipeline)
                VulkanContext::Get().PushDeletion([p = m_MomentsPipeline.release()]() { delete p; });
            m_MomentsSpv = spv;
            const std::vector<VkDescriptorSetLayout> layouts = {
                m_Pipeline->GetGlobal().GetSetLayout(), m_MomentsLayout };
            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfMomentsPC) };
            m_MomentsPipeline = std::make_unique<VKComputePipeline>(
                m_MomentsSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
            return true;
        }
        if (name == "svgf_atrous.slang" && m_AtrousLayout != VK_NULL_HANDLE)
        {
            if (m_AtrousPipeline)
                VulkanContext::Get().PushDeletion([p = m_AtrousPipeline.release()]() { delete p; });
            m_AtrousSpv = spv;
            const std::vector<VkDescriptorSetLayout> layouts = {
                m_Pipeline->GetGlobal().GetSetLayout(), m_AtrousLayout };
            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfAtrousPC) };
            m_AtrousPipeline = std::make_unique<VKComputePipeline>(
                m_AtrousSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
            return true;
        }
        return false;
    }

    void SvgfDenoiser::AllocateViewSets(ViewResources& vr)
    {
        LH_PROFILE_FUNCTION();
        if (vr.descPool == VK_NULL_HANDLE) return;
        VkDevice device = VulkanContext::Get().GetDevice();
        ChannelRefs c = Resolve(m_Channel, vr);
        const std::string pfx = (m_Channel == DenoiserChannel::Gi) ? "View.SvgfGi" : "View.Svgf";

        if (m_PassLayout != VK_NULL_HANDLE)
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = vr.descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_PassLayout;
            if (vkAllocateDescriptorSets(device, &ai, c.passthroughSet) != VK_SUCCESS)
            {
                LH_LOG(Renderer, error, "SvgfDenoiser: passthrough set alloc failed; bump view pool sizes");
                *c.passthroughSet = VK_NULL_HANDLE;
            }
            else VulkanContext::SetDebugName(*c.passthroughSet, (pfx + "Passthrough").c_str());
        }

        if (m_ReprojectLayout != VK_NULL_HANDLE)
        {
            VkDescriptorSetLayout layouts[2] = { m_ReprojectLayout, m_ReprojectLayout };
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = vr.descPool; ai.descriptorSetCount = 2; ai.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(device, &ai, c.reprojectSet) != VK_SUCCESS)
            {
                LH_LOG(Renderer, error, "SvgfDenoiser: reproject sets alloc failed; bump view pool sizes");
                c.reprojectSet[0] = c.reprojectSet[1] = VK_NULL_HANDLE;
            }
            else
            {
                VulkanContext::SetDebugName(c.reprojectSet[0], (pfx + "Reproject0").c_str());
                VulkanContext::SetDebugName(c.reprojectSet[1], (pfx + "Reproject1").c_str());
            }
        }

        if (m_MomentsLayout != VK_NULL_HANDLE)
        {
            VkDescriptorSetLayout layouts[2] = { m_MomentsLayout, m_MomentsLayout };
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = vr.descPool; ai.descriptorSetCount = 2; ai.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(device, &ai, c.momentsSet) != VK_SUCCESS)
            {
                LH_LOG(Renderer, error, "SvgfDenoiser: moments sets alloc failed; bump view pool sizes");
                c.momentsSet[0] = c.momentsSet[1] = VK_NULL_HANDLE;
            }
            else
            {
                VulkanContext::SetDebugName(c.momentsSet[0], (pfx + "Moments0").c_str());
                VulkanContext::SetDebugName(c.momentsSet[1], (pfx + "Moments1").c_str());
            }
        }

        if (m_AtrousLayout != VK_NULL_HANDLE)
        {
            VkDescriptorSetLayout layouts[2] = { m_AtrousLayout, m_AtrousLayout };
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = vr.descPool; ai.descriptorSetCount = 2; ai.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(device, &ai, c.atrousSet) != VK_SUCCESS)
            {
                LH_LOG(Renderer, error, "SvgfDenoiser: atrous sets alloc failed; bump view pool sizes");
                c.atrousSet[0] = c.atrousSet[1] = VK_NULL_HANDLE;
            }
            else
            {
                VulkanContext::SetDebugName(c.atrousSet[0], (pfx + "Atrous0").c_str());
                VulkanContext::SetDebugName(c.atrousSet[1], (pfx + "Atrous1").c_str());
            }
        }
    }

    void SvgfDenoiser::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        LH_PROFILE_FUNCTION();
        VkDevice device = VulkanContext::Get().GetDevice();
        ChannelRefs c = Resolve(m_Channel, vr);
        auto viewOf = [](const std::shared_ptr<Texture>& t) {
            return std::static_pointer_cast<VKTexture>(t)->GetImageView();
        };

        // Passthrough set: b0 noisy input sampler, b1 denoised storage.
        if (*c.passthroughSet != VK_NULL_HANDLE && *c.noisy && *c.denoised)
        {
            VkDescriptorImageInfo diIn{ m_Sampler, viewOf(*c.noisy), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo outImg{ VK_NULL_HANDLE, viewOf(*c.denoised), VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w[2]{};
            w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[0].dstSet = *c.passthroughSet; w[0].dstBinding = 0;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].descriptorCount = 1; w[0].pImageInfo = &diIn;
            w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[1].dstSet = *c.passthroughSet; w[1].dstBinding = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].descriptorCount = 1; w[1].pImageInfo = &outImg;
            vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
        }

        // Reproject sets: pre-build both parities (set[p] reads prev = [p^1], writes curr = [p]).
        // Reproject b3: DI/GI bind slim MOTION; the specular variant binds slim ROUGHNESS (it computes
        // the reflection's own motion internally via hit-distance virtual reprojection). b10 slim matID
        // is written for every channel (layout parity); only the motion variant's shader reads it.
        const std::shared_ptr<Texture> b3Tex = (m_Channel == DenoiserChannel::Reflections)
            ? targets.GetSlimRoughness() : targets.GetSlimMotion();
        if (c.reprojectSet[0] != VK_NULL_HANDLE && *c.noisy
            && c.colorHist[0] && c.moments[0] && c.geom[0]
            && targets.GetSceneDepth() && targets.GetSlimNormal() && b3Tex
            && targets.GetSlimMaterialID())
        {
            const VkImageView depthV  = viewOf(targets.GetSceneDepth());
            const VkImageView normalV = viewOf(targets.GetSlimNormal());
            const VkImageView motionV = viewOf(b3Tex);
            const VkImageView diV     = viewOf(*c.noisy);
            const VkImageView matIdV  = viewOf(targets.GetSlimMaterialID());

            for (u32 p = 0; p < 2; ++p)
            {
                const u32 q = p ^ 1u;  // prev parity
                VkDescriptorImageInfo info[11]{};
                info[0]  = { m_Sampler, diV,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[1]  = { m_Sampler, depthV,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[2]  = { m_Sampler, normalV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[3]  = { m_Sampler, motionV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[4]  = { VK_NULL_HANDLE, viewOf(c.colorHist[q]), VK_IMAGE_LAYOUT_GENERAL };
                info[5]  = { VK_NULL_HANDLE, viewOf(c.moments[q]),   VK_IMAGE_LAYOUT_GENERAL };
                info[6]  = { VK_NULL_HANDLE, viewOf(c.geom[q]),      VK_IMAGE_LAYOUT_GENERAL };
                info[7]  = { VK_NULL_HANDLE, viewOf(c.colorHist[p]), VK_IMAGE_LAYOUT_GENERAL };
                info[8]  = { VK_NULL_HANDLE, viewOf(c.moments[p]),   VK_IMAGE_LAYOUT_GENERAL };
                info[9]  = { VK_NULL_HANDLE, viewOf(c.geom[p]),      VK_IMAGE_LAYOUT_GENERAL };
                info[10] = { m_Sampler, matIdV,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

                VkWriteDescriptorSet w[11]{};
                for (u32 i = 0; i < 11; ++i)
                {
                    w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    w[i].dstSet          = c.reprojectSet[p];
                    w[i].dstBinding      = i;
                    w[i].descriptorType  = (i < 4 || i == 10) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    w[i].descriptorCount = 1;
                    w[i].pImageInfo      = &info[i];
                }
                vkUpdateDescriptorSets(device, 11, w, 0, nullptr);
            }
        }

        // Moments sets: per parity p, b0/b1 = colorHist[p]/moments[p] (the reproject's curr output),
        // b2/b3 depth/normal samplers, b4 svgfAtrous[0] (a-trous level-0 input; shared, not ping-ponged).
        if (c.momentsSet[0] != VK_NULL_HANDLE
            && c.colorHist[0] && c.moments[0] && c.atrous[0]
            && targets.GetSceneDepth() && targets.GetSlimNormal())
        {
            const VkImageView depthV  = viewOf(targets.GetSceneDepth());
            const VkImageView normalV = viewOf(targets.GetSlimNormal());
            const VkImageView a0V     = viewOf(c.atrous[0]);

            for (u32 p = 0; p < 2; ++p)
            {
                VkDescriptorImageInfo info[5]{};
                info[0] = { VK_NULL_HANDLE, viewOf(c.colorHist[p]), VK_IMAGE_LAYOUT_GENERAL };
                info[1] = { VK_NULL_HANDLE, viewOf(c.moments[p]),   VK_IMAGE_LAYOUT_GENERAL };
                info[2] = { m_Sampler, depthV,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[3] = { m_Sampler, normalV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[4] = { VK_NULL_HANDLE, a0V, VK_IMAGE_LAYOUT_GENERAL };

                const VkDescriptorType types[5] = {
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                };
                VkWriteDescriptorSet w[5]{};
                for (u32 i = 0; i < 5; ++i)
                {
                    w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    w[i].dstSet = c.momentsSet[p]; w[i].dstBinding = i;
                    w[i].descriptorType = types[i]; w[i].descriptorCount = 1; w[i].pImageInfo = &info[i];
                }
                vkUpdateDescriptorSets(device, 5, w, 0, nullptr);
            }
        }

        // A-trous sets: per iter parity ip, b0 = svgfAtrous[ip] (in), b3 = svgfAtrous[ip^1] (out),
        // b1/b2 depth/normal samplers, b4 svgfDenoised (final-level output), b5 slim roughness (spec
        // channels' edge-stop; bound for every channel, diffuse ones disable via phiRough 0).
        if (c.atrousSet[0] != VK_NULL_HANDLE
            && c.atrous[0] && c.atrous[1] && *c.denoised
            && targets.GetSceneDepth() && targets.GetSlimNormal() && targets.GetSlimRoughness())
        {
            const VkImageView depthV  = viewOf(targets.GetSceneDepth());
            const VkImageView normalV = viewOf(targets.GetSlimNormal());
            const VkImageView denV    = viewOf(*c.denoised);
            const VkImageView roughV  = viewOf(targets.GetSlimRoughness());

            for (u32 ip = 0; ip < 2; ++ip)
            {
                const u32 op = ip ^ 1u;
                VkDescriptorImageInfo info[6]{};
                info[0] = { VK_NULL_HANDLE, viewOf(c.atrous[ip]), VK_IMAGE_LAYOUT_GENERAL };
                info[1] = { m_Sampler, depthV,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[2] = { m_Sampler, normalV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[3] = { VK_NULL_HANDLE, viewOf(c.atrous[op]), VK_IMAGE_LAYOUT_GENERAL };
                info[4] = { VK_NULL_HANDLE, denV, VK_IMAGE_LAYOUT_GENERAL };
                info[5] = { m_Sampler, roughV,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

                const VkDescriptorType types[6] = {
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                };
                VkWriteDescriptorSet w[6]{};
                for (u32 i = 0; i < 6; ++i)
                {
                    w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    w[i].dstSet = c.atrousSet[ip]; w[i].dstBinding = i;
                    w[i].descriptorType = types[i]; w[i].descriptorCount = 1; w[i].pImageInfo = &info[i];
                }
                vkUpdateDescriptorSets(device, 6, w, 0, nullptr);
            }
        }
    }

    RG::ResourceHandle SvgfDenoiser::AddPasses(RG::RenderGraph& rg, const DenoiseInputs& in)
    {
        LH_PROFILE_FUNCTION();
        // Invalid input -> ReSTIR produced no DI this frame; return invalid so the GeometryPass skips
        // the read and pbr.frag runs its own light loop.
        if (!in.di.IsValid()) return {};

        ViewResources* vr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!vr) return {};
        ChannelRefs c = Resolve(m_Channel, *vr);
        if (!*c.denoised) return {};

        const bool enabled = Settings().enabled;
        const bool chainReady = m_ReprojectPipeline && m_MomentsPipeline && m_AtrousPipeline
            && c.reprojectSet[0] != VK_NULL_HANDLE
            && c.momentsSet[0] != VK_NULL_HANDLE
            && c.atrousSet[0] != VK_NULL_HANDLE
            && c.colorHist[0] && c.moments[0] && c.atrous[0] && c.atrous[1];
        if (enabled && chainReady)
            return AddDenoiseChain(rg, in);
        if (m_PassthroughPipeline && *c.passthroughSet != VK_NULL_HANDLE)
            return AddPassthroughPass(rg, in);
        return {};
    }

    RG::ResourceHandle SvgfDenoiser::AddDenoiseChain(RG::RenderGraph& rg, const DenoiseInputs& in)
    {
        LH_PROFILE_FUNCTION();
        const SvgfSettings& s = Settings();

        // Working resolution = the channel's history-texture extent. Half-res GI runs the whole chain
        // below the full G-buffer; full-res channels keep chW/chH == full -> scale 1 (identity remap).
        ViewResources* vrTop = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        u32 chW = vrTop ? vrTop->width  : 0u;
        u32 chH = vrTop ? vrTop->height : 0u;
        if (vrTop)
        {
            ChannelRefs cr = Resolve(m_Channel, *vrTop);
            if (cr.colorHist[0])
            {
                auto wt = std::static_pointer_cast<VKTexture>(cr.colorHist[0]);
                chW = wt->GetWidth();
                chH = wt->GetHeight();
            }
        }
        const i32 gbufScale = (vrTop && chW == vrTop->width && chH == vrTop->height) ? 1 : 2;

        // Channel routing: confidence only for the MOTION-variant channels (Di / Gi / DiSpecular) -
        // the spec variant's input alpha is hitDist, never confidence. Roughness edge-stop only for the
        // specular channels (Reflections / DiSpecular); zeroing here is structural, not a UI convention.
        const bool motionVariant = (m_Channel != DenoiserChannel::Reflections);
        const bool specChannel   = (m_Channel == DenoiserChannel::Reflections
                                 || m_Channel == DenoiserChannel::DiSpecular);

        SvgfReprojectPC rpc{};
        rpc.alphaColor       = s.alphaColor;
        rpc.alphaMoments     = s.alphaMoments;
        rpc.historyCap       = static_cast<f32>(s.historyCap);
        rpc.depthThreshold   = s.depthThreshold;
        rpc.normalThreshold  = s.normalThreshold;
        rpc.gbufferScale     = gbufScale;
        rpc.dispatchW        = static_cast<i32>(chW);
        rpc.dispatchH        = static_cast<i32>(chH);
        rpc.antiFireflySigma = s.antiFireflySigma;
        rpc.confidenceScale  = motionVariant ? s.confidenceScale : 0.0f;

        SvgfMomentsPC mpc{};
        mpc.phiDepth     = s.phiDepth;
        mpc.phiNormal    = s.phiNormal;
        mpc.gbufferScale = gbufScale;
        mpc.dispatchW    = static_cast<i32>(chW);
        mpc.dispatchH    = static_cast<i32>(chH);

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 fp       = frameAbs & 1u;                       // reproject/moments curr parity
        const u32 N        = std::max(1u, s.atrousIterations);

        // Import each distinct VkImage at most ONCE per frame: re-importing aliases distinct RG nodes
        // (a known hazard). colorHist[fp], moments[fp], svgfAtrous[0], svgfAtrous[1], svgfDenoised each
        // get exactly one ImportResource; their handles thread forward across the chain so the RG inserts
        // the within-frame barriers. History geom[fp] (reproject b9) stays descriptor-only (cross-frame).
        auto importTex = [&rg](const std::shared_ptr<Texture>& t, const char* name) {
            auto vt = std::static_pointer_cast<VKTexture>(t);
            RG::TextureDesc d;
            d.name   = name;
            d.width  = vt->GetWidth();
            d.height = vt->GetHeight();
            d.format = RG::TextureFormat::RGBA16_Float;
            return rg.ImportResource(d, (void*)vt->GetImage(), (void*)vt->GetImageView(),
                                     RG::ResourceState::Undefined);
        };

        // Reproject: writes colorHist[fp] (integrated color + temporal variance) + moments[fp]. The
        // current-frame inputs (DI/depth/normal/motion) come in through the RG; the curr history images
        // are imported here and their handles (hColor/hMom) thread into the moments read.
        struct ReprojData {
            RG::ResourceHandle di, depth, normal, motion;
            RG::ResourceHandle color, mom;
        };
        RG::ResourceHandle hColor{}, hMom{};
        rg.AddComputePass<ReprojData>(
            PassName(0),
            RG::QueueFamily::AsyncCompute,
            [&, this](ReprojData& data, RG::RenderPassBuilder& builder) {
                data.di = builder.ReadStorageImage(in.di);
                if (in.depth.IsValid())  data.depth  = builder.ReadStorageImage(in.depth);
                if (in.normal.IsValid()) data.normal = builder.ReadStorageImage(in.normal);
                if (in.motion.IsValid()) data.motion = builder.ReadStorageImage(in.motion);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                ChannelRefs c = Resolve(m_Channel, *vr);
                data.color = importTex(c.colorHist[fp], "SvgfColorHistCurr");
                data.color = builder.WriteStorageImage(data.color);
                hColor     = data.color;
                data.mom   = importTex(c.moments[fp], "SvgfMomentsCurr");
                data.mom   = builder.WriteStorageImage(data.mom);
                hMom       = data.mom;
            },
            [this, rpc](ReprojData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr) return;
                ChannelRefs c = Resolve(m_Channel, *vr);
                const u32 fa = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u32 parity = fa & 1u;
                const u32 sl     = fa % MAX_FRAMES_IN_FLIGHT;
                if (c.reprojectSet[parity] == VK_NULL_HANDLE) return;

                m_ReprojectPipeline->Bind(cmd);
                VkDescriptorSet sets[2] = { vr->globalDescriptorSet[sl], c.reprojectSet[parity] };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_ReprojectPipeline->GetLayout(), 0, 2, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_ReprojectPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfReprojectPC), &rpc);

                const u32 gx = (static_cast<u32>(rpc.dispatchW) + 7) / 8;
                const u32 gy = (static_cast<u32>(rpc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, gx, gy, 1);
            });

        // Moments: reads colorHist[fp] + moments[fp] (threaded hColor/hMom -> reproject->moments RAW
        // barrier), writes svgfAtrous[0]. Depth/normal are descriptor-only (already SHADER_READ_ONLY
        // from the reproject's RG reads). svgfAtrous[0] is imported ONCE here; hA0 threads into a-trous.
        struct MomentsData {
            RG::ResourceHandle color, mom, out;
        };
        RG::ResourceHandle hA0{};
        rg.AddComputePass<MomentsData>(
            PassName(1),
            RG::QueueFamily::AsyncCompute,
            [&, this](MomentsData& data, RG::RenderPassBuilder& builder) {
                // GENERAL-preserving reads: colorHist/moments are STORAGE images (imageLoad in the
                // shader), so they must stay GENERAL; ReadStorageImage would transition them to
                // SHADER_READ_ONLY and mismatch the STORAGE_IMAGE descriptor.
                data.color = builder.ReadStorageImageGeneral(hColor);
                data.mom   = builder.ReadStorageImageGeneral(hMom);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                ChannelRefs c = Resolve(m_Channel, *vr);
                data.out = importTex(c.atrous[0], "SvgfAtrous0");
                data.out = builder.WriteStorageImage(data.out);
                hA0      = data.out;
            },
            [this, mpc](MomentsData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr) return;
                ChannelRefs c = Resolve(m_Channel, *vr);
                const u32 fa = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u32 parity = fa & 1u;
                const u32 sl     = fa % MAX_FRAMES_IN_FLIGHT;
                if (c.momentsSet[parity] == VK_NULL_HANDLE) return;

                m_MomentsPipeline->Bind(cmd);
                VkDescriptorSet sets[2] = { vr->globalDescriptorSet[sl], c.momentsSet[parity] };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_MomentsPipeline->GetLayout(), 0, 2, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_MomentsPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfMomentsPC), &mpc);

                const u32 gx = (static_cast<u32>(mpc.dispatchW) + 7) / 8;
                const u32 gy = (static_cast<u32>(mpc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, gx, gy, 1);
            });

        // A-trous: N levels ping-ponging svgfAtrous[0]/[1] with a doubling step. hA[2] tracks the live
        // handle per slot: hA[0] starts as the moments output; svgfAtrous[1] is imported ONCE (the first
        // time it is written, i==0). Each level reads hA[in] and writes hA[out] (threaded -> per-level
        // RAW barrier). The final level also writes svgfDenoised (imported once -> hDen).
        RG::ResourceHandle hA[2] = { hA0, {} };
        RG::ResourceHandle hDen{};
        for (u32 i = 0; i < N; ++i)
        {
            const u32  inPar   = i & 1u;
            const u32  outPar  = inPar ^ 1u;
            const bool isFinal = (i == N - 1);
            const i32  stepSize = 1 << i;

            SvgfAtrousPC apc{};
            apc.stepSize     = stepSize;
            apc.writeFinal   = isFinal ? 1 : 0;
            apc.phiColor     = s.phiColor;
            apc.phiNormal    = s.phiNormal;
            apc.phiDepth     = s.phiDepth;
            apc.gbufferScale = gbufScale;
            apc.dispatchW    = static_cast<i32>(chW);
            apc.dispatchH    = static_cast<i32>(chH);
            apc.phiRough     = specChannel ? s.phiRough : 0.0f;

            struct AtrousData {
                RG::ResourceHandle in, out, den;
            };
            rg.AddComputePass<AtrousData>(
                PassName(2),
                RG::QueueFamily::AsyncCompute,
                [&, this](AtrousData& data, RG::RenderPassBuilder& builder) {
                    data.in = builder.ReadStorageImageGeneral(hA[inPar]);  // STORAGE imageLoad; keep GENERAL

                    ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                    ChannelRefs c = Resolve(m_Channel, *vr);
                    if (!hA[outPar].IsValid())
                        hA[outPar] = importTex(c.atrous[outPar], "SvgfAtrousAlt");
                    data.out   = builder.WriteStorageImage(hA[outPar]);
                    hA[outPar] = data.out;

                    if (isFinal)
                    {
                        data.den = importTex(*c.denoised, "SvgfDenoised");
                        data.den = builder.WriteStorageImage(data.den);
                        hDen     = data.den;
                    }
                },
                [this, apc, inPar](AtrousData&, RG::RenderPassContext& ctx) {
                    VkCommandBuffer cmd = ctx.commandBuffer;
                    ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                    if (!vr) return;
                    ChannelRefs c = Resolve(m_Channel, *vr);
                    const u32 sl = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex())
                                 % MAX_FRAMES_IN_FLIGHT;
                    if (c.atrousSet[inPar] == VK_NULL_HANDLE) return;

                    m_AtrousPipeline->Bind(cmd);
                    VkDescriptorSet sets[2] = { vr->globalDescriptorSet[sl], c.atrousSet[inPar] };
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        m_AtrousPipeline->GetLayout(), 0, 2, sets, 0, nullptr);
                    vkCmdPushConstants(cmd, m_AtrousPipeline->GetLayout(),
                        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfAtrousPC), &apc);

                    const u32 gx = (static_cast<u32>(apc.dispatchW) + 7) / 8;
                    const u32 gy = (static_cast<u32>(apc.dispatchH) + 7) / 8;
                    vkCmdDispatch(cmd, gx, gy, 1);
                });
        }

        return hDen;
    }

    RG::ResourceHandle SvgfDenoiser::AddPassthroughPass(RG::RenderGraph& rg, const DenoiseInputs& in)
    {
        LH_PROFILE_FUNCTION();
        struct PassData {
            RG::ResourceHandle di;
            RG::ResourceHandle out;
        };
        RG::ResourceHandle outHandle{};
        rg.AddComputePass<PassData>(
            PassName(3),
            RG::QueueFamily::AsyncCompute,
            [&, this](PassData& data, RG::RenderPassBuilder& builder) {
                data.di = builder.ReadStorageImage(in.di);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                ChannelRefs c = Resolve(m_Channel, *vr);
                auto outTex = std::static_pointer_cast<VKTexture>(*c.denoised);
                RG::TextureDesc desc;
                desc.name   = "SvgfDenoised";
                desc.width  = outTex->GetWidth();
                desc.height = outTex->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;
                data.out = rg.ImportResource(desc,
                    (void*)outTex->GetImage(), (void*)outTex->GetImageView(),
                    RG::ResourceState::Undefined);
                data.out = builder.WriteStorageImage(data.out);
                outHandle = data.out;
            },
            [this](PassData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr) return;
                ChannelRefs c = Resolve(m_Channel, *vr);
                if (*c.passthroughSet == VK_NULL_HANDLE) return;

                m_PassthroughPipeline->Bind(cmd);
                VkDescriptorSet sets[1] = { *c.passthroughSet };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_PassthroughPipeline->GetLayout(), 0, 1, sets, 0, nullptr);

                auto outTex = (c.denoised && *c.denoised)
                    ? std::static_pointer_cast<VKTexture>(*c.denoised) : nullptr;
                const u32 groupX = ((outTex ? outTex->GetWidth()  : vr->width)  + 7) / 8;
                const u32 groupY = ((outTex ? outTex->GetHeight() : vr->height) + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });
        return outHandle;
    }
}
