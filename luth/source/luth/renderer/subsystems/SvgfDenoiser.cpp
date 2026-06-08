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
        // Mirrors svgf_reproject.comp's push_constant (5 floats = 20 B).
        struct SvgfReprojectPC {
            f32 alphaColor;
            f32 alphaMoments;
            f32 historyCap;
            f32 depthThreshold;
            f32 normalThreshold;
        };
        static_assert(sizeof(SvgfReprojectPC) == 20, "SvgfReprojectPC must match svgf_reproject.comp push_constant");

        // Mirrors svgf_moments.comp's push_constant (2 floats = 8 B).
        struct SvgfMomentsPC {
            f32 phiDepth;
            f32 phiNormal;
        };
        static_assert(sizeof(SvgfMomentsPC) == 8, "SvgfMomentsPC must match svgf_moments.comp push_constant");

        // Mirrors svgf_atrous.comp's push_constant (2 ints + 3 floats = 20 B).
        struct SvgfAtrousPC {
            i32 stepSize;
            i32 writeFinal;
            f32 phiColor;
            f32 phiNormal;
            f32 phiDepth;
        };
        static_assert(sizeof(SvgfAtrousPC) == 20, "SvgfAtrousPC must match svgf_atrous.comp push_constant");
    }

    bool SvgfDenoiser::IsEnabled() const
    {
        return m_Pipeline && m_Pipeline->GetSystem().GetSvgfSettings().enabled;
    }

    void SvgfDenoiser::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear clamp-to-edge — matches the ReSTIR/RT pass samplers for SceneDepth/SlimNormal.
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
        // storage. History stays GENERAL (storage), so no UAB / per-frame rewrite — the two sets are
        // pre-built per parity and bound by frameAbs & 1. The denoised output moved to the à-trous final
        // level, so the reproject no longer binds it.
        {
            VkDescriptorSetLayoutBinding b[10]{};
            for (u32 i = 0; i < 10; ++i)
            {
                b[i].binding         = i;
                b[i].descriptorCount = 1;
                b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
                b[i].descriptorType  = (i < 4) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            }
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 10; ci.pBindings = b;
            vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_ReprojectLayout);
        }

        // Moments set (pass-local): b0 colorHist[curr] storage, b1 moments[curr] storage, b2 depth
        // sampler, b3 normal sampler, b4 svgfAtrous[0] storage (à-trous level-0 input).
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

        // À-trous set (pass-local): b0 svgfAtrous[IN] storage, b1 depth sampler, b2 normal sampler,
        // b3 svgfAtrous[OUT] storage, b4 svgfDenoised storage (final level). Two sets by iter parity.
        {
            VkDescriptorSetLayoutBinding b[5]{};
            const VkDescriptorType types[5] = {
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            };
            for (u32 i = 0; i < 5; ++i)
            {
                b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                b[i].descriptorType = types[i];
            }
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 5; ci.pBindings = b;
            vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_AtrousLayout);
        }

        if (auto sh = ShaderLibrary::LoadEngine("shaders/svgf_passthrough.comp"))
            m_PassthroughSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/svgf_reproject.comp"))
            m_ReprojectSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/svgf_moments.comp"))
            m_MomentsSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/svgf_atrous.comp"))
            m_AtrousSpv = sh->GetSpirV();
        if (m_PassthroughSpv.empty() || m_ReprojectSpv.empty() || m_MomentsSpv.empty() || m_AtrousSpv.empty())
        {
            LH_CORE_ERROR("SvgfDenoiser: failed to load svgf_passthrough/reproject/moments/atrous.comp SPIR-V");
            return;
        }

        const std::vector<VkDescriptorSetLayout> passLayouts = { m_PassLayout };
        m_PassthroughPipeline = std::make_unique<VKComputePipeline>(
            m_PassthroughSpv, passLayouts, std::vector<VkPushConstantRange>{});

        VkDescriptorSetLayout globalLayout = m_Pipeline->GetGlobal().GetSetLayout();

        // Reproject binds Set 0 (global UBO — nearZ/farZ/viewportSize) + the pass-local set.
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
        if (!m_Pipeline) return false;

        // Defer the old pipeline's destruction — an in-flight frame may still bind it; PushDeletion
        // drains it MAX_FRAMES_IN_FLIGHT frames later (no vkDeviceWaitIdle).
        if (name == "svgf_passthrough.comp" && m_PassLayout != VK_NULL_HANDLE)
        {
            if (m_PassthroughPipeline)
                VulkanContext::Get().PushDeletion([p = m_PassthroughPipeline.release()]() { delete p; });
            m_PassthroughSpv = spv;
            const std::vector<VkDescriptorSetLayout> layouts = { m_PassLayout };
            m_PassthroughPipeline = std::make_unique<VKComputePipeline>(
                m_PassthroughSpv, layouts, std::vector<VkPushConstantRange>{});
            return true;
        }
        if (name == "svgf_reproject.comp" && m_ReprojectLayout != VK_NULL_HANDLE)
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
        if (name == "svgf_moments.comp" && m_MomentsLayout != VK_NULL_HANDLE)
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
        if (name == "svgf_atrous.comp" && m_AtrousLayout != VK_NULL_HANDLE)
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
        if (vr.descPool == VK_NULL_HANDLE) return;
        VkDevice device = VulkanContext::Get().GetDevice();

        if (m_PassLayout != VK_NULL_HANDLE)
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = vr.descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_PassLayout;
            if (vkAllocateDescriptorSets(device, &ai, &vr.svgfPassthroughDescSet) != VK_SUCCESS)
            {
                LH_CORE_ERROR("SvgfDenoiser: passthrough set alloc failed; bump view pool sizes");
                vr.svgfPassthroughDescSet = VK_NULL_HANDLE;
            }
            else VulkanContext::SetDebugName(vr.svgfPassthroughDescSet, "View.SvgfPassthrough");
        }

        if (m_ReprojectLayout != VK_NULL_HANDLE)
        {
            VkDescriptorSetLayout layouts[2] = { m_ReprojectLayout, m_ReprojectLayout };
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = vr.descPool; ai.descriptorSetCount = 2; ai.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(device, &ai, vr.svgfReprojectDescSet) != VK_SUCCESS)
            {
                LH_CORE_ERROR("SvgfDenoiser: reproject sets alloc failed; bump view pool sizes");
                vr.svgfReprojectDescSet[0] = vr.svgfReprojectDescSet[1] = VK_NULL_HANDLE;
            }
            else
            {
                VulkanContext::SetDebugName(vr.svgfReprojectDescSet[0], "View.SvgfReproject0");
                VulkanContext::SetDebugName(vr.svgfReprojectDescSet[1], "View.SvgfReproject1");
            }
        }

        if (m_MomentsLayout != VK_NULL_HANDLE)
        {
            VkDescriptorSetLayout layouts[2] = { m_MomentsLayout, m_MomentsLayout };
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = vr.descPool; ai.descriptorSetCount = 2; ai.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(device, &ai, vr.svgfMomentsDescSet) != VK_SUCCESS)
            {
                LH_CORE_ERROR("SvgfDenoiser: moments sets alloc failed; bump view pool sizes");
                vr.svgfMomentsDescSet[0] = vr.svgfMomentsDescSet[1] = VK_NULL_HANDLE;
            }
            else
            {
                VulkanContext::SetDebugName(vr.svgfMomentsDescSet[0], "View.SvgfMoments0");
                VulkanContext::SetDebugName(vr.svgfMomentsDescSet[1], "View.SvgfMoments1");
            }
        }

        if (m_AtrousLayout != VK_NULL_HANDLE)
        {
            VkDescriptorSetLayout layouts[2] = { m_AtrousLayout, m_AtrousLayout };
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = vr.descPool; ai.descriptorSetCount = 2; ai.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(device, &ai, vr.svgfAtrousDescSet) != VK_SUCCESS)
            {
                LH_CORE_ERROR("SvgfDenoiser: atrous sets alloc failed; bump view pool sizes");
                vr.svgfAtrousDescSet[0] = vr.svgfAtrousDescSet[1] = VK_NULL_HANDLE;
            }
            else
            {
                VulkanContext::SetDebugName(vr.svgfAtrousDescSet[0], "View.SvgfAtrous0");
                VulkanContext::SetDebugName(vr.svgfAtrousDescSet[1], "View.SvgfAtrous1");
            }
        }
    }

    void SvgfDenoiser::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        auto viewOf = [](const std::shared_ptr<Texture>& t) {
            return std::static_pointer_cast<VKTexture>(t)->GetImageView();
        };

        // Passthrough set: b0 noisy DI sampler, b1 denoised storage.
        if (vr.svgfPassthroughDescSet != VK_NULL_HANDLE && vr.restirDI && vr.svgfDenoised)
        {
            VkDescriptorImageInfo diIn{ m_Sampler, viewOf(vr.restirDI), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo outImg{ VK_NULL_HANDLE, viewOf(vr.svgfDenoised), VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w[2]{};
            w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[0].dstSet = vr.svgfPassthroughDescSet; w[0].dstBinding = 0;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].descriptorCount = 1; w[0].pImageInfo = &diIn;
            w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[1].dstSet = vr.svgfPassthroughDescSet; w[1].dstBinding = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].descriptorCount = 1; w[1].pImageInfo = &outImg;
            vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
        }

        // Reproject sets: pre-build both parities (set[p] reads prev = [p^1], writes curr = [p]).
        if (vr.svgfReprojectDescSet[0] != VK_NULL_HANDLE && vr.restirDI
            && vr.svgfColorHist[0] && vr.svgfMoments[0] && vr.svgfGeom[0]
            && targets.GetSceneDepth() && targets.GetSlimNormal() && targets.GetSlimMotion())
        {
            const VkImageView depthV  = viewOf(targets.GetSceneDepth());
            const VkImageView normalV = viewOf(targets.GetSlimNormal());
            const VkImageView motionV = viewOf(targets.GetSlimMotion());
            const VkImageView diV     = viewOf(vr.restirDI);

            for (u32 p = 0; p < 2; ++p)
            {
                const u32 q = p ^ 1u;  // prev parity
                VkDescriptorImageInfo info[10]{};
                info[0] = { m_Sampler, diV,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[1] = { m_Sampler, depthV,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[2] = { m_Sampler, normalV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[3] = { m_Sampler, motionV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[4] = { VK_NULL_HANDLE, viewOf(vr.svgfColorHist[q]), VK_IMAGE_LAYOUT_GENERAL };
                info[5] = { VK_NULL_HANDLE, viewOf(vr.svgfMoments[q]),   VK_IMAGE_LAYOUT_GENERAL };
                info[6] = { VK_NULL_HANDLE, viewOf(vr.svgfGeom[q]),      VK_IMAGE_LAYOUT_GENERAL };
                info[7] = { VK_NULL_HANDLE, viewOf(vr.svgfColorHist[p]), VK_IMAGE_LAYOUT_GENERAL };
                info[8] = { VK_NULL_HANDLE, viewOf(vr.svgfMoments[p]),   VK_IMAGE_LAYOUT_GENERAL };
                info[9] = { VK_NULL_HANDLE, viewOf(vr.svgfGeom[p]),      VK_IMAGE_LAYOUT_GENERAL };

                VkWriteDescriptorSet w[10]{};
                for (u32 i = 0; i < 10; ++i)
                {
                    w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    w[i].dstSet          = vr.svgfReprojectDescSet[p];
                    w[i].dstBinding      = i;
                    w[i].descriptorType  = (i < 4) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                   : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    w[i].descriptorCount = 1;
                    w[i].pImageInfo      = &info[i];
                }
                vkUpdateDescriptorSets(device, 10, w, 0, nullptr);
            }
        }

        // Moments sets: per parity p, b0/b1 = colorHist[p]/moments[p] (the reproject's curr output),
        // b2/b3 depth/normal samplers, b4 svgfAtrous[0] (à-trous level-0 input — shared, not ping-ponged).
        if (vr.svgfMomentsDescSet[0] != VK_NULL_HANDLE
            && vr.svgfColorHist[0] && vr.svgfMoments[0] && vr.svgfAtrous[0]
            && targets.GetSceneDepth() && targets.GetSlimNormal())
        {
            const VkImageView depthV  = viewOf(targets.GetSceneDepth());
            const VkImageView normalV = viewOf(targets.GetSlimNormal());
            const VkImageView a0V     = viewOf(vr.svgfAtrous[0]);

            for (u32 p = 0; p < 2; ++p)
            {
                VkDescriptorImageInfo info[5]{};
                info[0] = { VK_NULL_HANDLE, viewOf(vr.svgfColorHist[p]), VK_IMAGE_LAYOUT_GENERAL };
                info[1] = { VK_NULL_HANDLE, viewOf(vr.svgfMoments[p]),   VK_IMAGE_LAYOUT_GENERAL };
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
                    w[i].dstSet = vr.svgfMomentsDescSet[p]; w[i].dstBinding = i;
                    w[i].descriptorType = types[i]; w[i].descriptorCount = 1; w[i].pImageInfo = &info[i];
                }
                vkUpdateDescriptorSets(device, 5, w, 0, nullptr);
            }
        }

        // À-trous sets: per iter parity ip, b0 = svgfAtrous[ip] (in), b3 = svgfAtrous[ip^1] (out),
        // b1/b2 depth/normal samplers, b4 svgfDenoised (final-level output).
        if (vr.svgfAtrousDescSet[0] != VK_NULL_HANDLE
            && vr.svgfAtrous[0] && vr.svgfAtrous[1] && vr.svgfDenoised
            && targets.GetSceneDepth() && targets.GetSlimNormal())
        {
            const VkImageView depthV  = viewOf(targets.GetSceneDepth());
            const VkImageView normalV = viewOf(targets.GetSlimNormal());
            const VkImageView denV    = viewOf(vr.svgfDenoised);

            for (u32 ip = 0; ip < 2; ++ip)
            {
                const u32 op = ip ^ 1u;
                VkDescriptorImageInfo info[5]{};
                info[0] = { VK_NULL_HANDLE, viewOf(vr.svgfAtrous[ip]), VK_IMAGE_LAYOUT_GENERAL };
                info[1] = { m_Sampler, depthV,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[2] = { m_Sampler, normalV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                info[3] = { VK_NULL_HANDLE, viewOf(vr.svgfAtrous[op]), VK_IMAGE_LAYOUT_GENERAL };
                info[4] = { VK_NULL_HANDLE, denV, VK_IMAGE_LAYOUT_GENERAL };

                const VkDescriptorType types[5] = {
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                };
                VkWriteDescriptorSet w[5]{};
                for (u32 i = 0; i < 5; ++i)
                {
                    w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    w[i].dstSet = vr.svgfAtrousDescSet[ip]; w[i].dstBinding = i;
                    w[i].descriptorType = types[i]; w[i].descriptorCount = 1; w[i].pImageInfo = &info[i];
                }
                vkUpdateDescriptorSets(device, 5, w, 0, nullptr);
            }
        }
    }

    RG::ResourceHandle SvgfDenoiser::AddPasses(RG::RenderGraph& rg, const DenoiseInputs& in)
    {
        // Invalid input → ReSTIR produced no DI this frame; return invalid so the GeometryPass skips
        // the read and pbr.frag runs its own light loop.
        if (!in.di.IsValid()) return {};

        ViewResources* vr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!vr || !vr->svgfDenoised) return {};

        const bool enabled = m_Pipeline->GetSystem().GetSvgfSettings().enabled;
        const bool chainReady = m_ReprojectPipeline && m_MomentsPipeline && m_AtrousPipeline
            && vr->svgfReprojectDescSet[0] != VK_NULL_HANDLE
            && vr->svgfMomentsDescSet[0] != VK_NULL_HANDLE
            && vr->svgfAtrousDescSet[0] != VK_NULL_HANDLE
            && vr->svgfColorHist[0] && vr->svgfMoments[0] && vr->svgfAtrous[0] && vr->svgfAtrous[1];
        if (enabled && chainReady)
            return AddDenoiseChain(rg, in);
        if (m_PassthroughPipeline && vr->svgfPassthroughDescSet != VK_NULL_HANDLE)
            return AddPassthroughPass(rg, in);
        return {};
    }

    RG::ResourceHandle SvgfDenoiser::AddDenoiseChain(RG::RenderGraph& rg, const DenoiseInputs& in)
    {
        const SvgfSettings& s = m_Pipeline->GetSystem().GetSvgfSettings();

        SvgfReprojectPC rpc{};
        rpc.alphaColor      = s.alphaColor;
        rpc.alphaMoments    = s.alphaMoments;
        rpc.historyCap      = static_cast<f32>(s.historyCap);
        rpc.depthThreshold  = s.depthThreshold;
        rpc.normalThreshold = s.normalThreshold;

        SvgfMomentsPC mpc{};
        mpc.phiDepth  = s.phiDepth;
        mpc.phiNormal = s.phiNormal;

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 fp       = frameAbs & 1u;                       // reproject/moments curr parity
        const u32 N        = std::max(1u, s.atrousIterations);

        // Import each distinct VkImage at most ONCE per frame — re-importing aliases distinct RG nodes
        // (a known hazard). colorHist[fp], moments[fp], svgfAtrous[0], svgfAtrous[1], svgfDenoised each
        // get exactly one ImportResource; their handles thread forward across the chain so the RG inserts
        // the within-frame barriers. History geom[fp] (reproject b9) stays descriptor-only — cross-frame.
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

        // Reproject — writes colorHist[fp] (integrated color + temporal variance) + moments[fp]. The
        // current-frame inputs (DI/depth/normal/motion) come in through the RG; the curr history images
        // are imported here and their handles (hColor/hMom) thread into the moments read.
        struct ReprojData {
            RG::ResourceHandle di, depth, normal, motion;
            RG::ResourceHandle color, mom;
        };
        RG::ResourceHandle hColor{}, hMom{};
        rg.AddComputePass<ReprojData>(
            "SvgfReproject",
            RG::QueueFamily::AsyncCompute,
            [&, this](ReprojData& data, RG::RenderPassBuilder& builder) {
                data.di = builder.ReadStorageImage(in.di);
                if (in.depth.IsValid())  data.depth  = builder.ReadStorageImage(in.depth);
                if (in.normal.IsValid()) data.normal = builder.ReadStorageImage(in.normal);
                if (in.motion.IsValid()) data.motion = builder.ReadStorageImage(in.motion);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                data.color = importTex(vr->svgfColorHist[fp], "SvgfColorHistCurr");
                data.color = builder.WriteStorageImage(data.color);
                hColor     = data.color;
                data.mom   = importTex(vr->svgfMoments[fp], "SvgfMomentsCurr");
                data.mom   = builder.WriteStorageImage(data.mom);
                hMom       = data.mom;
            },
            [this, rpc](ReprojData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr) return;
                const u32 fa = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u32 parity = fa & 1u;
                const u32 sl     = fa % MAX_FRAMES_IN_FLIGHT;
                if (vr->svgfReprojectDescSet[parity] == VK_NULL_HANDLE) return;

                m_ReprojectPipeline->Bind(cmd);
                VkDescriptorSet sets[2] = { vr->globalDescriptorSet[sl], vr->svgfReprojectDescSet[parity] };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_ReprojectPipeline->GetLayout(), 0, 2, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_ReprojectPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfReprojectPC), &rpc);

                const u32 gx = (vr->width + 7) / 8;
                const u32 gy = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, gx, gy, 1);
            });

        // Moments — reads colorHist[fp] + moments[fp] (threaded hColor/hMom → reproject→moments RAW
        // barrier), writes svgfAtrous[0]. Depth/normal are descriptor-only (already SHADER_READ_ONLY
        // from the reproject's RG reads). svgfAtrous[0] is imported ONCE here; hA0 threads into à-trous.
        struct MomentsData {
            RG::ResourceHandle color, mom, out;
        };
        RG::ResourceHandle hA0{};
        rg.AddComputePass<MomentsData>(
            "SvgfMoments",
            RG::QueueFamily::AsyncCompute,
            [&, this](MomentsData& data, RG::RenderPassBuilder& builder) {
                // GENERAL-preserving reads: colorHist/moments are STORAGE images (imageLoad in the
                // shader), so they must stay GENERAL — ReadStorageImage would transition them to
                // SHADER_READ_ONLY and mismatch the STORAGE_IMAGE descriptor.
                data.color = builder.ReadStorageImageGeneral(hColor);
                data.mom   = builder.ReadStorageImageGeneral(hMom);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                data.out = importTex(vr->svgfAtrous[0], "SvgfAtrous0");
                data.out = builder.WriteStorageImage(data.out);
                hA0      = data.out;
            },
            [this, mpc](MomentsData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr) return;
                const u32 fa = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u32 parity = fa & 1u;
                const u32 sl     = fa % MAX_FRAMES_IN_FLIGHT;
                if (vr->svgfMomentsDescSet[parity] == VK_NULL_HANDLE) return;

                m_MomentsPipeline->Bind(cmd);
                VkDescriptorSet sets[2] = { vr->globalDescriptorSet[sl], vr->svgfMomentsDescSet[parity] };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_MomentsPipeline->GetLayout(), 0, 2, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_MomentsPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfMomentsPC), &mpc);

                const u32 gx = (vr->width + 7) / 8;
                const u32 gy = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, gx, gy, 1);
            });

        // À-trous — N levels ping-ponging svgfAtrous[0]/[1] with a doubling step. hA[2] tracks the live
        // handle per slot: hA[0] starts as the moments output; svgfAtrous[1] is imported ONCE (the first
        // time it is written, i==0). Each level reads hA[in] and writes hA[out] (threaded → per-level
        // RAW barrier). The final level also writes svgfDenoised (imported once → hDen).
        RG::ResourceHandle hA[2] = { hA0, {} };
        RG::ResourceHandle hDen{};
        for (u32 i = 0; i < N; ++i)
        {
            const u32  inPar   = i & 1u;
            const u32  outPar  = inPar ^ 1u;
            const bool isFinal = (i == N - 1);
            const i32  stepSize = 1 << i;

            SvgfAtrousPC apc{};
            apc.stepSize   = stepSize;
            apc.writeFinal = isFinal ? 1 : 0;
            apc.phiColor   = s.phiColor;
            apc.phiNormal  = s.phiNormal;
            apc.phiDepth   = s.phiDepth;

            struct AtrousData {
                RG::ResourceHandle in, out, den;
            };
            rg.AddComputePass<AtrousData>(
                "SvgfAtrous",
                RG::QueueFamily::AsyncCompute,
                [&, this](AtrousData& data, RG::RenderPassBuilder& builder) {
                    data.in = builder.ReadStorageImageGeneral(hA[inPar]);  // STORAGE imageLoad — keep GENERAL

                    ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                    if (!hA[outPar].IsValid())
                        hA[outPar] = importTex(vr->svgfAtrous[outPar], "SvgfAtrousAlt");
                    data.out   = builder.WriteStorageImage(hA[outPar]);
                    hA[outPar] = data.out;

                    if (isFinal)
                    {
                        data.den = importTex(vr->svgfDenoised, "SvgfDenoised");
                        data.den = builder.WriteStorageImage(data.den);
                        hDen     = data.den;
                    }
                },
                [this, apc, inPar](AtrousData&, RG::RenderPassContext& ctx) {
                    VkCommandBuffer cmd = ctx.commandBuffer;
                    ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                    if (!vr) return;
                    const u32 sl = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex())
                                 % MAX_FRAMES_IN_FLIGHT;
                    if (vr->svgfAtrousDescSet[inPar] == VK_NULL_HANDLE) return;

                    m_AtrousPipeline->Bind(cmd);
                    VkDescriptorSet sets[2] = { vr->globalDescriptorSet[sl], vr->svgfAtrousDescSet[inPar] };
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        m_AtrousPipeline->GetLayout(), 0, 2, sets, 0, nullptr);
                    vkCmdPushConstants(cmd, m_AtrousPipeline->GetLayout(),
                        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfAtrousPC), &apc);

                    const u32 gx = (vr->width + 7) / 8;
                    const u32 gy = (vr->height + 7) / 8;
                    vkCmdDispatch(cmd, gx, gy, 1);
                });
        }

        return hDen;
    }

    RG::ResourceHandle SvgfDenoiser::AddPassthroughPass(RG::RenderGraph& rg, const DenoiseInputs& in)
    {
        struct PassData {
            RG::ResourceHandle di;
            RG::ResourceHandle out;
        };
        RG::ResourceHandle outHandle{};
        rg.AddComputePass<PassData>(
            "SvgfPassthrough",
            RG::QueueFamily::AsyncCompute,
            [&, this](PassData& data, RG::RenderPassBuilder& builder) {
                data.di = builder.ReadStorageImage(in.di);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                auto outTex = std::static_pointer_cast<VKTexture>(vr->svgfDenoised);
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
                if (!vr || vr->svgfPassthroughDescSet == VK_NULL_HANDLE) return;

                m_PassthroughPipeline->Bind(cmd);
                VkDescriptorSet sets[1] = { vr->svgfPassthroughDescSet };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_PassthroughPipeline->GetLayout(), 0, 1, sets, 0, nullptr);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });
        return outHandle;
    }
}
