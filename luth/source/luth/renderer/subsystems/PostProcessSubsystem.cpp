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

    // Mirrors bloom_downsample.slang's push_constant. prefilter=1 gates the threshold + Karis
    // bright-pass on the scene->mip0 step; later mips run the plain 13-tap.
    struct BloomDownPC
    {
        Vec2  srcTexel;   // 0  — 1/sourceResolution
        IVec2 dstSize;    // 8  — dest extent
        f32   threshold;  // 16
        f32   knee;       // 20
        u32   prefilter;  // 24
        u32   _pad;       // 28
    };
    static_assert(sizeof(BloomDownPC) == 32, "BloomDownPC must match bloom_downsample.slang");

    // Mirrors bloom_upsample.slang's push_constant. radius scales the tent spread (scatter).
    struct BloomUpPC
    {
        Vec2  srcTexel;            // 0
        IVec2 dstSize;             // 8
        f32   radius;              // 16
        f32   _pad0, _pad1, _pad2; // 20-31
    };
    static_assert(sizeof(BloomUpPC) == 32, "BloomUpPC must match bloom_upsample.slang");

    void PostProcessSubsystem::Init(RenderPipeline& pipeline)
    {
        LH_PROFILE_FUNCTION();
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

        // Composite descriptor layout:
        //   binding 0 = sampler2D (HDR), binding 1 = sampler2D (bloom mip0), binding 2 = UBO.
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

        // Bloom pyramid compute layout: b0 = source mip (COMBINED_IMAGE_SAMPLER), b1 = dest mip
        // (STORAGE_IMAGE), both COMPUTE. b0 is UAB so the prefilter's per-frame source rebind
        // (UpdateBloomCompositeInput) is race-safe; the single down/up sets bind stable per-view mips.
        VkDescriptorSetLayoutBinding bloomBindings[2] = {};
        bloomBindings[0].binding         = 0;
        bloomBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bloomBindings[0].descriptorCount = 1;
        bloomBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bloomBindings[1].binding         = 1;
        bloomBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bloomBindings[1].descriptorCount = 1;
        bloomBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorBindingFlags bloomFlags[2] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bloomFlagsCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bloomFlagsCI.bindingCount  = 2;
        bloomFlagsCI.pBindingFlags = bloomFlags;
        VkDescriptorSetLayoutCreateInfo bloomLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        bloomLayoutInfo.pNext        = &bloomFlagsCI;
        bloomLayoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        bloomLayoutInfo.bindingCount = 2;
        bloomLayoutInfo.pBindings    = bloomBindings;
        vkCreateDescriptorSetLayout(device, &bloomLayoutInfo, nullptr, &m_BloomComputeLayout);

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
        m_BloomDownSpv        = loadSpv("shaders/bloom_downsample.slang");
        m_BloomUpSpv          = loadSpv("shaders/bloom_upsample.slang");
        m_PostProcessFragSpv  = loadSpv("shaders/postprocess.frag");
        m_SlimVizFragSpv      = loadSpv("shaders/slim_viz.frag");
        m_TaaResolveFragSpv   = loadSpv("shaders/taa_resolve.frag");

        if (m_FullscreenVertSpv.empty() || m_BloomDownSpv.empty() ||
            m_BloomUpSpv.empty() || m_PostProcessFragSpv.empty() ||
            m_SlimVizFragSpv.empty() || m_TaaResolveFragSpv.empty())
        {
            LH_LOG(Renderer, error, "PostProcessSubsystem: shader SPIR-V empty after asset load!");
            return;
        }

        BuildPipelines();
    }

    void PostProcessSubsystem::BuildPipelines()
    {
        LH_PROFILE_FUNCTION();
        std::vector<VkDescriptorSetLayout> ppLayouts = { m_DescSetLayout };

        // Bloom pyramid compute pipelines (shared 2-binding layout). Downsample doubles as the
        // prefilter via its push-constant flag; upsample tent-blends additively back up.
        std::vector<VkDescriptorSetLayout> bloomLayouts = { m_BloomComputeLayout };
        if (!m_BloomDownSpv.empty())
        {
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BloomDownPC) };
            m_BloomDownPipeline = std::make_unique<VKComputePipeline>(
                m_BloomDownSpv, bloomLayouts, std::vector<VkPushConstantRange>{ pc });
        }
        if (!m_BloomUpSpv.empty())
        {
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BloomUpPC) };
            m_BloomUpPipeline = std::make_unique<VKComputePipeline>(
                m_BloomUpSpv, bloomLayouts, std::vector<VkPushConstantRange>{ pc });
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
        LH_PROFILE_FUNCTION();
        VkDevice device = VulkanContext::Get().GetDevice();
        m_TaaResolvePipeline.reset();
        m_SlimVizPipeline.reset();
        m_PostProcessPipeline.reset();
        m_BloomUpPipeline.reset();
        m_BloomDownPipeline.reset();
        if (m_Sampler)              { vkDestroySampler(device, m_Sampler, nullptr); m_Sampler = VK_NULL_HANDLE; }
        if (m_NearestSampler)       { vkDestroySampler(device, m_NearestSampler, nullptr); m_NearestSampler = VK_NULL_HANDLE; }
        if (m_DescSetLayout)           { vkDestroyDescriptorSetLayout(device, m_DescSetLayout, nullptr); m_DescSetLayout = VK_NULL_HANDLE; }
        if (m_BloomComputeLayout)      { vkDestroyDescriptorSetLayout(device, m_BloomComputeLayout, nullptr); m_BloomComputeLayout = VK_NULL_HANDLE; }
        if (m_SlimVizDescSetLayout)    { vkDestroyDescriptorSetLayout(device, m_SlimVizDescSetLayout, nullptr); m_SlimVizDescSetLayout = VK_NULL_HANDLE; }
        if (m_TaaResolveDescSetLayout) { vkDestroyDescriptorSetLayout(device, m_TaaResolveDescSetLayout, nullptr); m_TaaResolveDescSetLayout = VK_NULL_HANDLE; }
    }

    bool PostProcessSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        LH_PROFILE_FUNCTION();
        auto deferGfx = [](std::unique_ptr<VKPipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };
        auto deferComp = [](std::unique_ptr<VKComputePipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if      (name == "fullscreen.vert")        m_FullscreenVertSpv  = spv;
        else if (name == "bloom_downsample.slang") m_BloomDownSpv       = spv;
        else if (name == "bloom_upsample.slang")   m_BloomUpSpv         = spv;
        else if (name == "postprocess.frag")       m_PostProcessFragSpv = spv;
        else if (name == "slim_viz.frag")          m_SlimVizFragSpv     = spv;
        else if (name == "taa_resolve.frag")       m_TaaResolveFragSpv  = spv;
        else return false;

        deferComp(m_BloomDownPipeline);
        deferComp(m_BloomUpPipeline);
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
        LH_PROFILE_FUNCTION();
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

        // invariant: the composite UBO write rides one tagged-heap region against the per-frame slot.
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

        if (vr->compositeDescSet[slot] == VK_NULL_HANDLE) return;
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vr->compositeDescSet[slot];
        write.dstBinding      = 2;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo     = &bi;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);
    }

    void PostProcessSubsystem::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        LH_PROFILE_FUNCTION();
        if (vr.compositeDescSet[0] == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        auto sceneVk = std::static_pointer_cast<VKTexture>(targets.GetSceneColor());
        auto mip0Vk  = std::static_pointer_cast<VKTexture>(vr.bloomMip[0]);

        auto makeImg = [&](VkImageView v) {
            VkDescriptorImageInfo info{};
            info.sampler     = m_Sampler;
            info.imageView   = v;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return info;
        };

        // Composite: b0 = HDR source (default; rebound per frame by UpdateBloomCompositeInput),
        // b1 = bloom pyramid mip0 (the accumulated bloom). Both stable across cycled slots.
        VkDescriptorImageInfo compImg0 = makeImg(sceneVk->GetImageView());
        VkDescriptorImageInfo compImg1 = makeImg(mip0Vk->GetImageView());

        VkWriteDescriptorSet writes[2 * MAX_FRAMES_IN_FLIGHT] = {};
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
            addImg(vr.compositeDescSet[s], 0, &compImg0);
            addImg(vr.compositeDescSet[s], 1, &compImg1);
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

    void PostProcessSubsystem::WriteBloomView(ViewResources& vr)
    {
        LH_PROFILE_FUNCTION();
        if (!vr.bloomMip[0]) return;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Per-mip image infos must outlive the single vkUpdateDescriptorSets — hold them in arrays.
        // sampled[i] = SHADER_READ_ONLY (filtered taps); storage[i] = GENERAL (imageStore dest).
        std::array<VkDescriptorImageInfo, ViewResources::kBloomMipCount> sampled{};
        std::array<VkDescriptorImageInfo, ViewResources::kBloomMipCount> storage{};
        for (u32 i = 0; i < ViewResources::kBloomMipCount; ++i)
        {
            VkImageView view = std::static_pointer_cast<VKTexture>(vr.bloomMip[i])->GetImageView();
            sampled[i].sampler     = m_Sampler;
            sampled[i].imageView   = view;
            sampled[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            storage[i].imageView   = view;
            storage[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }

        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(MAX_FRAMES_IN_FLIGHT + 4 * (ViewResources::kBloomMipCount - 1));
        auto add = [&](VkDescriptorSet set, u32 binding, VkDescriptorType type, const VkDescriptorImageInfo* info) {
            if (set == VK_NULL_HANDLE) return;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet          = set;
            w.dstBinding      = binding;
            w.descriptorCount = 1;
            w.descriptorType  = type;
            w.pImageInfo      = info;
            writes.push_back(w);
        };

        // Prefilter dest (b1 = mip0 storage); b0 source is rebound per frame by UpdateBloomCompositeInput.
        for (u32 s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
            add(vr.bloomPrefilterDescSet[s], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &storage[0]);

        for (u32 i = 0; i < ViewResources::kBloomMipCount - 1; ++i)
        {
            // Downsample i: mip[i] (sampled) -> mip[i+1] (storage).
            add(vr.bloomDownDescSet[i], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sampled[i]);
            add(vr.bloomDownDescSet[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &storage[i + 1]);
            // Upsample i: mip[i+1] (sampled) -> mip[i] (storage, additive RMW).
            add(vr.bloomUpDescSet[i],   0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sampled[i + 1]);
            add(vr.bloomUpDescSet[i],   1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &storage[i]);
        }

        if (!writes.empty())
            vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }

    void PostProcessSubsystem::RecordBloomDispatch(RG::RenderPassContext& ctx, VKComputePipeline* pipe,
        VkDescriptorSet set, const void* pc, u32 pcSize, u32 dstW, u32 dstH, const char* label, const char* shader)
    {
        auto& sys = m_Pipeline->GetSystem();
        sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, label, "BloomMip", false,
            { shader, 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });
        if (!pipe || set == VK_NULL_HANDLE) { sys.GetFrameDebugger().EndCapturePass(); return; }

        VkCommandBuffer cmd = ctx.commandBuffer;
        pipe->Bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->GetLayout(), 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipe->GetLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize, pc);
        const u32 gx = (dstW + 7) / 8, gy = (dstH + 7) / 8;
        vkCmdDispatch(cmd, gx, gy, 1);

        sys.GetFrameDebugger().CaptureComputeDispatch(label, shader, gx, gy, 1);
        sys.GetFrameDebugger().EndCapturePass();
    }

    RG::ResourceHandle PostProcessSubsystem::AddBloomPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneColor)
    {
        LH_PROFILE_FUNCTION();
        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!m_BloomDownPipeline || !m_BloomUpPipeline || !vr || !vr->bloomMip[0])
            return {};

        constexpr u32 N = ViewResources::kBloomMipCount;

        // Thread one handle per mip. Each mip is imported exactly once (at its producing pass); re-importing
        // a VkImage an upstream pass already imported aliases it onto two RG nodes with divergent state (arch
        // hazard). h[i] always names mip[i]'s single live node. see arch/rendering-pipeline.md.
        RG::ResourceHandle h[N];
        struct BloomData {};

        auto importMip = [&](u32 mip, RG::RenderPassBuilder& b) -> RG::ResourceHandle {
            auto vk = std::static_pointer_cast<VKTexture>(vr->bloomMip[mip]);
            RG::TextureDesc desc;
            desc.name   = "BloomMip";
            desc.width  = vr->bloomMip[mip]->GetWidth();
            desc.height = vr->bloomMip[mip]->GetHeight();
            desc.format = RG::TextureFormat::RGBA16_Float;
            RG::ResourceHandle handle = rg.ImportResource(desc,
                (void*)vk->GetImage(), (void*)vk->GetImageView(), RG::ResourceState::Undefined);
            return b.WriteStorageImage(handle);
        };

        // Prefilter: full-res scene -> mip0 (threshold + soft-knee + Karis bright-pass).
        rg.AddComputePass<BloomData>("BloomPrefilter",
            [&](BloomData&, RG::RenderPassBuilder& b)
            {
                b.ReadStorageImage(sceneColor);
                h[0] = importMip(0, b);
            },
            [this](BloomData&, RG::RenderPassContext& ctx)
            {
                ViewResources* vr  = m_Pipeline->GetCurrentViewResources();
                const auto*    view = m_Pipeline->GetCurrentView();
                if (!vr || !view || !view->targets->GetSceneColor() || !vr->bloomMip[0]) return;
                const auto& s  = m_Pipeline->GetSystem().GetPostProcessSettings();
                const u32 srcW = view->targets->GetSceneColor()->GetWidth();
                const u32 srcH = view->targets->GetSceneColor()->GetHeight();
                const u32 dstW = vr->bloomMip[0]->GetWidth(), dstH = vr->bloomMip[0]->GetHeight();
                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                BloomDownPC pc{};
                pc.srcTexel  = { 1.0f / float(srcW), 1.0f / float(srcH) };
                pc.dstSize   = { (i32)dstW, (i32)dstH };
                pc.threshold = s.bloomThreshold;
                pc.knee      = 0.5f;
                pc.prefilter = 1u;
                RecordBloomDispatch(ctx, m_BloomDownPipeline.get(), vr->bloomPrefilterDescSet[slot],
                                    &pc, sizeof(pc), dstW, dstH, "BloomPrefilter", "bloom_downsample");
            });

        // Downsample chain: mip[i] -> mip[i+1] (plain 13-tap).
        for (u32 i = 0; i < N - 1; ++i)
        {
            rg.AddComputePass<BloomData>("BloomDown" + std::to_string(i),
                [&, i](BloomData&, RG::RenderPassBuilder& b)
                {
                    b.ReadStorageImage(h[i]);
                    h[i + 1] = importMip(i + 1, b);
                },
                [this, i](BloomData&, RG::RenderPassContext& ctx)
                {
                    ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                    if (!vr || !vr->bloomMip[i + 1]) return;
                    const u32 srcW = vr->bloomMip[i]->GetWidth(),     srcH = vr->bloomMip[i]->GetHeight();
                    const u32 dstW = vr->bloomMip[i + 1]->GetWidth(), dstH = vr->bloomMip[i + 1]->GetHeight();
                    BloomDownPC pc{};
                    pc.srcTexel  = { 1.0f / float(srcW), 1.0f / float(srcH) };
                    pc.dstSize   = { (i32)dstW, (i32)dstH };
                    pc.prefilter = 0u;
                    RecordBloomDispatch(ctx, m_BloomDownPipeline.get(), vr->bloomDownDescSet[i],
                                        &pc, sizeof(pc), dstW, dstH, "BloomDown", "bloom_downsample");
                });
        }

        // Upsample chain: mip[i+1] -> mip[i] (tent + additive accumulation into the existing content).
        const float radius = m_Pipeline->GetSystem().GetPostProcessSettings().bloomRadius;
        for (i32 i = (i32)N - 2; i >= 0; --i)
        {
            rg.AddComputePass<BloomData>("BloomUp" + std::to_string(i),
                [&, i](BloomData&, RG::RenderPassBuilder& b)
                {
                    b.ReadStorageImage(h[i + 1]);            // smaller mip, sampled (tent taps)
                    // Additive RMW: the dest read needs read-visibility for the imageLoad, so declare
                    // ReadStorageImageGeneral + WriteStorageImage on the same node (PathTrace ptAccum
                    // pattern) — WriteStorageImage alone would leave the imageLoad of the downsample
                    // content un-synchronized. Same node, no re-import (arch RG-aliasing hazard).
                    h[i] = b.ReadStorageImageGeneral(h[i]);
                    h[i] = b.WriteStorageImage(h[i]);
                },
                [this, i, radius](BloomData&, RG::RenderPassContext& ctx)
                {
                    ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                    if (!vr || !vr->bloomMip[i + 1]) return;
                    const u32 srcW = vr->bloomMip[i + 1]->GetWidth(), srcH = vr->bloomMip[i + 1]->GetHeight();
                    const u32 dstW = vr->bloomMip[i]->GetWidth(),     dstH = vr->bloomMip[i]->GetHeight();
                    BloomUpPC pc{};
                    pc.srcTexel = { 1.0f / float(srcW), 1.0f / float(srcH) };
                    pc.dstSize  = { (i32)dstW, (i32)dstH };
                    pc.radius   = radius;
                    RecordBloomDispatch(ctx, m_BloomUpPipeline.get(), vr->bloomUpDescSet[i],
                                        &pc, sizeof(pc), dstW, dstH, "BloomUp", "bloom_upsample");
                });
        }

        return h[0];
    }

    RG::ResourceHandle PostProcessSubsystem::AddCompositePass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle bloomResult)
    {
        LH_PROFILE_FUNCTION();
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
        LH_PROFILE_FUNCTION();
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
        LH_PROFILE_FUNCTION();
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
        LH_PROFILE_FUNCTION();
        const u32 slot = frameAbs % MAX_FRAMES_IN_FLIGHT;
        if (vr.bloomPrefilterDescSet[slot] == VK_NULL_HANDLE || vr.compositeDescSet[slot] == VK_NULL_HANDLE)
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
        writes[0].dstSet          = vr.bloomPrefilterDescSet[slot];
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
        LH_PROFILE_FUNCTION();
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
        LH_PROFILE_FUNCTION();
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
