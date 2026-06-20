#include "luthpch.h"
#include "luth/renderer/subsystems/ReflectionsSubsystem.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/settings/ReflectionsSettings.h"
#include "luth/renderer/settings/SvgfSettings.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/core/FrameData.h"
#include "luth/core/types/LuthMath.h"

namespace Luth
{
    namespace {
        // Reflection trace push constants. Mirrors rt_reflections.comp's push_constant block; the fixed
        // 128 B range leaves tail headroom so growing this never touches the pipeline layout.
        struct ReflPC {
            Mat4 invViewProj;
            u32  frameSeed;
            f32  roughnessCutoff;   // skip the trace above this (rough → prefiltered-env IBL fallback)
            f32  maxRayDistance;
            f32  fireflyClamp;
            u32  envReady;          // 1 → IBL prefiltered env bound
            i32  gbufferScale;      // 1 = full-res; 2 = half-res (G-buffer reads remap to full)
            i32  dispatchW;         // reflection working (dispatch) resolution
            i32  dispatchH;
            u64  geomTableBDA;      // secondary-hit material fetch (paired with the bound TLAS)
        };
        static_assert(sizeof(ReflPC) == 104, "ReflPC must match rt_reflections.comp push_constant");
        constexpr u32 k_ReflPCSize = 128;   // fixed range — tail headroom

        // Mirrors bilateral_upscale.comp's push_constant (shared with the GI/DI upscale).
        struct ReflUpscalePC {
            i32 fullW;
            i32 fullH;
            i32 halfW;
            i32 halfH;
            f32 phiDepth;
            f32 phiNormal;
        };
        static_assert(sizeof(ReflUpscalePC) == 24, "ReflUpscalePC must match bilateral_upscale.comp push_constant");
    }

    bool ReflectionsSubsystem::IsEnabled() const
    {
        return m_Pipeline && m_Pipeline->GetSystem().GetReflectionsSettings().enabled;
    }

    void ReflectionsSubsystem::Init(RenderPipeline& pipeline)
    {
        LH_PROFILE_FUNCTION();
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

        if (auto sh = ShaderLibrary::LoadEngine("shaders/rt_reflections.slang"))
            m_Spv = sh->GetSpirV();
        if (m_Spv.empty())
        {
            LH_CORE_ERROR("ReflectionsSubsystem: failed to load rt_reflections.slang SPIR-V");
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
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, k_ReflPCSize };
        m_ReflPipeline = std::make_unique<VKComputePipeline>(
            m_Spv, layouts, std::vector<VkPushConstantRange>{ pcRange });

        // Half-res reflections bilateral-upscale pipeline (shared bilateral_upscale.comp). Set 0 = global
        // UBO; Set 1 = b0 half-res signal sampler, b1 depth sampler, b2 normal sampler, b3 full-res storage.
        {
            VkDescriptorSetLayoutBinding ub[4]{};
            ub[0].binding = 0; ub[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ub[0].descriptorCount = 1; ub[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            ub[1].binding = 1; ub[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ub[1].descriptorCount = 1; ub[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            ub[2].binding = 2; ub[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ub[2].descriptorCount = 1; ub[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            ub[3].binding = 3; ub[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          ub[3].descriptorCount = 1; ub[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo uci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            uci.bindingCount = 4; uci.pBindings = ub;
            vkCreateDescriptorSetLayout(device, &uci, nullptr, &m_UpscaleSetLayout);

            if (auto sh = ShaderLibrary::LoadEngine("shaders/bilateral_upscale.comp")) m_UpscaleSpv = sh->GetSpirV();
            if (!m_UpscaleSpv.empty())
            {
                const std::vector<VkDescriptorSetLayout> ulayouts = {
                    m_Pipeline->GetGlobal().GetSetLayout(),
                    m_UpscaleSetLayout,
                };
                VkPushConstantRange upc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReflUpscalePC) };
                m_UpscalePipeline = std::make_unique<VKComputePipeline>(
                    m_UpscaleSpv, ulayouts, std::vector<VkPushConstantRange>{ upc });
            }
        }
    }

    void ReflectionsSubsystem::Shutdown()
    {
        LH_PROFILE_FUNCTION();
        VkDevice device = VulkanContext::Get().GetDevice();
        m_ReflPipeline.reset();
        m_UpscalePipeline.reset();
        if (m_Sampler)          vkDestroySampler(device, m_Sampler, nullptr);
        if (m_SetLayout)        vkDestroyDescriptorSetLayout(device, m_SetLayout, nullptr);
        if (m_UpscaleSetLayout) vkDestroyDescriptorSetLayout(device, m_UpscaleSetLayout, nullptr);
        m_Sampler          = VK_NULL_HANDLE;
        m_SetLayout        = VK_NULL_HANDLE;
        m_UpscaleSetLayout = VK_NULL_HANDLE;
        m_Spv.clear();
        m_UpscaleSpv.clear();
        m_Pipeline = nullptr;
    }

    bool ReflectionsSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        LH_PROFILE_FUNCTION();
        if (!m_Pipeline) return false;

        if (name == "rt_reflections.slang" && m_SetLayout != VK_NULL_HANDLE)
        {
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
            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, k_ReflPCSize };
            m_ReflPipeline = std::make_unique<VKComputePipeline>(
                m_Spv, layouts, std::vector<VkPushConstantRange>{ pcRange });
            return true;
        }

        // Shared with the GI/DI upscale loaders — the reload dispatch's || short-circuit rebuilds only the
        // first matching subsystem; a restart picks up all three (known watch-item).
        if (name == "bilateral_upscale.comp" && m_UpscaleSetLayout != VK_NULL_HANDLE)
        {
            m_UpscaleSpv = spv;
            if (auto* raw = m_UpscalePipeline.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
            const std::vector<VkDescriptorSetLayout> ulayouts = {
                m_Pipeline->GetGlobal().GetSetLayout(), m_UpscaleSetLayout };
            VkPushConstantRange upc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReflUpscalePC) };
            m_UpscalePipeline = std::make_unique<VKComputePipeline>(
                m_UpscaleSpv, ulayouts, std::vector<VkPushConstantRange>{ upc });
            return true;
        }
        return false;
    }

    void ReflectionsSubsystem::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        LH_PROFILE_FUNCTION();
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
        LH_PROFILE_FUNCTION();
        if (!IsEnabled() || !m_ReflPipeline) return {};
        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || !preflightVr->reflRadiance || preflightVr->reflDescSet == VK_NULL_HANDLE) return {};
        if (m_Pipeline->GetRt().GetTlas() == VK_NULL_HANDLE) return {};

        // Reflection working resolution (half when ReflectionsSettings::halfResolution) — derive from
        // reflRadiance's extent (the alloc-time source of truth); G-buffer reads remap to full in-shader.
        auto reflTex0 = std::static_pointer_cast<VKTexture>(preflightVr->reflRadiance);
        const i32 reflW = reflTex0 ? static_cast<i32>(reflTex0->GetWidth())  : static_cast<i32>(preflightVr->width);
        const i32 reflH = reflTex0 ? static_cast<i32>(reflTex0->GetHeight()) : static_cast<i32>(preflightVr->height);
        const i32 reflScale = ((u32)reflW == preflightVr->width && (u32)reflH == preflightVr->height) ? 1 : 2;

        const ReflectionsSettings& s = m_Pipeline->GetSystem().GetReflectionsSettings();
        ReflPC pc{};
        pc.invViewProj     = Math::Inverse(m_Pipeline->GetGlobal().GetCachedViewProj());
        pc.frameSeed       = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        pc.roughnessCutoff = s.roughnessFadeEnd;   // skip above the fade band; pbr.frag blends within it
        pc.maxRayDistance  = s.maxRayDistance;
        pc.fireflyClamp    = s.fireflyClamp;
        pc.envReady        = m_Pipeline->GetLighting().IsIBLReady() ? 1u : 0u;
        pc.gbufferScale    = reflScale;
        pc.dispatchW       = reflW;
        pc.dispatchH       = reflH;
        // Geometry-table BDA paired with the bound TLAS at preflight (same m_LastResult Set 0 b6 binds).
        pc.geomTableBDA    = m_Pipeline->GetRt().GetGeometryTableBDA();

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
            [this, pc](ReflData&, RG::RenderPassContext& ctx) {
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
                vkCmdPushConstants(cmd, m_ReflPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReflPC), &pc);

                const u32 groupX = (static_cast<u32>(pc.dispatchW) + 7) / 8;
                const u32 groupY = (static_cast<u32>(pc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        return reflHandle;
    }

    void ReflectionsSubsystem::WriteUpscaleView(ViewResources& vr, FrameTargets& targets)
    {
        LH_PROFILE_FUNCTION();
        if (vr.reflUpscaleDescSet == VK_NULL_HANDLE) return;
        if (!vr.svgfSpecHalf || !vr.svgfSpecDenoised || !targets.GetSceneDepth() || !targets.GetSlimNormal()) return;

        VkDescriptorImageInfo halfInfo{ m_Sampler,
            std::static_pointer_cast<VKTexture>(vr.svgfSpecHalf)->GetImageView(), VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo depthInfo{ m_Sampler,
            std::static_pointer_cast<VKTexture>(targets.GetSceneDepth())->GetImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo normalInfo{ m_Sampler,
            std::static_pointer_cast<VKTexture>(targets.GetSlimNormal())->GetImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE,
            std::static_pointer_cast<VKTexture>(vr.svgfSpecDenoised)->GetImageView(), VK_IMAGE_LAYOUT_GENERAL };

        VkWriteDescriptorSet w[4]{};
        for (u32 i = 0; i < 4; ++i)
        {
            w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[i].dstSet = vr.reflUpscaleDescSet; w[i].dstBinding = i; w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &halfInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &depthInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &normalInfo;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          w[3].pImageInfo = &outInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 4, w, 0, nullptr);
    }

    RG::ResourceHandle ReflectionsSubsystem::AddUpscalePass(RG::RenderGraph& rg, RG::ResourceHandle reflHalf,
                                                            RG::ResourceHandle sceneDepth, RG::ResourceHandle slimNormal)
    {
        LH_PROFILE_FUNCTION();
        if (!m_UpscalePipeline || !reflHalf.IsValid()) return reflHalf;
        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || preflightVr->reflUpscaleDescSet == VK_NULL_HANDLE || !preflightVr->svgfSpecDenoised)
            return reflHalf;

        struct UpData { RG::ResourceHandle half, depth, normal, out; };
        RG::ResourceHandle outHandle{};
        rg.AddComputePass<UpData>(
            "ReflUpscale",
            RG::QueueFamily::AsyncCompute,
            [&, this](UpData& data, RG::RenderPassBuilder& builder) {
                data.half = builder.ReadStorageImageGeneral(reflHalf);  // svgfSpecHalf stays GENERAL (atrous imageStore)
                if (sceneDepth.IsValid()) data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid()) data.normal = builder.ReadStorageImage(slimNormal);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                auto outTex = std::static_pointer_cast<VKTexture>(vr->svgfSpecDenoised);
                RG::TextureDesc desc;
                desc.name   = "SvgfSpecDenoised";
                desc.width  = outTex->GetWidth();
                desc.height = outTex->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;
                data.out = rg.ImportResource(desc, (void*)outTex->GetImage(), (void*)outTex->GetImageView(),
                                             RG::ResourceState::Undefined);
                data.out  = builder.WriteStorageImage(data.out);
                outHandle = data.out;
            },
            [this](UpData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->reflUpscaleDescSet == VK_NULL_HANDLE || !vr->svgfSpecDenoised || !vr->svgfSpecHalf) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_UpscalePipeline->Bind(cmd);
                VkDescriptorSet sets[2] = { vr->globalDescriptorSet[slot], vr->reflUpscaleDescSet };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_UpscalePipeline->GetLayout(), 0, 2, sets, 0, nullptr);

                auto full = std::static_pointer_cast<VKTexture>(vr->svgfSpecDenoised);
                auto half = std::static_pointer_cast<VKTexture>(vr->svgfSpecHalf);
                const SvgfSettings& ss = m_Pipeline->GetSystem().GetSvgfSpecSettings();
                ReflUpscalePC pc{};
                pc.fullW = (i32)full->GetWidth();  pc.fullH = (i32)full->GetHeight();
                pc.halfW = (i32)half->GetWidth();  pc.halfH = (i32)half->GetHeight();
                pc.phiDepth  = ss.depthThreshold;
                pc.phiNormal = 32.0f;
                vkCmdPushConstants(cmd, m_UpscalePipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReflUpscalePC), &pc);

                const u32 gx = (full->GetWidth()  + 7) / 8;
                const u32 gy = (full->GetHeight() + 7) / 8;
                vkCmdDispatch(cmd, gx, gy, 1);
            });
        return outHandle;
    }
}
