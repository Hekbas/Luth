#include "luthpch.h"
#include "luth/renderer/subsystems/PostProcessSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/core/FrameData.h"
#include "luth/core/time/Time.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

namespace Luth
{
    // Mirrors the GLSL push_constant block in taa_resolve.frag. Source-side de-jitter now lives
    // in slim_gbuffer.frag (ubo.taaParams.zw + ubo.prevJitter), so the resolve no longer carries
    // a jitter delta — just the temporal feedback weight.
    struct TaaResolvePushConstants
    {
        f32 temporalAlpha;
    };
    static_assert(sizeof(TaaResolvePushConstants) == 4,
                  "TaaResolvePushConstants must match taa_resolve.frag's push_constant block");

    void PostProcessSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter    = VK_FILTER_LINEAR;
        samplerInfo.minFilter    = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        vkCreateSampler(device, &samplerInfo, nullptr, &m_Sampler);

        // Nearest sampler for the slim G-buffer matID binding (R16_UINT — integer formats lack
        // SAMPLED_IMAGE_FILTER_LINEAR_BIT, so binding the LINEAR m_Sampler trips VUID 04553).
        VkSamplerCreateInfo nearestInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        nearestInfo.magFilter    = VK_FILTER_NEAREST;
        nearestInfo.minFilter    = VK_FILTER_NEAREST;
        nearestInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        nearestInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        nearestInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        nearestInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &nearestInfo, nullptr, &m_NearestSampler);

        // Bloom extract / blur / composite descriptor layout:
        //   binding 0 = sampler2D (primary), binding 1 = sampler2D (secondary), binding 2 = UBO.
        VkDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        // invariant: binding 2 (PP UBO) is rewritten per render-stage; cycling alone
        // doesn't avoid the in-pending-cmdbuf race in practice. UAB needed.
        VkDescriptorBindingFlags bindingFlags[3] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsCI.bindingCount  = 3;
        bindingFlagsCI.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.pNext        = &bindingFlagsCI;
        layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescSetLayout);

        // Slim viz descriptor set layout — 4 sampler bindings (normal/roughness/motion/matID).
        // Stable per-view; written once at AllocateViewResources time. No UAB needed since the
        // slim attachment views only change on resize (which destroys + recreates the descPool).
        VkDescriptorSetLayoutBinding slimBindings[4] = {};
        for (u32 i = 0; i < 4; ++i)
        {
            slimBindings[i].binding         = i;
            slimBindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            slimBindings[i].descriptorCount = 1;
            slimBindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo slimLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slimLayoutInfo.bindingCount = 4;
        slimLayoutInfo.pBindings    = slimBindings;
        vkCreateDescriptorSetLayout(device, &slimLayoutInfo, nullptr, &m_SlimVizDescSetLayout);

        // TAA Resolve descriptor set layout (Karis14 YCoCg-clip recipe).
        //   0 = sceneColor (sampler2D, current HDR after volumetric composite)
        //   1 = motion vectors (sampler2D, RG16F NDC delta from SlimGBufferPass)
        //   2 = history-prev (sampler2D, cycled UAB — parity-picked taaHistoryA/B each frame)
        //   3 = sceneDepth (sampler2D, for closest-depth velocity dilation in resolve)
        //   4 = PP UBO (shared with bloom/composite sets — rewritten per render-stage)
        // All bindings UAB so binding 2's per-frame parity-rewrite is race-safe.
        VkDescriptorSetLayoutBinding taaBindings[5] = {};
        for (u32 i = 0; i < 4; ++i)
        {
            taaBindings[i].binding         = i;
            taaBindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            taaBindings[i].descriptorCount = 1;
            taaBindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        taaBindings[4].binding         = 4;
        taaBindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        taaBindings[4].descriptorCount = 1;
        taaBindings[4].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorBindingFlags taaFlags[5] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo taaFlagsCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        taaFlagsCI.bindingCount  = 5;
        taaFlagsCI.pBindingFlags = taaFlags;
        VkDescriptorSetLayoutCreateInfo taaLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        taaLayoutInfo.pNext        = &taaFlagsCI;
        taaLayoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        taaLayoutInfo.bindingCount = 5;
        taaLayoutInfo.pBindings    = taaBindings;
        vkCreateDescriptorSetLayout(device, &taaLayoutInfo, nullptr, &m_TaaResolveDescSetLayout);

        auto loadSpv = [](const char* relPath) -> std::vector<u32> {
            auto sh = ShaderLibrary::LoadEngine(relPath);
            return sh ? sh->GetSpirV() : std::vector<u32>{};
        };
        m_FullscreenVertSpv   = loadSpv("shaders/fullscreen.vert");
        m_BloomExtractFragSpv = loadSpv("shaders/bloomExtract.frag");
        m_BloomBlurFragSpv    = loadSpv("shaders/bloomBlur.frag");
        m_PostProcessFragSpv  = loadSpv("shaders/postprocess.frag");
        m_SlimVizFragSpv      = loadSpv("shaders/slim_viz.frag");
        m_TaaResolveFragSpv   = loadSpv("shaders/taa_resolve.frag");

        if (m_FullscreenVertSpv.empty() || m_BloomExtractFragSpv.empty() ||
            m_BloomBlurFragSpv.empty() || m_PostProcessFragSpv.empty() ||
            m_SlimVizFragSpv.empty() || m_TaaResolveFragSpv.empty())
        {
            LH_CORE_ERROR("PostProcessSubsystem: shader SPIR-V empty after asset load!");
            return;
        }

        BuildPipelines();
    }

    void PostProcessSubsystem::BuildPipelines()
    {
        std::vector<VkDescriptorSetLayout> ppLayouts = { m_DescSetLayout };

        if (!m_BloomExtractFragSpv.empty())
        {
            VkPushConstantRange bloomExtractPC{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4 };
            PipelineConfig cfg;
            cfg.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
            cfg.depthFormat  = VK_FORMAT_UNDEFINED;
            cfg.depthTest    = false; cfg.depthWrite = false;
            cfg.blendEnabled = false;
            cfg.cullMode     = VK_CULL_MODE_NONE;
            cfg.pushConstantRanges = { bloomExtractPC };
            m_BloomExtractPipeline = std::make_unique<VKPipeline>(
                cfg, m_FullscreenVertSpv, m_BloomExtractFragSpv, ppLayouts);
        }
        if (!m_BloomBlurFragSpv.empty())
        {
            VkPushConstantRange bloomBlurPC{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4 };
            PipelineConfig cfg;
            cfg.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
            cfg.depthFormat  = VK_FORMAT_UNDEFINED;
            cfg.depthTest    = false; cfg.depthWrite = false;
            cfg.blendEnabled = false;
            cfg.cullMode     = VK_CULL_MODE_NONE;
            cfg.pushConstantRanges = { bloomBlurPC };
            m_BloomBlurPipeline = std::make_unique<VKPipeline>(
                cfg, m_FullscreenVertSpv, m_BloomBlurFragSpv, ppLayouts);
        }
        if (!m_PostProcessFragSpv.empty())
        {
            PipelineConfig cfg;
            cfg.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
            cfg.depthFormat  = VK_FORMAT_UNDEFINED;
            cfg.depthTest    = false; cfg.depthWrite = false;
            cfg.blendEnabled = false;
            cfg.cullMode     = VK_CULL_MODE_NONE;
            m_PostProcessPipeline = std::make_unique<VKPipeline>(
                cfg, m_FullscreenVertSpv, m_PostProcessFragSpv, ppLayouts);
        }

        // Slim G-buffer viz pipeline (live ShadeMode toggle). Push constants: mode + scale = 8B.
        if (!m_SlimVizFragSpv.empty())
        {
            std::vector<VkDescriptorSetLayout> slimLayouts = { m_SlimVizDescSetLayout };
            VkPushConstantRange slimPC{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(u32) + sizeof(float) };
            PipelineConfig cfg;
            cfg.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
            cfg.depthFormat  = VK_FORMAT_UNDEFINED;
            cfg.depthTest    = false; cfg.depthWrite = false;
            cfg.blendEnabled = false;
            cfg.cullMode     = VK_CULL_MODE_NONE;
            cfg.pushConstantRanges = { slimPC };
            m_SlimVizPipeline = std::make_unique<VKPipeline>(
                cfg, m_FullscreenVertSpv, m_SlimVizFragSpv, slimLayouts);
        }

        // TAA Resolve pipeline. Output to RGBA16F (HDR history texture); push constant carries
        // temporalAlpha (jitter delta moved to slim_gbuffer.frag as source-side de-jitter).
        // No depth, no blend — opaque write.
        if (!m_TaaResolveFragSpv.empty())
        {
            std::vector<VkDescriptorSetLayout> taaLayouts = { m_TaaResolveDescSetLayout };
            VkPushConstantRange taaPC{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TaaResolvePushConstants) };
            PipelineConfig cfg;
            cfg.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
            cfg.depthFormat  = VK_FORMAT_UNDEFINED;
            cfg.depthTest    = false; cfg.depthWrite = false;
            cfg.blendEnabled = false;
            cfg.cullMode     = VK_CULL_MODE_NONE;
            cfg.pushConstantRanges = { taaPC };
            m_TaaResolvePipeline = std::make_unique<VKPipeline>(
                cfg, m_FullscreenVertSpv, m_TaaResolveFragSpv, taaLayouts);
        }
    }

    void PostProcessSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_TaaResolvePipeline.reset();
        m_SlimVizPipeline.reset();
        m_PostProcessPipeline.reset();
        m_BloomBlurPipeline.reset();
        m_BloomExtractPipeline.reset();
        if (m_Sampler)              { vkDestroySampler(device, m_Sampler, nullptr); m_Sampler = VK_NULL_HANDLE; }
        if (m_NearestSampler)       { vkDestroySampler(device, m_NearestSampler, nullptr); m_NearestSampler = VK_NULL_HANDLE; }
        if (m_DescSetLayout)           { vkDestroyDescriptorSetLayout(device, m_DescSetLayout, nullptr); m_DescSetLayout = VK_NULL_HANDLE; }
        if (m_SlimVizDescSetLayout)    { vkDestroyDescriptorSetLayout(device, m_SlimVizDescSetLayout, nullptr); m_SlimVizDescSetLayout = VK_NULL_HANDLE; }
        if (m_TaaResolveDescSetLayout) { vkDestroyDescriptorSetLayout(device, m_TaaResolveDescSetLayout, nullptr); m_TaaResolveDescSetLayout = VK_NULL_HANDLE; }
    }

    bool PostProcessSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        auto deferGfx = [](std::unique_ptr<VKPipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if      (name == "fullscreen.vert")    m_FullscreenVertSpv   = spv;
        else if (name == "bloomExtract.frag")  m_BloomExtractFragSpv = spv;
        else if (name == "bloomBlur.frag")     m_BloomBlurFragSpv    = spv;
        else if (name == "postprocess.frag")   m_PostProcessFragSpv  = spv;
        else if (name == "slim_viz.frag")      m_SlimVizFragSpv      = spv;
        else if (name == "taa_resolve.frag")   m_TaaResolveFragSpv   = spv;
        else return false;

        deferGfx(m_BloomExtractPipeline);
        deferGfx(m_BloomBlurPipeline);
        deferGfx(m_PostProcessPipeline);
        deferGfx(m_SlimVizPipeline);
        deferGfx(m_TaaResolvePipeline);
        BuildPipelines();
        // For fullscreen.vert, return false so the orchestrator also rebuilds Outline + Grid
        // (they share the same vertex shader). PostProcess pipelines are already rebuilt above.
        return name != "fullscreen.vert";
    }

    void PostProcessSubsystem::UpdateUBO()
    {
        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!vr || vr->compositeDescSet[0] == VK_NULL_HANDLE) return;

        const auto& s = m_Pipeline->GetSystem().GetPostProcessSettings();
        PostProcessUBO ubo{};
        ubo.bloomThreshold      = s.bloomThreshold;
        ubo.bloomStrength       = s.bloomStrength;
        ubo.exposure            = s.exposure;
        ubo.contrast            = s.contrast;
        ubo.saturation          = s.saturation;
        ubo.tonemapOp           = static_cast<int>(s.tonemapOp);
        ubo.vignetteAmount      = s.vignetteAmount;
        ubo.vignetteHardness    = s.vignetteHardness;
        ubo.grainAmount         = s.grainAmount;
        ubo.sharpness           = s.sharpness;
        ubo.chromaticAberration = s.chromaticAberration;
        ubo.time                = Time::GetTime();
        ubo.shadowBalance       = s.shadowBalance;
        ubo.midtoneBalance      = s.midtoneBalance;
        ubo.highlightBalance    = s.highlightBalance;

        // invariant: all 4 PP sets share one tagged-heap region AND the same per-frame slot.
        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
        jobCtx->GpuCache.CurrentTag = frameAbs;

        auto& heap   = Memory::GPUTaggedPageAllocator::Get();
        const u64 al = VulkanContext::Get().GetMinUniformBufferAlignment();
        Memory::GPUSubRegion region = heap.Allocate(jobCtx->GpuCache, sizeof(PostProcessUBO), al);
        if (!region.buffer) return;

        memcpy(region.mappedPtr, &ubo, sizeof(PostProcessUBO));
        heap.FlushRegion(region);

        VkDescriptorBufferInfo bi{};
        bi.buffer = region.buffer;
        bi.offset = region.offset;
        bi.range  = region.size;

        const VkDescriptorSet ppSets[4] = {
            vr->bloomExtractDescSet[slot],
            vr->bloomBlurHDescSet[slot],
            vr->bloomBlurVDescSet[slot],
            vr->compositeDescSet[slot],
        };
        VkWriteDescriptorSet writes[4] = {};
        u32 n = 0;
        for (u32 i = 0; i < 4; ++i)
        {
            if (ppSets[i] == VK_NULL_HANDLE) continue;
            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = ppSets[i];
            writes[n].dstBinding      = 2;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[n].descriptorCount = 1;
            writes[n].pBufferInfo     = &bi;
            ++n;
        }
        if (n > 0) vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), n, writes, 0, nullptr);
    }

    void PostProcessSubsystem::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.compositeDescSet[0] == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        auto sceneVk  = std::static_pointer_cast<VKTexture>(targets.GetSceneColor());
        auto bloomAVk = std::static_pointer_cast<VKTexture>(vr.bloomA);
        auto bloomBVk = std::static_pointer_cast<VKTexture>(vr.bloomB);

        auto makeImg = [&](VkImageView v) {
            VkDescriptorImageInfo info{};
            info.sampler     = m_Sampler;
            info.imageView   = v;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return info;
        };

        VkDescriptorImageInfo bloomExtractImg0 = makeImg(sceneVk->GetImageView());
        VkDescriptorImageInfo bloomExtractImg1 = makeImg(bloomAVk->GetImageView());
        VkDescriptorImageInfo blurHImg0        = makeImg(bloomAVk->GetImageView());
        VkDescriptorImageInfo blurHImg1        = makeImg(bloomBVk->GetImageView());
        VkDescriptorImageInfo blurVImg0        = makeImg(bloomBVk->GetImageView());
        VkDescriptorImageInfo blurVImg1        = makeImg(bloomAVk->GetImageView());
        VkDescriptorImageInfo compImg0         = makeImg(sceneVk->GetImageView());
        VkDescriptorImageInfo compImg1         = makeImg(bloomAVk->GetImageView());

        // 8 stable bindings (b0+b1 of each of 4 sets) propagated to every cycled slot.
        VkWriteDescriptorSet writes[8 * MAX_FRAMES_IN_FLIGHT] = {};
        u32 idx = 0;
        auto addImg = [&](VkDescriptorSet set, u32 binding, VkDescriptorImageInfo* imgInfo) {
            writes[idx] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[idx].dstSet          = set;
            writes[idx].dstBinding      = binding;
            writes[idx].descriptorCount = 1;
            writes[idx].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[idx].pImageInfo      = imgInfo;
            ++idx;
        };

        for (u32 s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
        {
            addImg(vr.bloomExtractDescSet[s], 0, &bloomExtractImg0);
            addImg(vr.bloomExtractDescSet[s], 1, &bloomExtractImg1);
            addImg(vr.bloomBlurHDescSet[s],   0, &blurHImg0);
            addImg(vr.bloomBlurHDescSet[s],   1, &blurHImg1);
            addImg(vr.bloomBlurVDescSet[s],   0, &blurVImg0);
            addImg(vr.bloomBlurVDescSet[s],   1, &blurVImg1);
            addImg(vr.compositeDescSet[s],    0, &compImg0);
            addImg(vr.compositeDescSet[s],    1, &compImg1);
        }

        vkUpdateDescriptorSets(device, idx, writes, 0, nullptr);

        // Slim viz set — 4 stable bindings into the per-view slim attachments. Written once
        // per resize. Binding 3 (R16_UINT matID) uses m_NearestSampler — integer formats don't
        // support LINEAR filtering (VUID 04553).
        if (vr.slimVizDescSet != VK_NULL_HANDLE)
        {
            auto slimN = std::static_pointer_cast<VKTexture>(targets.GetSlimNormal());
            auto slimR = std::static_pointer_cast<VKTexture>(targets.GetSlimRoughness());
            auto slimM = std::static_pointer_cast<VKTexture>(targets.GetSlimMotion());
            auto slimID = std::static_pointer_cast<VKTexture>(targets.GetSlimMaterialID());
            if (slimN && slimR && slimM && slimID)
            {
                auto makeImgWithSampler = [](VkImageView v, VkSampler s) {
                    VkDescriptorImageInfo info{};
                    info.sampler     = s;
                    info.imageView   = v;
                    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    return info;
                };
                VkDescriptorImageInfo slimInfos[4] = {
                    makeImgWithSampler(slimN->GetImageView(),  m_Sampler),
                    makeImgWithSampler(slimR->GetImageView(),  m_Sampler),
                    makeImgWithSampler(slimM->GetImageView(),  m_Sampler),
                    makeImgWithSampler(slimID->GetImageView(), m_NearestSampler),
                };
                VkWriteDescriptorSet slimWrites[4] = {};
                for (u32 b = 0; b < 4; ++b)
                {
                    slimWrites[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    slimWrites[b].dstSet          = vr.slimVizDescSet;
                    slimWrites[b].dstBinding      = b;
                    slimWrites[b].descriptorCount = 1;
                    slimWrites[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    slimWrites[b].pImageInfo      = &slimInfos[b];
                }
                vkUpdateDescriptorSets(device, 4, slimWrites, 0, nullptr);
            }
        }
    }

    RG::ResourceHandle PostProcessSubsystem::AddBloomPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneColor)
    {
        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!m_BloomExtractPipeline || !m_BloomBlurPipeline || !vr->bloomA || !vr->bloomB)
            return {};

        struct BloomPassData {
            RG::ResourceHandle output;
            RG::ResourceHandle input;
        };

        u32 halfW = vr->bloomA->GetWidth();
        u32 halfH = vr->bloomA->GetHeight();
        auto bloomAVk = std::static_pointer_cast<VKTexture>(vr->bloomA);
        auto bloomBVk = std::static_pointer_cast<VKTexture>(vr->bloomB);

        // BloomExtract: SceneColor → BloomA.
        RG::ResourceHandle bloomAHandle;
        rg.AddPass<BloomPassData>("BloomExtract",
            [&](BloomPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "BloomA";
                desc.width  = halfW;
                desc.height = halfH;
                desc.format = RG::TextureFormat::RGBA16_Float;

                data.output = rg.ImportResource(desc,
                    (void*)bloomAVk->GetImage(), (void*)bloomAVk->GetImageView(),
                    RG::ResourceState::Undefined);
                data.output = builder.Write(data.output);
                data.input  = builder.Read(sceneColor);
                bloomAHandle = data.output;
            },
            [this, halfW, halfH](BloomPassData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "BloomExtract", "BloomA", false,
                    { "bloomExtract", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkCommandBuffer cmd = ctx.commandBuffer;
                m_BloomExtractPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_BloomExtractPipeline->GetLayout(), 0, 1, &vr->bloomExtractDescSet[slot], 0, nullptr);

                VkViewport vp{}; vp.width = (float)halfW; vp.height = (float)halfH; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { halfW, halfH };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                float pc[4] = { sys.GetPostProcessSettings().bloomThreshold, 0, 0, 0 };
                vkCmdPushConstants(cmd, m_BloomExtractPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("BloomExtract", "FullscreenTriangle", "BloomExtract", 0, 0, dummyPC,
                    { "bloomExtract", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );

        // BloomBlurH: BloomA → BloomB.
        RG::ResourceHandle bloomBHandle;
        rg.AddPass<BloomPassData>("BloomBlurH",
            [&](BloomPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "BloomB";
                desc.width  = halfW;
                desc.height = halfH;
                desc.format = RG::TextureFormat::RGBA16_Float;

                data.output = rg.ImportResource(desc,
                    (void*)bloomBVk->GetImage(), (void*)bloomBVk->GetImageView(),
                    RG::ResourceState::Undefined);
                data.output = builder.Write(data.output);
                data.input  = builder.Read(bloomAHandle);
                bloomBHandle = data.output;
            },
            [this, halfW, halfH](BloomPassData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "BloomBlurH", "BloomB", false,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkCommandBuffer cmd = ctx.commandBuffer;
                m_BloomBlurPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_BloomBlurPipeline->GetLayout(), 0, 1, &vr->bloomBlurHDescSet[slot], 0, nullptr);

                VkViewport vp{}; vp.width = (float)halfW; vp.height = (float)halfH; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { halfW, halfH };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                float pc[4] = { 1.0f / (float)halfW, 0.0f, 0.0f, 0.0f };
                vkCmdPushConstants(cmd, m_BloomBlurPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("BloomBlurH", "FullscreenTriangle", "BloomBlurH", 0, 0, dummyPC,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );

        // BloomBlurV: BloomB → BloomA (re-imported as BloomAFinal).
        RG::ResourceHandle finalBloomHandle;
        rg.AddPass<BloomPassData>("BloomBlurV",
            [&](BloomPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "BloomAFinal";
                desc.width  = halfW;
                desc.height = halfH;
                desc.format = RG::TextureFormat::RGBA16_Float;

                data.output = rg.ImportResource(desc,
                    (void*)bloomAVk->GetImage(), (void*)bloomAVk->GetImageView(),
                    RG::ResourceState::Undefined);
                data.output = builder.Write(data.output);
                data.input  = builder.Read(bloomBHandle);
                finalBloomHandle = data.output;
            },
            [this, halfW, halfH](BloomPassData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "BloomBlurV", "BloomAFinal", false,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkCommandBuffer cmd = ctx.commandBuffer;
                m_BloomBlurPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_BloomBlurPipeline->GetLayout(), 0, 1, &vr->bloomBlurVDescSet[slot], 0, nullptr);

                VkViewport vp{}; vp.width = (float)halfW; vp.height = (float)halfH; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { halfW, halfH };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                float pc[4] = { 0.0f, 1.0f / (float)halfH, 0.0f, 0.0f };
                vkCmdPushConstants(cmd, m_BloomBlurPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("BloomBlurV", "FullscreenTriangle", "BloomBlurV", 0, 0, dummyPC,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );

        return finalBloomHandle;
    }

    RG::ResourceHandle PostProcessSubsystem::AddCompositePass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle bloomResult)
    {
        const auto* view = m_Pipeline->GetCurrentView();
        if (!m_PostProcessPipeline || !view->targets->GetLDROutput())
            return sceneColor;

        struct PostProcessPassData {
            RG::ResourceHandle output;
            RG::ResourceHandle hdrInput;
            RG::ResourceHandle bloomInput;
        };
        RG::ResourceHandle outputHandle;
        auto ldrVk = std::static_pointer_cast<VKTexture>(view->targets->GetLDROutput());

        rg.AddPass<PostProcessPassData>("PostProcess",
            [&, sceneColor, bloomResult, ldrVk](PostProcessPassData& data, RG::RenderPassBuilder& builder)
            {
                const auto* v = m_Pipeline->GetCurrentView();
                RG::TextureDesc desc;
                desc.name   = "LDROutput";
                desc.width  = v->targets->GetLDROutput()->GetWidth();
                desc.height = v->targets->GetLDROutput()->GetHeight();
                desc.format = RG::TextureFormat::RGBA8_Unorm;

                data.output = rg.ImportResource(desc,
                    (void*)ldrVk->GetImage(), (void*)ldrVk->GetImageView(),
                    RG::ResourceState::ShaderResource);
                data.output = builder.Write(data.output);

                data.hdrInput = builder.Read(sceneColor);
                if (bloomResult.IsValid())
                    data.bloomInput = builder.Read(bloomResult);

                outputHandle = data.output;
            },
            [this](PostProcessPassData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                const auto* v = m_Pipeline->GetCurrentView();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "PostProcess", "LDROutput", false,
                    { "postprocess", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkCommandBuffer cmd = ctx.commandBuffer;
                m_PostProcessPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_PostProcessPipeline->GetLayout(), 0, 1, &vr->compositeDescSet[slot], 0, nullptr);

                u32 w = v->targets->GetLDROutput()->GetWidth();
                u32 h = v->targets->GetLDROutput()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("PostProcess", "FullscreenTriangle", "PostProcess", 0, 0, dummyPC,
                    { "postprocess", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );
        return outputHandle;
    }

    RG::ResourceHandle PostProcessSubsystem::AddSlimVizPass(RG::RenderGraph& rg, RG::ResourceHandle ldrInput,
                                                            const SlimGBufferOutput& slimGB, u32 mode, float scale)
    {
        const auto* view = m_Pipeline->GetCurrentView();
        if (!m_SlimVizPipeline || !view->targets->GetLDROutput())
            return ldrInput;

        struct SlimVizPassData {
            RG::ResourceHandle output;
            RG::ResourceHandle slimNormal;
            RG::ResourceHandle slimRoughness;
            RG::ResourceHandle slimMotion;
            RG::ResourceHandle slimMaterialID;
        };
        RG::ResourceHandle outputHandle;

        rg.AddPass<SlimVizPassData>("SlimVizPass",
            [&, ldrInput, mode, slimGB](SlimVizPassData& data, RG::RenderPassBuilder& builder)
            {
                // Write to the LDR handle Composite returned — same RG resource node, so the
                // barrier solver sees the producer's COLOR_ATTACHMENT state. Re-importing would
                // alias the same VkImage onto a fresh node with a stale initialState (VUID 01197).
                VkClearValue clearVal{ { {0.f, 0.f, 0.f, 1.f} } };
                data.output = builder.Write(ldrInput, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, clearVal);

                // Same reasoning for the slim attachments — reuse SlimGBufferPass's handles.
                data.slimNormal     = builder.Read(slimGB.normal);
                data.slimRoughness  = builder.Read(slimGB.roughness);
                data.slimMotion     = builder.Read(slimGB.motion);
                data.slimMaterialID = builder.Read(slimGB.materialID);

                outputHandle = data.output;
            },
            [this, mode, scale](SlimVizPassData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                const auto* v = m_Pipeline->GetCurrentView();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->slimVizDescSet == VK_NULL_HANDLE) return;

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "SlimVizPass", "LDROutput", false,
                    { "slim_viz", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_SlimVizPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_SlimVizPipeline->GetLayout(), 0, 1, &vr->slimVizDescSet, 0, nullptr);

                struct { u32 mode; float scale; } pcData{ mode, scale };
                vkCmdPushConstants(cmd, m_SlimVizPipeline->GetLayout(),
                                   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcData), &pcData);

                u32 w = v->targets->GetLDROutput()->GetWidth();
                u32 h = v->targets->GetLDROutput()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("SlimVizPass", "FullscreenTriangle", "SlimViz", 0, 0, dummyPC,
                    { "slim_viz", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );
        return outputHandle;
    }

    void PostProcessSubsystem::WriteTaaResolveView(ViewResources& vr, FrameTargets& targets)
    {
        // Bindings 0/1/3 are stable per-view-resize — write once across all cycled slots.
        // Binding 2 (history-prev sampler) cycles per-frame in WriteTaaResolvePerFrame.
        // Binding 4 (UBO) is declared in the layout but unused by the current shader.
        if (vr.taaResolveDescSet[0] == VK_NULL_HANDLE) return;

        auto sceneTex  = std::static_pointer_cast<VKTexture>(targets.GetSceneColor());
        auto motionTex = std::static_pointer_cast<VKTexture>(targets.GetSlimMotion());
        auto depthTex  = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth());
        if (!sceneTex || !motionTex || !depthTex) return;

        auto makeImg = [&](VkImageView v) {
            VkDescriptorImageInfo info{};
            info.sampler     = m_Sampler;
            info.imageView   = v;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return info;
        };
        VkDescriptorImageInfo sceneInfo  = makeImg(sceneTex->GetImageView());
        VkDescriptorImageInfo motionInfo = makeImg(motionTex->GetImageView());
        // Depth: aspect is DEPTH only; layout is DEPTH_STENCIL_READ_ONLY_OPTIMAL once the prepass
        // completes. SHADER_READ_ONLY_OPTIMAL works as well because the RG will transition into
        // a read-compatible state when the pass declares the depth Read.
        VkDescriptorImageInfo depthInfo  = makeImg(depthTex->GetImageView());

        VkWriteDescriptorSet writes[3 * MAX_FRAMES_IN_FLIGHT] = {};
        u32 idx = 0;
        auto addImg = [&](VkDescriptorSet set, u32 binding, VkDescriptorImageInfo* info) {
            writes[idx] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[idx].dstSet          = set;
            writes[idx].dstBinding      = binding;
            writes[idx].descriptorCount = 1;
            writes[idx].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[idx].pImageInfo      = info;
            ++idx;
        };
        for (u32 s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
        {
            addImg(vr.taaResolveDescSet[s], 0, &sceneInfo);
            addImg(vr.taaResolveDescSet[s], 1, &motionInfo);
            addImg(vr.taaResolveDescSet[s], 3, &depthInfo);
        }
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), idx, writes, 0, nullptr);
    }

    void PostProcessSubsystem::UpdateBloomCompositeInput(ViewResources& vr, FrameTargets& targets, u32 frameAbs)
    {
        const u32 slot = frameAbs % MAX_FRAMES_IN_FLIGHT;
        if (vr.bloomExtractDescSet[slot] == VK_NULL_HANDLE || vr.compositeDescSet[slot] == VK_NULL_HANDLE)
            return;

        const auto& pps  = m_Pipeline->GetSystem().GetPostProcessSettings();
        // Path-traced reference mode (rt-renderer C.5) takes priority: bloom + composite sample the PT
        // display image (ptColor), the megakernel's HDR output, in place of the raster sceneColor / TAA.
        const bool ptOn  = m_Pipeline->GetSystem().GetRenderMode() == RenderMode::PathTrace && vr.ptColor;
        const bool taaOn = !ptOn && pps.taaEnabled && vr.taaHistoryA && vr.taaHistoryB;

        VkImageView srcView = VK_NULL_HANDLE;
        if (ptOn)
        {
            srcView = std::static_pointer_cast<VKTexture>(vr.ptColor)->GetImageView();
        }
        else if (taaOn)
        {
            // Parity rule matches AddTaaResolvePass — parity=0 writes taaHistoryA, =1 writes B.
            // Bloom + grid + composite all read the same VkImage; RG inserts barriers so bloom
            // sees the pre-grid version and composite sees the post-grid version.
            const bool parity = (frameAbs & 1u) != 0u;
            auto historyCurr = parity ? vr.taaHistoryB : vr.taaHistoryA;
            srcView = std::static_pointer_cast<VKTexture>(historyCurr)->GetImageView();
        }
        else
        {
            auto sceneTex = std::static_pointer_cast<VKTexture>(targets.GetSceneColor());
            if (!sceneTex) return;
            srcView = sceneTex->GetImageView();
        }

        VkDescriptorImageInfo info{};
        info.sampler     = m_Sampler;
        info.imageView   = srcView;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2] = {};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr.bloomExtractDescSet[slot];
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &info;
        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = vr.compositeDescSet[slot];
        writes[1].dstBinding      = 0;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &info;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 2, writes, 0, nullptr);
    }

    void PostProcessSubsystem::WriteTaaResolvePerFrame(ViewResources& vr, u32 frameAbs)
    {
        // Binding 2 = history-prev sampler. Parity-pick: even frame reads HistA + writes HistB;
        // odd frame reads HistB + writes HistA. The write target is bound as a color attachment
        // via the RG (not in this descriptor set), so we only rebind the READ side here.
        if (!vr.taaHistoryA || !vr.taaHistoryB) return;
        const u32 slot = frameAbs % MAX_FRAMES_IN_FLIGHT;
        if (vr.taaResolveDescSet[slot] == VK_NULL_HANDLE) return;

        const bool parity = (frameAbs & 1u) != 0u;
        auto vkPrev = std::static_pointer_cast<VKTexture>(parity ? vr.taaHistoryA : vr.taaHistoryB);

        VkDescriptorImageInfo prevInfo{};
        prevInfo.sampler     = m_Sampler;
        prevInfo.imageView   = vkPrev->GetImageView();
        prevInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vr.taaResolveDescSet[slot];
        write.dstBinding      = 2;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &prevInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);
    }

    RG::ResourceHandle PostProcessSubsystem::AddTaaResolvePass(
        RG::RenderGraph& rg, RG::ResourceHandle sceneColor,
        RG::ResourceHandle motion, RG::ResourceHandle sceneDepth)
    {
        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!m_TaaResolvePipeline || !vr || !vr->taaHistoryA || !vr->taaHistoryB)
            return sceneColor;

        const u64 frameAbs = Renderer::GetFrameData()->GetRenderFrameIndex();
        const bool parity  = (frameAbs & 1u) != 0u;
        // Output target: opposite of what WriteTaaResolvePerFrame picked as "prev".
        auto historyCurrTex = parity ? vr->taaHistoryB : vr->taaHistoryA;
        auto historyCurrVk  = std::static_pointer_cast<VKTexture>(historyCurrTex);
        const u32 w = historyCurrTex->GetWidth();
        const u32 h = historyCurrTex->GetHeight();

        struct TaaResolvePassData {
            RG::ResourceHandle output;
            RG::ResourceHandle current;
            RG::ResourceHandle motion;
            RG::ResourceHandle depth;
        };
        RG::ResourceHandle outHandle;

        rg.AddPass<TaaResolvePassData>("TaaResolve",
            [&, sceneColor, motion, sceneDepth, historyCurrVk, w, h](TaaResolvePassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "TaaCurrent";
                desc.width  = w;
                desc.height = h;
                desc.format = RG::TextureFormat::RGBA16_Float;

                data.output = rg.ImportResource(desc,
                    (void*)historyCurrVk->GetImage(), (void*)historyCurrVk->GetImageView(),
                    RG::ResourceState::Undefined);
                data.output  = builder.Write(data.output, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                             VK_ATTACHMENT_STORE_OP_STORE);
                data.current = builder.Read(sceneColor);
                data.motion  = builder.Read(motion);
                data.depth   = builder.Read(sceneDepth);
                outHandle    = data.output;
            },
            [this, w, h](TaaResolvePassData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                if (!vr) return;

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "TaaResolve", "TaaCurrent", false,
                    { "taa_resolve", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkCommandBuffer cmd = ctx.commandBuffer;
                m_TaaResolvePipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_TaaResolvePipeline->GetLayout(), 0, 1, &vr->taaResolveDescSet[slot], 0, nullptr);

                // Source-side de-jitter lives in slim_gbuffer.frag — the motion attachment carries
                // pure scene displacement, so the resolve push constant is just the feedback weight.
                TaaResolvePushConstants pc{};
                pc.temporalAlpha = sys.GetPostProcessSettings().taaTemporalAlpha;
                vkCmdPushConstants(cmd, m_TaaResolvePipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("TaaResolve", "FullscreenTriangle", "TaaResolve", 0, 0, dummyPC,
                    { "taa_resolve", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );
        return outHandle;
    }
}
