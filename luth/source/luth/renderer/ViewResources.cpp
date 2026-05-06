#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/subsystems/GlobalSubsystem.h"
#include "luth/renderer/subsystems/GTAOSubsystem.h"
#include "luth/renderer/subsystems/PostProcessSubsystem.h"
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

    namespace {
        // Build the per-view Set 0 write context from RP-side state. invariant:
        // GTAO textures must already exist (RecreateViewTextures runs before).
        GlobalViewWriteContext MakeGlobalCtx(const RenderPipeline& rp, const ViewResources& vr)
        {
            const auto& lighting = rp.GetLighting();
            GlobalViewWriteContext ctx{};
            ctx.haveIBL          = lighting.IsIBLReady();
            ctx.iblSampler       = lighting.GetIBLSampler();
            ctx.gtaoSampler      = rp.GetGTAO().GetSampler();
            if (ctx.haveIBL)
            {
                ctx.irradianceView  = std::static_pointer_cast<VKTexture>(lighting.GetIrradianceMap())->GetImageView();
                ctx.prefilteredView = std::static_pointer_cast<VKTexture>(lighting.GetPrefilteredMap())->GetImageView();
                ctx.brdfView        = std::static_pointer_cast<VKTexture>(lighting.GetBRDFLut())->GetImageView();
            }
            ctx.gtaoFinalView = vr.gtaoFinal
                ? std::static_pointer_cast<VKTexture>(vr.gtaoFinal)->GetImageView()
                : VK_NULL_HANDLE;
            return ctx;
        }
    }

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
            m_PostProcess.WriteView(vr, targets);
            m_GTAO.WriteView(vr, targets);
            WriteViewOutlineSet(vr, targets);
            WriteViewGridSet(vr, targets);
            // Set 0 bindings 1-4 reference the (re)created IBL + GTAO textures.
            m_Global.WriteView(vr, MakeGlobalCtx(*this, vr));
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

        const VkDescriptorSetLayout ppLayout = m_PostProcess.GetDescSetLayout();
        alloc(m_Global.GetSetLayout(),         vr.globalDescriptorSet);
        alloc(ppLayout,                        vr.bloomExtractDescSet);
        alloc(ppLayout,                        vr.bloomBlurHDescSet);
        alloc(ppLayout,                        vr.bloomBlurVDescSet);
        alloc(ppLayout,                        vr.compositeDescSet);
        alloc(m_GTAO.GetPrefilterLayout(),     vr.gtaoPrefilterDescSet);
        alloc(m_GTAO.GetMainLayout(),          vr.gtaoMainDescSet);
        alloc(m_GTAO.GetDenoiseLayout(),       vr.gtaoDenoiseDescSet);
        alloc(m_OutlineDescSetLayout,          vr.outlineDescSet);
        alloc(m_GridDescSetLayout,             vr.gridDescSet);

        m_PostProcess.WriteView(vr, targets);
        m_GTAO.WriteView(vr, targets);
        WriteViewOutlineSet(vr, targets);
        WriteViewGridSet(vr, targets);
        // Global writes last — reads vr.gtaoFinal view that GTAO writes set up.
        m_Global.WriteView(vr, MakeGlobalCtx(*this, vr));
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
