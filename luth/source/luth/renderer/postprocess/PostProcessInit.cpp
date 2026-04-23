#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/core/time/Time.h"

namespace Luth
{
    void RenderPipeline::InitPostProcessResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        u32 w = m_System.m_SceneTargets.GetSceneColor()->GetWidth();
        u32 h = m_System.m_SceneTargets.GetSceneColor()->GetHeight();

        // Bloom textures (half-res)
        m_BloomA = Texture::Create(std::max(w / 2, 1u), std::max(h / 2, 1u), TextureFormat::RGBA16F);
        m_BloomB = Texture::Create(std::max(w / 2, 1u), std::max(h / 2, 1u), TextureFormat::RGBA16F);

        // Post-process UBO
        m_PostProcessUBOBuffer = std::make_shared<VKUniformBuffer>(sizeof(PostProcessUBO));

        // Linear clamp sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        vkCreateSampler(device, &samplerInfo, nullptr, &m_PPSampler);

        // Descriptor set layout: [sampler2D, sampler2D, UBO]
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

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_PPDescSetLayout);

        // Descriptor pool: 4 sets × (2 samplers + 1 UBO)
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 8; // 4 sets × 2 sampler bindings
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[1].descriptorCount = 4; // 4 sets × 1 UBO binding

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 4;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_PPDescPool);

        // Allocate 4 descriptor sets
        VkDescriptorSetLayout setLayouts[4] = { m_PPDescSetLayout, m_PPDescSetLayout, m_PPDescSetLayout, m_PPDescSetLayout };
        VkDescriptorSet sets[4];
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_PPDescPool;
        allocInfo.descriptorSetCount = 4;
        allocInfo.pSetLayouts = setLayouts;
        vkAllocateDescriptorSets(device, &allocInfo, sets);

        m_BloomExtractDescSet = sets[0];
        m_BloomBlurHDescSet   = sets[1];
        m_BloomBlurVDescSet   = sets[2];
        m_CompositeDescSet    = sets[3];

        UpdatePostProcessDescriptors(m_System.m_SceneTargets);

        // ---- Outline pass resources ----
        {
            // Nearest-neighbor sampler for mask and depth textures
            VkSamplerCreateInfo outlineSamplerInfo{};
            outlineSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            outlineSamplerInfo.magFilter = VK_FILTER_NEAREST;
            outlineSamplerInfo.minFilter = VK_FILTER_NEAREST;
            outlineSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &outlineSamplerInfo, nullptr, &m_OutlineSampler);

            // Descriptor set layout: 3 sampler bindings
            VkDescriptorSetLayoutBinding bindings[3] = {};
            // Binding 0: sampler2D (selection mask)
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            // Binding 1: sampler2D (selection depth)
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            // Binding 2: sampler2D (scene depth)
            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo outlineLayoutInfo{};
            outlineLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            outlineLayoutInfo.bindingCount = 3;
            outlineLayoutInfo.pBindings = bindings;
            vkCreateDescriptorSetLayout(device, &outlineLayoutInfo, nullptr, &m_OutlineDescSetLayout);

            // Descriptor pool: 1 set, 3 combined image samplers
            VkDescriptorPoolSize outlinePoolSize{};
            outlinePoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            outlinePoolSize.descriptorCount = 3;

            VkDescriptorPoolCreateInfo outlinePoolInfo{};
            outlinePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            outlinePoolInfo.maxSets = 1;
            outlinePoolInfo.poolSizeCount = 1;
            outlinePoolInfo.pPoolSizes = &outlinePoolSize;
            vkCreateDescriptorPool(device, &outlinePoolInfo, nullptr, &m_OutlineDescPool);

            // Allocate descriptor set
            VkDescriptorSetAllocateInfo outlineAllocInfo{};
            outlineAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            outlineAllocInfo.descriptorPool = m_OutlineDescPool;
            outlineAllocInfo.descriptorSetCount = 1;
            outlineAllocInfo.pSetLayouts = &m_OutlineDescSetLayout;
            vkAllocateDescriptorSets(device, &outlineAllocInfo, &m_OutlineDescSet);

            // Write all 3 descriptors: selection mask, selection depth, scene depth
            auto vkMask      = std::static_pointer_cast<VKTexture>(m_System.m_SceneTargets.GetSelectionMask());
            auto vkSelDepth  = std::static_pointer_cast<VKTexture>(m_System.m_SceneTargets.GetSelectionDepth());
            auto vkScnDepth  = std::static_pointer_cast<VKTexture>(m_System.m_SceneTargets.GetSceneDepth());

            VkDescriptorImageInfo maskImgInfo{};
            maskImgInfo.sampler     = m_OutlineSampler;
            maskImgInfo.imageView   = vkMask->GetImageView();
            maskImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo selDepthImgInfo{};
            selDepthImgInfo.sampler     = m_OutlineSampler;
            selDepthImgInfo.imageView   = vkSelDepth->GetImageView();
            selDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo scnDepthImgInfo{};
            scnDepthImgInfo.sampler     = m_OutlineSampler;
            scnDepthImgInfo.imageView   = vkScnDepth->GetImageView();
            scnDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3] = {};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_OutlineDescSet;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &maskImgInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = m_OutlineDescSet;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &selDepthImgInfo;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = m_OutlineDescSet;
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &scnDepthImgInfo;

            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }

        // ---- Grid pass resources ----
        {
            // Linear sampler for scene depth read (matching behaviour used elsewhere for depth texture reads)
            VkSamplerCreateInfo gridSamplerInfo{};
            gridSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            gridSamplerInfo.magFilter = VK_FILTER_NEAREST;
            gridSamplerInfo.minFilter = VK_FILTER_NEAREST;
            gridSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &gridSamplerInfo, nullptr, &m_GridDepthSampler);

            // Descriptor set layout: binding 0 = GlobalUBO (camera), binding 1 = scene depth sampler
            VkDescriptorSetLayoutBinding gridBindings[2] = {};
            gridBindings[0].binding = 0;
            gridBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            gridBindings[0].descriptorCount = 1;
            gridBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            gridBindings[1].binding = 1;
            gridBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gridBindings[1].descriptorCount = 1;
            gridBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo gridLayoutInfo{};
            gridLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            gridLayoutInfo.bindingCount = 2;
            gridLayoutInfo.pBindings = gridBindings;
            vkCreateDescriptorSetLayout(device, &gridLayoutInfo, nullptr, &m_GridDescSetLayout);

            // Descriptor pool: 1 set (UBO + sampler)
            VkDescriptorPoolSize gridPoolSizes[2] = {};
            gridPoolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            gridPoolSizes[0].descriptorCount = 1;
            gridPoolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gridPoolSizes[1].descriptorCount = 1;

            VkDescriptorPoolCreateInfo gridPoolInfo{};
            gridPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            gridPoolInfo.maxSets = 1;
            gridPoolInfo.poolSizeCount = 2;
            gridPoolInfo.pPoolSizes = gridPoolSizes;
            vkCreateDescriptorPool(device, &gridPoolInfo, nullptr, &m_GridDescPool);

            VkDescriptorSetAllocateInfo gridAllocInfo{};
            gridAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            gridAllocInfo.descriptorPool = m_GridDescPool;
            gridAllocInfo.descriptorSetCount = 1;
            gridAllocInfo.pSetLayouts = &m_GridDescSetLayout;
            vkAllocateDescriptorSets(device, &gridAllocInfo, &m_GridDescSet);

            // Write: global UBO + scene depth
            VkDescriptorBufferInfo gridUBOInfo{};
            gridUBOInfo.buffer = m_GlobalUniformBuffer->GetVulkanBuffer();
            gridUBOInfo.offset = 0;
            gridUBOInfo.range  = sizeof(GlobalUniforms);

            auto vkScnDepthGrid = std::static_pointer_cast<VKTexture>(m_System.m_SceneTargets.GetSceneDepth());
            VkDescriptorImageInfo gridDepthImgInfo{};
            gridDepthImgInfo.sampler     = m_GridDepthSampler;
            gridDepthImgInfo.imageView   = vkScnDepthGrid->GetImageView();
            gridDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet gridWrites[2] = {};
            gridWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gridWrites[0].dstSet = m_GridDescSet;
            gridWrites[0].dstBinding = 0;
            gridWrites[0].descriptorCount = 1;
            gridWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            gridWrites[0].pBufferInfo = &gridUBOInfo;

            gridWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gridWrites[1].dstSet = m_GridDescSet;
            gridWrites[1].dstBinding = 1;
            gridWrites[1].descriptorCount = 1;
            gridWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gridWrites[1].pImageInfo = &gridDepthImgInfo;

            vkUpdateDescriptorSets(device, 2, gridWrites, 0, nullptr);
        }
    }

    void RenderPipeline::UpdatePostProcessDescriptors(FrameTargets& targets)
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        auto sceneVk  = std::static_pointer_cast<VKTexture>(targets.GetSceneColor());
        auto bloomAVk = std::static_pointer_cast<VKTexture>(m_BloomA);
        auto bloomBVk = std::static_pointer_cast<VKTexture>(m_BloomB);

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = m_PostProcessUBOBuffer->GetVulkanBuffer();
        uboInfo.offset = 0;
        uboInfo.range  = sizeof(PostProcessUBO);

        // Helper: write a combined image sampler descriptor
        auto MakeImageInfo = [this](VkImageView view) -> VkDescriptorImageInfo {
            VkDescriptorImageInfo info{};
            info.sampler     = m_PPSampler;
            info.imageView   = view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return info;
        };

        // BloomExtract: binding 0 = SceneColor, binding 1 = unused (BloomA as placeholder), binding 2 = UBO
        VkDescriptorImageInfo bloomExtractImg0 = MakeImageInfo(sceneVk->GetImageView());
        VkDescriptorImageInfo bloomExtractImg1 = MakeImageInfo(bloomAVk->GetImageView());

        // BloomBlurH: binding 0 = BloomA, binding 1 = unused, binding 2 = UBO
        VkDescriptorImageInfo blurHImg0 = MakeImageInfo(bloomAVk->GetImageView());
        VkDescriptorImageInfo blurHImg1 = MakeImageInfo(bloomBVk->GetImageView());

        // BloomBlurV: binding 0 = BloomB, binding 1 = unused, binding 2 = UBO
        VkDescriptorImageInfo blurVImg0 = MakeImageInfo(bloomBVk->GetImageView());
        VkDescriptorImageInfo blurVImg1 = MakeImageInfo(bloomAVk->GetImageView());

        // Composite: binding 0 = SceneColor (HDR), binding 1 = BloomA (blurred), binding 2 = UBO
        VkDescriptorImageInfo compImg0 = MakeImageInfo(sceneVk->GetImageView());
        VkDescriptorImageInfo compImg1 = MakeImageInfo(bloomAVk->GetImageView());

        // Write all 4 sets (3 writes each = 12 total)
        VkWriteDescriptorSet writes[12] = {};
        int idx = 0;

        auto AddWrite = [&](VkDescriptorSet set, u32 binding, VkDescriptorType type,
                           VkDescriptorImageInfo* imgInfo, VkDescriptorBufferInfo* bufInfo) {
            writes[idx].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[idx].dstSet = set;
            writes[idx].dstBinding = binding;
            writes[idx].descriptorCount = 1;
            writes[idx].descriptorType = type;
            writes[idx].pImageInfo = imgInfo;
            writes[idx].pBufferInfo = bufInfo;
            idx++;
        };

        // BloomExtract set
        AddWrite(m_BloomExtractDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bloomExtractImg0, nullptr);
        AddWrite(m_BloomExtractDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bloomExtractImg1, nullptr);
        AddWrite(m_BloomExtractDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // BloomBlurH set
        AddWrite(m_BloomBlurHDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurHImg0, nullptr);
        AddWrite(m_BloomBlurHDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurHImg1, nullptr);
        AddWrite(m_BloomBlurHDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // BloomBlurV set
        AddWrite(m_BloomBlurVDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurVImg0, nullptr);
        AddWrite(m_BloomBlurVDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurVImg1, nullptr);
        AddWrite(m_BloomBlurVDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // Composite set
        AddWrite(m_CompositeDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &compImg0, nullptr);
        AddWrite(m_CompositeDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &compImg1, nullptr);
        AddWrite(m_CompositeDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        vkUpdateDescriptorSets(device, idx, writes, 0, nullptr);
    }

    void RenderPipeline::UpdatePostProcessUBO()
    {
        PostProcessUBO ubo{};
        ubo.bloomThreshold      = m_System.m_PostProcessSettings.bloomThreshold;
        ubo.bloomStrength       = m_System.m_PostProcessSettings.bloomStrength;
        ubo.exposure            = m_System.m_PostProcessSettings.exposure;
        ubo.contrast            = m_System.m_PostProcessSettings.contrast;
        ubo.saturation          = m_System.m_PostProcessSettings.saturation;
        ubo.tonemapOp           = static_cast<int>(m_System.m_PostProcessSettings.tonemapOp);
        ubo.vignetteAmount      = m_System.m_PostProcessSettings.vignetteAmount;
        ubo.vignetteHardness    = m_System.m_PostProcessSettings.vignetteHardness;
        ubo.grainAmount         = m_System.m_PostProcessSettings.grainAmount;
        ubo.sharpness           = m_System.m_PostProcessSettings.sharpness;
        ubo.chromaticAberration = m_System.m_PostProcessSettings.chromaticAberration;
        ubo.time                = Time::GetTime();
        ubo.shadowBalance       = m_System.m_PostProcessSettings.shadowBalance;
        ubo.midtoneBalance      = m_System.m_PostProcessSettings.midtoneBalance;
        ubo.highlightBalance    = m_System.m_PostProcessSettings.highlightBalance;
        m_PostProcessUBOBuffer->SetData(&ubo, sizeof(PostProcessUBO));
    }
}
