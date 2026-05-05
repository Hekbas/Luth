#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/settings/GTAOSettings.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/core/diagnostics/Log.h"

namespace Luth
{
    // Per-view descriptor pool capacity. 9 sets per view (1 global, 4 PP,
    // 3 GTAO, 1 outline, 1 grid); binding counts sized above the minimum
    // so future binding additions don't force a pool-size revisit.
    static constexpr u32 k_ViewPoolMaxSets              = 16;
    static constexpr u32 k_ViewPoolUniformBufferCount   = 12;
    static constexpr u32 k_ViewPoolStorageImageCount    = 12;
    static constexpr u32 k_ViewPoolCombinedSamplerCount = 32;

    ViewResources& RenderPipeline::EnsureViewResources(FrameTargets& targets)
    {
        auto [it, inserted] = m_ViewResources.try_emplace(&targets);
        ViewResources& vr = it->second;

        if (!targets.GetSceneColor())
            return vr;

        const u32 newW = targets.GetSceneColor()->GetWidth();
        const u32 newH = targets.GetSceneColor()->GetHeight();

        if (inserted)
        {
            AllocateViewResources(vr, targets);
        }
        else if (vr.width != newW || vr.height != newH)
        {
            const u32 halfW = std::max(newW / 2, 1u);
            const u32 halfH = std::max(newH / 2, 1u);
            RecreateViewTextures(vr, halfW, halfH);
            WriteViewPostProcessSets(vr, targets);
            WriteViewGTAOSets(vr, targets);
            WriteViewOutlineSet(vr, targets);
            WriteViewGridSet(vr, targets);
            // Set 0 bindings 4 + 5 reference the recreated GTAO textures.
            WriteViewGlobalSet(vr);
        }

        vr.width  = newW;
        vr.height = newH;
        return vr;
    }

    void RenderPipeline::ReleaseViewResources(FrameTargets& targets)
    {
        auto it = m_ViewResources.find(&targets);
        if (it == m_ViewResources.end()) return;
        DestroyViewResources(it->second);
        m_ViewResources.erase(it);
    }

    void RenderPipeline::AllocateViewResources(ViewResources& vr, FrameTargets& targets)
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorPoolSize poolSizes[3] = {};
        poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = k_ViewPoolUniformBufferCount;
        poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[1].descriptorCount = k_ViewPoolStorageImageCount;
        poolSizes[2].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[2].descriptorCount = k_ViewPoolCombinedSamplerCount;

        // UPDATE_AFTER_BIND for sets that rebind their UBO bindings per render-stage
        // (Global / GTAO Main / PostProcess / Grid) to fresh tagged-heap regions.
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets       = k_ViewPoolMaxSets;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes    = poolSizes;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &vr.descPool);

        // Set 0 UBO bindings (0 + 5) and Grid set binding 0 are written per render-stage
        // by UpdateGlobalUniforms / UpdateGTAOUBO; nothing to allocate up front.

        const u32 halfW = std::max(targets.GetSceneColor()->GetWidth()  / 2, 1u);
        const u32 halfH = std::max(targets.GetSceneColor()->GetHeight() / 2, 1u);
        RecreateViewTextures(vr, halfW, halfH);

        auto alloc = [&](VkDescriptorSetLayout layout, VkDescriptorSet& outSet) {
            if (layout == VK_NULL_HANDLE) return;
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool     = vr.descPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &layout;
            vkAllocateDescriptorSets(device, &ai, &outSet);
        };

        alloc(m_GlobalSetLayout,          vr.globalDescriptorSet);
        alloc(m_PPDescSetLayout,          vr.bloomExtractDescSet);
        alloc(m_PPDescSetLayout,          vr.bloomBlurHDescSet);
        alloc(m_PPDescSetLayout,          vr.bloomBlurVDescSet);
        alloc(m_PPDescSetLayout,          vr.compositeDescSet);
        alloc(m_GTAOPrefilterDescLayout,  vr.gtaoPrefilterDescSet);
        alloc(m_GTAOMainDescLayout,       vr.gtaoMainDescSet);
        alloc(m_GTAODenoiseDescLayout,    vr.gtaoDenoiseDescSet);
        alloc(m_OutlineDescSetLayout,     vr.outlineDescSet);
        alloc(m_GridDescSetLayout,        vr.gridDescSet);

        WriteViewGlobalSet(vr);
        WriteViewPostProcessSets(vr, targets);
        WriteViewGTAOSets(vr, targets);
        WriteViewOutlineSet(vr, targets);
        WriteViewGridSet(vr, targets);
    }

    void RenderPipeline::RecreateViewTextures(ViewResources& vr, u32 halfW, u32 halfH)
    {
        vr.bloomA = Texture::Create(halfW, halfH, TextureFormat::RGBA16F);
        vr.bloomB = Texture::Create(halfW, halfH, TextureFormat::RGBA16F);

        auto makeStorage = [&](TextureFormat fmt) {
            return std::make_shared<VKTexture>(
                halfW, halfH, fmt,
                /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
                VK_IMAGE_USAGE_STORAGE_BIT);
        };
        vr.gtaoLinearDepth = makeStorage(TextureFormat::R32_Float);
        vr.gtaoRawAO       = makeStorage(TextureFormat::R8);
        vr.gtaoEdges       = makeStorage(TextureFormat::R8);
        vr.gtaoFinal       = makeStorage(TextureFormat::R8);
    }

    void RenderPipeline::WriteViewGlobalSet(ViewResources& vr)
    {
        if (vr.globalDescriptorSet == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        // Stable bindings only: 1-3 IBL, 4 GTAO final sampler. Bindings 0 (Global UBO)
        // and 5 (GTAO UBO) are rewritten each frame by UpdateGlobalUniforms / UpdateGTAOUBO.
        // IBL bindings are skipped if InitIBLResources hasn't run yet;
        // ReloadSkybox/InitIBLResources rewrites every cached view afterwards.
        const bool haveIBL = m_IrradianceMap && m_PrefilteredMap && m_BRDFLut && m_IBLSampler;
        VkDescriptorImageInfo irrInfo{}, pfInfo{}, lutInfo{};
        if (haveIBL)
        {
            auto vkIrr = std::static_pointer_cast<VKTexture>(m_IrradianceMap);
            auto vkPf  = std::static_pointer_cast<VKTexture>(m_PrefilteredMap);
            auto vkLut = std::static_pointer_cast<VKTexture>(m_BRDFLut);

            irrInfo.sampler     = m_IBLSampler;
            irrInfo.imageView   = vkIrr->GetImageView();
            irrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            pfInfo.sampler      = m_IBLSampler;
            pfInfo.imageView    = vkPf->GetImageView();
            pfInfo.imageLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            lutInfo.sampler     = m_IBLSampler;
            lutInfo.imageView   = vkLut->GetImageView();
            lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        auto vkGTAOFinal = std::static_pointer_cast<VKTexture>(vr.gtaoFinal);
        VkDescriptorImageInfo gtaoFinalInfo{};
        gtaoFinalInfo.sampler     = m_GTAOSampler;
        gtaoFinalInfo.imageView   = vkGTAOFinal->GetImageView();
        gtaoFinalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[4] = {};
        u32 n = 0;

        if (haveIBL)
        {
            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = vr.globalDescriptorSet;
            writes[n].dstBinding      = 1;
            writes[n].descriptorCount = 1;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].pImageInfo      = &irrInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = vr.globalDescriptorSet;
            writes[n].dstBinding      = 2;
            writes[n].descriptorCount = 1;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].pImageInfo      = &pfInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = vr.globalDescriptorSet;
            writes[n].dstBinding      = 3;
            writes[n].descriptorCount = 1;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].pImageInfo      = &lutInfo;
            ++n;
        }

        writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[n].dstSet          = vr.globalDescriptorSet;
        writes[n].dstBinding      = 4;
        writes[n].descriptorCount = 1;
        writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[n].pImageInfo      = &gtaoFinalInfo;
        ++n;

        if (n > 0) vkUpdateDescriptorSets(device, n, writes, 0, nullptr);
    }

    void RenderPipeline::WriteViewPostProcessSets(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.compositeDescSet == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        auto sceneVk  = std::static_pointer_cast<VKTexture>(targets.GetSceneColor());
        auto bloomAVk = std::static_pointer_cast<VKTexture>(vr.bloomA);
        auto bloomBVk = std::static_pointer_cast<VKTexture>(vr.bloomB);

        // Stable image bindings only. The shared PostProcess UBO (binding 2 of each
        // PP set) is rewritten per render-stage in UpdatePostProcessUBO.
        auto makeImg = [&](VkImageView v) {
            VkDescriptorImageInfo info{};
            info.sampler     = m_PPSampler;
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

        VkWriteDescriptorSet writes[8] = {};
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

        addImg(vr.bloomExtractDescSet, 0, &bloomExtractImg0);
        addImg(vr.bloomExtractDescSet, 1, &bloomExtractImg1);
        addImg(vr.bloomBlurHDescSet,   0, &blurHImg0);
        addImg(vr.bloomBlurHDescSet,   1, &blurHImg1);
        addImg(vr.bloomBlurVDescSet,   0, &blurVImg0);
        addImg(vr.bloomBlurVDescSet,   1, &blurVImg1);
        addImg(vr.compositeDescSet,    0, &compImg0);
        addImg(vr.compositeDescSet,    1, &compImg1);

        vkUpdateDescriptorSets(device, idx, writes, 0, nullptr);
    }

    void RenderPipeline::WriteViewGTAOSets(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.gtaoPrefilterDescSet == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        auto vkSceneDepth = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth());
        auto vkLinDepth   = std::static_pointer_cast<VKTexture>(vr.gtaoLinearDepth);
        auto vkRawAO      = std::static_pointer_cast<VKTexture>(vr.gtaoRawAO);
        auto vkFinalAO    = std::static_pointer_cast<VKTexture>(vr.gtaoFinal);

        VkDescriptorImageInfo sceneDepthInfo{};
        sceneDepthInfo.sampler     = m_GTAOSampler;
        sceneDepthInfo.imageView   = vkSceneDepth->GetImageView();
        sceneDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo linDepthSampledInfo{};
        linDepthSampledInfo.sampler     = m_GTAOSampler;
        linDepthSampledInfo.imageView   = vkLinDepth->GetImageView();
        linDepthSampledInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo linDepthStorageInfo{};
        linDepthStorageInfo.imageView   = vkLinDepth->GetImageView();
        linDepthStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo rawAOStorageInfo{};
        rawAOStorageInfo.imageView   = vkRawAO->GetImageView();
        rawAOStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // GTAO main set binding 2 (UBO) is rewritten per render-stage in UpdateGTAOUBO
        // alongside Set 0 binding 5. Stable bindings only here.

        // Prefilter: [sceneDepth (sampler), linDepth (storage)]
        VkWriteDescriptorSet preWrites[2]{};
        preWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        preWrites[0].dstSet          = vr.gtaoPrefilterDescSet;
        preWrites[0].dstBinding      = 0;
        preWrites[0].descriptorCount = 1;
        preWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        preWrites[0].pImageInfo      = &sceneDepthInfo;

        preWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        preWrites[1].dstSet          = vr.gtaoPrefilterDescSet;
        preWrites[1].dstBinding      = 1;
        preWrites[1].descriptorCount = 1;
        preWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        preWrites[1].pImageInfo      = &linDepthStorageInfo;

        vkUpdateDescriptorSets(device, 2, preWrites, 0, nullptr);

        if (vr.gtaoMainDescSet == VK_NULL_HANDLE) return;

        VkWriteDescriptorSet mainWrites[2]{};
        mainWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        mainWrites[0].dstSet          = vr.gtaoMainDescSet;
        mainWrites[0].dstBinding      = 0;
        mainWrites[0].descriptorCount = 1;
        mainWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mainWrites[0].pImageInfo      = &linDepthSampledInfo;

        mainWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        mainWrites[1].dstSet          = vr.gtaoMainDescSet;
        mainWrites[1].dstBinding      = 1;
        mainWrites[1].descriptorCount = 1;
        mainWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        mainWrites[1].pImageInfo      = &rawAOStorageInfo;

        vkUpdateDescriptorSets(device, 2, mainWrites, 0, nullptr);

        if (vr.gtaoDenoiseDescSet == VK_NULL_HANDLE) return;

        VkDescriptorImageInfo rawAOSampledInfo{};
        rawAOSampledInfo.sampler     = m_GTAOSampler;
        rawAOSampledInfo.imageView   = vkRawAO->GetImageView();
        rawAOSampledInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo finalAOStorageInfo{};
        finalAOStorageInfo.imageView   = vkFinalAO->GetImageView();
        finalAOStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet denoiseWrites[3]{};
        denoiseWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        denoiseWrites[0].dstSet          = vr.gtaoDenoiseDescSet;
        denoiseWrites[0].dstBinding      = 0;
        denoiseWrites[0].descriptorCount = 1;
        denoiseWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        denoiseWrites[0].pImageInfo      = &rawAOSampledInfo;

        denoiseWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        denoiseWrites[1].dstSet          = vr.gtaoDenoiseDescSet;
        denoiseWrites[1].dstBinding      = 1;
        denoiseWrites[1].descriptorCount = 1;
        denoiseWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        denoiseWrites[1].pImageInfo      = &linDepthSampledInfo;

        denoiseWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        denoiseWrites[2].dstSet          = vr.gtaoDenoiseDescSet;
        denoiseWrites[2].dstBinding      = 2;
        denoiseWrites[2].descriptorCount = 1;
        denoiseWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        denoiseWrites[2].pImageInfo      = &finalAOStorageInfo;

        vkUpdateDescriptorSets(device, 3, denoiseWrites, 0, nullptr);
    }

    void RenderPipeline::WriteViewOutlineSet(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.outlineDescSet == VK_NULL_HANDLE || m_OutlineSampler == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        auto vkMask     = std::static_pointer_cast<VKTexture>(targets.GetSelectionMask());
        auto vkSelDepth = std::static_pointer_cast<VKTexture>(targets.GetSelectionDepth());
        auto vkScnDepth = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth());

        VkDescriptorImageInfo maskInfo{};
        maskInfo.sampler     = m_OutlineSampler;
        maskInfo.imageView   = vkMask->GetImageView();
        maskInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo selDepthInfo{};
        selDepthInfo.sampler     = m_OutlineSampler;
        selDepthInfo.imageView   = vkSelDepth->GetImageView();
        selDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo scnDepthInfo{};
        scnDepthInfo.sampler     = m_OutlineSampler;
        scnDepthInfo.imageView   = vkScnDepth->GetImageView();
        scnDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[3] = {};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr.outlineDescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &maskInfo;

        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = vr.outlineDescSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &selDepthInfo;

        writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[2].dstSet          = vr.outlineDescSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &scnDepthInfo;

        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    }

    void RenderPipeline::WriteViewGridSet(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.gridDescSet == VK_NULL_HANDLE || m_GridDepthSampler == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        // Binding 0 (per-view GlobalUBO) is rewritten per render-stage in
        // UpdateGlobalUniforms alongside Set 0 binding 0. Stable depth sampler only here.
        auto vkScnDepth = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth());
        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler     = m_GridDepthSampler;
        depthInfo.imageView   = vkScnDepth->GetImageView();
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet samplerWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        samplerWrite.dstSet          = vr.gridDescSet;
        samplerWrite.dstBinding      = 1;
        samplerWrite.descriptorCount = 1;
        samplerWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerWrite.pImageInfo      = &depthInfo;

        vkUpdateDescriptorSets(device, 1, &samplerWrite, 0, nullptr);
    }

    void RenderPipeline::DestroyViewResources(ViewResources& vr)
    {
        // Pool destruction frees every descriptor set allocated from it.
        vr.bloomA.reset();
        vr.bloomB.reset();
        vr.gtaoLinearDepth.reset();
        vr.gtaoRawAO.reset();
        vr.gtaoEdges.reset();
        vr.gtaoFinal.reset();

        if (vr.descPool != VK_NULL_HANDLE)
        {
            VulkanContext::Get().PushDeletion([pool = vr.descPool]() {
                vkDestroyDescriptorPool(VulkanContext::Get().GetDevice(), pool, nullptr);
            });
            vr.descPool = VK_NULL_HANDLE;
        }
        vr = {};
    }
}
