#include "luthpch.h"
#include "luth/renderer/subsystems/ReflectionsSubsystem.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/settings/ReflectionsSettings.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/core/FrameData.h"

namespace Luth
{
    bool ReflectionsSubsystem::IsEnabled() const
    {
        return m_Pipeline && m_Pipeline->GetSystem().GetReflectionsSettings().enabled;
    }

    void ReflectionsSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear clamp-to-edge — same shape as the ReSTIR passes' SceneDepth / SlimNormal sampler.
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_Sampler);

        // Set 2 (pass-local) — b0 reflection output (storage image, GENERAL), b1 depth, b2 slim oct-normal,
        // b3 slim roughness (combined image samplers, SHADER_READ_ONLY). All stable per-view (written once
        // at WriteView). S0's stub uses b0 only; the real trace (S2) reads b1-b3.
        VkDescriptorSetLayoutBinding bindings[4]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        for (u32 i = 1; i < 4; ++i)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutCI.bindingCount = 4;
        layoutCI.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_SetLayout);

        if (auto sh = ShaderLibrary::LoadEngine("shaders/rt_reflections.comp"))
            m_Spv = sh->GetSpirV();
        if (m_Spv.empty())
        {
            LH_CORE_ERROR("ReflectionsSubsystem: failed to load rt_reflections.comp SPIR-V");
            return;
        }

        // Sets: 0 = global (UBO b0 + IBL b1-b3 + TLAS b6), 1 = light SSBO, 2 = pass-local, 3 = Material
        // SSBO, 4 = bindless textures. Mirrors path_trace.comp so S2's hit-surface material fetch + IBL
        // ambient drop in without a layout change. S0's stub references only Set 2 b0.
        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
            MaterialSystem::GetDescriptorSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
        };
        m_ReflPipeline = std::make_unique<VKComputePipeline>(
            m_Spv, layouts, std::vector<VkPushConstantRange>{});
    }

    void ReflectionsSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_ReflPipeline.reset();
        if (m_Sampler)   vkDestroySampler(device, m_Sampler, nullptr);
        if (m_SetLayout) vkDestroyDescriptorSetLayout(device, m_SetLayout, nullptr);
        m_Sampler   = VK_NULL_HANDLE;
        m_SetLayout = VK_NULL_HANDLE;
        m_Spv.clear();
        m_Pipeline = nullptr;
    }

    bool ReflectionsSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        if (m_SetLayout == VK_NULL_HANDLE || !m_Pipeline) return false;
        if (name != "rt_reflections.comp") return false;

        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
            MaterialSystem::GetDescriptorSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
        };
        m_Spv = spv;
        if (auto* raw = m_ReflPipeline.release(); raw)
            VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        m_ReflPipeline = std::make_unique<VKComputePipeline>(
            m_Spv, layouts, std::vector<VkPushConstantRange>{});
        return true;
    }

    void ReflectionsSubsystem::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.reflDescSet == VK_NULL_HANDLE || !vr.reflRadiance) return;
        if (!targets.GetSceneDepth() || !targets.GetSlimNormal() || !targets.GetSlimRoughness()) return;

        VkDescriptorImageInfo reflInfo{};
        reflInfo.imageView   = std::static_pointer_cast<VKTexture>(vr.reflRadiance)->GetImageView();
        reflInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler     = m_Sampler;
        depthInfo.imageView   = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth())->GetImageView();
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo normalInfo{};
        normalInfo.sampler     = m_Sampler;
        normalInfo.imageView   = std::static_pointer_cast<VKTexture>(targets.GetSlimNormal())->GetImageView();
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo roughInfo{};
        roughInfo.sampler     = m_Sampler;
        roughInfo.imageView   = std::static_pointer_cast<VKTexture>(targets.GetSlimRoughness())->GetImageView();
        roughInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[4]{};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet = vr.reflDescSet; writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          writes[0].descriptorCount = 1; writes[0].pImageInfo = &reflInfo;
        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet = vr.reflDescSet; writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].descriptorCount = 1; writes[1].pImageInfo = &depthInfo;
        writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[2].dstSet = vr.reflDescSet; writes[2].dstBinding = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[2].descriptorCount = 1; writes[2].pImageInfo = &normalInfo;
        writes[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[3].dstSet = vr.reflDescSet; writes[3].dstBinding = 3;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[3].descriptorCount = 1; writes[3].pImageInfo = &roughInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 4, writes, 0, nullptr);
    }

    RG::ResourceHandle ReflectionsSubsystem::AddPasses(RG::RenderGraph& rg,
                                                       RG::ResourceHandle sceneDepth,
                                                       RG::ResourceHandle slimNormal,
                                                       RG::ResourceHandle slimRoughness)
    {
        if (!IsEnabled() || !m_ReflPipeline) return {};
        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || !preflightVr->reflRadiance || preflightVr->reflDescSet == VK_NULL_HANDLE) return {};
        if (m_Pipeline->GetRt().GetTlas() == VK_NULL_HANDLE) return {};

        struct ReflData { RG::ResourceHandle refl; RG::ResourceHandle depth; RG::ResourceHandle normal; RG::ResourceHandle rough; };
        RG::ResourceHandle reflHandle{};
        rg.AddComputePass<ReflData>(
            "RtReflections",
            RG::QueueFamily::AsyncCompute,
            [&, this](ReflData& data, RG::RenderPassBuilder& builder) {
                ViewResources* v = m_Pipeline->GetCurrentViewResources();

                // Slim G-buffer reads — barrier ordering only (the stub ignores them; S2 samples b1-b3).
                if (sceneDepth.IsValid())    data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid())    data.normal = builder.ReadStorageImage(slimNormal);
                if (slimRoughness.IsValid()) data.rough  = builder.ReadStorageImage(slimRoughness);

                // Reflection output — fully overwritten each frame (every pixel: reflection or env
                // fallback), so Undefined import (restirGiDI pattern; no cross-frame read → no clear).
                auto reflTex = std::static_pointer_cast<VKTexture>(v->reflRadiance);
                RG::TextureDesc desc;
                desc.name   = "Reflections";
                desc.width  = reflTex->GetWidth();
                desc.height = reflTex->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;
                data.refl = rg.ImportResource(desc,
                    (void*)reflTex->GetImage(), (void*)reflTex->GetImageView(),
                    RG::ResourceState::Undefined);
                data.refl = builder.WriteStorageImage(data.refl);
                reflHandle = data.refl;

                // No RG consumer until S4 composites it into pbr.frag — keep the pass from being
                // dead-pass-culled so the seam runs + validates (NamedTexture "Reflections" inspects it).
                builder.SetHasSideEffect();
            },
            [this](ReflData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  v   = m_Pipeline->GetCurrentViewResources();
                if (!v || v->reflDescSet == VK_NULL_HANDLE) return;

                // AS-build → AS-read barrier. dstStageMask is COMPUTE_SHADER (NOT RAY_TRACING) —
                // rayQuery executes in the compute stage; a RAY_TRACING dst here is a TDR trap.
                VkMemoryBarrier2 asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                asBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                asBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                VkDependencyInfo asDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                asDep.memoryBarrierCount = 1;
                asDep.pMemoryBarriers    = &asBarrier;
                vkCmdPipelineBarrier2(cmd, &asDep);

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_ReflPipeline->Bind(cmd);
                VkDescriptorSet sets[5] = {
                    v->globalDescriptorSet[slot],
                    v->lightDescSet[slot],
                    v->reflDescSet,
                    MaterialSystem::GetDescriptorSet(slot),
                    VulkanContext::Get().GetBindlessSet().GetSet(),
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_ReflPipeline->GetLayout(), 0, 5, sets, 0, nullptr);

                const u32 groupX = (v->width + 7) / 8;
                const u32 groupY = (v->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        return reflHandle;
    }
}
