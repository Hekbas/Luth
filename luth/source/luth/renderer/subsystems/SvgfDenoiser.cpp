#include "luthpch.h"
#include "luth/renderer/subsystems/SvgfDenoiser.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/settings/SvgfSettings.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"

namespace Luth
{
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

        // Passthrough set (pass-local): b0 demodulated-DI sampler, b1 output storage image. Both stable
        // per-view, so a single non-cycled set suffices (no per-frame ping-pong yet).
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutCI.bindingCount = 2;
        layoutCI.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_PassLayout);

        if (auto sh = ShaderLibrary::LoadEngine("shaders/svgf_passthrough.comp"))
            m_PassthroughSpv = sh->GetSpirV();
        if (m_PassthroughSpv.empty())
        {
            LH_CORE_ERROR("SvgfDenoiser: failed to load svgf_passthrough.comp SPIR-V");
            return;
        }

        const std::vector<VkDescriptorSetLayout> layouts = { m_PassLayout };
        m_PassthroughPipeline = std::make_unique<VKComputePipeline>(
            m_PassthroughSpv, layouts, std::vector<VkPushConstantRange>{});
    }

    void SvgfDenoiser::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_PassthroughPipeline.reset();
        if (m_Sampler)   vkDestroySampler(device, m_Sampler, nullptr);
        if (m_PassLayout) vkDestroyDescriptorSetLayout(device, m_PassLayout, nullptr);
        m_Sampler   = VK_NULL_HANDLE;
        m_PassLayout = VK_NULL_HANDLE;
        m_PassthroughSpv.clear();
        m_Pipeline = nullptr;
    }

    bool SvgfDenoiser::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        if (m_PassLayout == VK_NULL_HANDLE || !m_Pipeline) return false;
        if (name != "svgf_passthrough.comp") return false;

        // Defer the old pipeline's destruction — a command buffer from a still-in-flight frame may
        // bind it; PushDeletion drains it MAX_FRAMES_IN_FLIGHT frames later (no vkDeviceWaitIdle).
        if (m_PassthroughPipeline)
        {
            VulkanContext::Get().PushDeletion([p = m_PassthroughPipeline.release()]() { delete p; });
        }
        m_PassthroughSpv = spv;
        const std::vector<VkDescriptorSetLayout> layouts = { m_PassLayout };
        m_PassthroughPipeline = std::make_unique<VKComputePipeline>(
            m_PassthroughSpv, layouts, std::vector<VkPushConstantRange>{});
        return true;
    }

    void SvgfDenoiser::AllocateViewSets(ViewResources& vr)
    {
        if (m_PassLayout == VK_NULL_HANDLE || vr.descPool == VK_NULL_HANDLE) return;
        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool     = vr.descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_PassLayout;
        if (vkAllocateDescriptorSets(device, &ai, &vr.svgfPassthroughDescSet) != VK_SUCCESS)
        {
            LH_CORE_ERROR("SvgfDenoiser: AllocateViewSets failed; bump view pool sizes");
            vr.svgfPassthroughDescSet = VK_NULL_HANDLE;
            return;
        }
        VulkanContext::SetDebugName(vr.svgfPassthroughDescSet, "View.SvgfPassthrough");
    }

    void SvgfDenoiser::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        (void)targets;
        if (vr.svgfPassthroughDescSet == VK_NULL_HANDLE) return;
        if (!vr.restirDI || !vr.svgfDenoised) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        // b0 reads the noisy demodulated DI (RestirShade output); the RestirShade write + the
        // pass's Read transition it to SHADER_READ_ONLY before the copy samples it.
        VkDescriptorImageInfo diIn{};
        diIn.sampler     = m_Sampler;
        diIn.imageView   = std::static_pointer_cast<VKTexture>(vr.restirDI)->GetImageView();
        diIn.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // b1 is the denoised output (GENERAL for the imageStore); the GeometryPass Read transitions it
        // to SHADER_READ_ONLY before pbr.frag samples it via Set 3 b5.
        VkDescriptorImageInfo outImg{};
        outImg.imageView   = std::static_pointer_cast<VKTexture>(vr.svgfDenoised)->GetImageView();
        outImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr.svgfPassthroughDescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &diIn;
        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = vr.svgfPassthroughDescSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &outImg;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    RG::ResourceHandle SvgfDenoiser::AddPasses(RG::RenderGraph& rg, const DenoiseInputs& in)
    {
        // Invalid input → ReSTIR produced no DI this frame (disabled / pre-TLAS). Return invalid so the
        // GeometryPass skips the read and pbr.frag runs its own light loop (restirParams.x gate off).
        if (!m_PassthroughPipeline || !in.di.IsValid()) return {};

        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || !preflightVr->svgfDenoised
            || preflightVr->svgfPassthroughDescSet == VK_NULL_HANDLE) return {};

        struct PassData {
            RG::ResourceHandle di;
            RG::ResourceHandle out;
        };
        RG::ResourceHandle outHandle{};
        rg.AddComputePass<PassData>(
            "SvgfPassthrough",
            RG::QueueFamily::AsyncCompute,
            [&, this](PassData& data, RG::RenderPassBuilder& builder) {
                // Read the producer's DI handle (not a re-import) so the RG chains the
                // RestirShade write → denoise read barrier on the same ResourceNode.
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
