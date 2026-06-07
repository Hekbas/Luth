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
        // storage; b10 denoised output storage. History stays GENERAL (storage), so no UAB / per-frame
        // rewrite — the two sets are pre-built per parity and bound by frameAbs & 1.
        {
            VkDescriptorSetLayoutBinding b[11]{};
            for (u32 i = 0; i < 11; ++i)
            {
                b[i].binding         = i;
                b[i].descriptorCount = 1;
                b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
                b[i].descriptorType  = (i < 4) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            }
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 11; ci.pBindings = b;
            vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_ReprojectLayout);
        }

        if (auto sh = ShaderLibrary::LoadEngine("shaders/svgf_passthrough.comp"))
            m_PassthroughSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/svgf_reproject.comp"))
            m_ReprojectSpv = sh->GetSpirV();
        if (m_PassthroughSpv.empty() || m_ReprojectSpv.empty())
        {
            LH_CORE_ERROR("SvgfDenoiser: failed to load svgf_passthrough/reproject.comp SPIR-V");
            return;
        }

        const std::vector<VkDescriptorSetLayout> passLayouts = { m_PassLayout };
        m_PassthroughPipeline = std::make_unique<VKComputePipeline>(
            m_PassthroughSpv, passLayouts, std::vector<VkPushConstantRange>{});

        // Reproject binds Set 0 (global UBO — nearZ/farZ/viewportSize) + the pass-local set.
        const std::vector<VkDescriptorSetLayout> reprojLayouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_ReprojectLayout,
        };
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfReprojectPC) };
        m_ReprojectPipeline = std::make_unique<VKComputePipeline>(
            m_ReprojectSpv, reprojLayouts, std::vector<VkPushConstantRange>{ pcRange });
    }

    void SvgfDenoiser::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_PassthroughPipeline.reset();
        m_ReprojectPipeline.reset();
        if (m_Sampler)         vkDestroySampler(device, m_Sampler, nullptr);
        if (m_PassLayout)      vkDestroyDescriptorSetLayout(device, m_PassLayout, nullptr);
        if (m_ReprojectLayout) vkDestroyDescriptorSetLayout(device, m_ReprojectLayout, nullptr);
        m_Sampler        = VK_NULL_HANDLE;
        m_PassLayout      = VK_NULL_HANDLE;
        m_ReprojectLayout = VK_NULL_HANDLE;
        m_PassthroughSpv.clear();
        m_ReprojectSpv.clear();
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
        if (vr.svgfReprojectDescSet[0] != VK_NULL_HANDLE && vr.restirDI && vr.svgfDenoised
            && vr.svgfColorHist[0] && vr.svgfMoments[0] && vr.svgfGeom[0]
            && targets.GetSceneDepth() && targets.GetSlimNormal() && targets.GetSlimMotion())
        {
            const VkImageView depthV  = viewOf(targets.GetSceneDepth());
            const VkImageView normalV = viewOf(targets.GetSlimNormal());
            const VkImageView motionV = viewOf(targets.GetSlimMotion());
            const VkImageView diV     = viewOf(vr.restirDI);
            const VkImageView denV    = viewOf(vr.svgfDenoised);

            for (u32 p = 0; p < 2; ++p)
            {
                const u32 q = p ^ 1u;  // prev parity
                VkDescriptorImageInfo info[11]{};
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
                info[10]= { VK_NULL_HANDLE, denV,                        VK_IMAGE_LAYOUT_GENERAL };

                VkWriteDescriptorSet w[11]{};
                for (u32 i = 0; i < 11; ++i)
                {
                    w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    w[i].dstSet          = vr.svgfReprojectDescSet[p];
                    w[i].dstBinding      = i;
                    w[i].descriptorType  = (i < 4) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                   : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    w[i].descriptorCount = 1;
                    w[i].pImageInfo      = &info[i];
                }
                vkUpdateDescriptorSets(device, 11, w, 0, nullptr);
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
        if (enabled && m_ReprojectPipeline && vr->svgfReprojectDescSet[0] != VK_NULL_HANDLE)
            return AddReprojectPass(rg, in);
        if (m_PassthroughPipeline && vr->svgfPassthroughDescSet != VK_NULL_HANDLE)
            return AddPassthroughPass(rg, in);
        return {};
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

    RG::ResourceHandle SvgfDenoiser::AddReprojectPass(RG::RenderGraph& rg, const DenoiseInputs& in)
    {
        const SvgfSettings& s = m_Pipeline->GetSystem().GetSvgfSettings();
        SvgfReprojectPC pc{};
        pc.alphaColor      = s.alphaColor;
        pc.alphaMoments    = s.alphaMoments;
        pc.historyCap      = static_cast<f32>(s.historyCap);
        pc.depthThreshold  = s.depthThreshold;
        pc.normalThreshold = s.normalThreshold;

        // History (svgfColorHist/Moments/Geom) is bound via the descriptor set, not RG-tracked: it
        // stays GENERAL across frames and the ping-pong + frame timeline make the prev read safe. Only
        // the cross-pass I/O (current-frame inputs + the denoised output) goes through the RG.
        struct PassData {
            RG::ResourceHandle di;
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::ResourceHandle motion;
            RG::ResourceHandle out;
        };
        RG::ResourceHandle outHandle{};
        rg.AddComputePass<PassData>(
            "SvgfReproject",
            RG::QueueFamily::AsyncCompute,
            [&, this](PassData& data, RG::RenderPassBuilder& builder) {
                data.di = builder.ReadStorageImage(in.di);
                if (in.depth.IsValid())  data.depth  = builder.ReadStorageImage(in.depth);
                if (in.normal.IsValid()) data.normal = builder.ReadStorageImage(in.normal);
                if (in.motion.IsValid()) data.motion = builder.ReadStorageImage(in.motion);

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
            [this, pc](PassData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr) return;

                const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u32 parity   = frameAbs & 1u;
                const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
                if (vr->svgfReprojectDescSet[parity] == VK_NULL_HANDLE) return;

                m_ReprojectPipeline->Bind(cmd);
                VkDescriptorSet sets[2] = {
                    vr->globalDescriptorSet[slot],
                    vr->svgfReprojectDescSet[parity],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_ReprojectPipeline->GetLayout(), 0, 2, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_ReprojectPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SvgfReprojectPC), &pc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });
        return outHandle;
    }
}
