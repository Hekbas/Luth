#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/settings/GTAOSettings.h"

namespace Luth
{
    // =========================================================================
    //  GTAO (Ground Truth Ambient Occlusion) — epic #58
    // =========================================================================
    //
    // InitAOResources allocates the persistent half-res textures that back
    // the GTAO pass chain (prefilter → main → denoise) and the prefilter
    // compute pipeline. Main/denoise pipelines land in their own sub-tasks;
    // their textures are allocated here too so Resize sizes everything in
    // one place.
    //
    // Descriptor writes live in UpdateAODescriptors — called at end of init
    // and again after Resize recreates the textures (which invalidates any
    // stored image view pointers in the descriptor sets).
    void RenderPipeline::InitAOResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // ---- Half-res persistent textures ----
        const u32 halfW = std::max(m_System.m_Targets.GetSceneColor()->GetWidth()  / 2, 1u);
        const u32 halfH = std::max(m_System.m_Targets.GetSceneColor()->GetHeight() / 2, 1u);

        auto makeStorage = [&](TextureFormat fmt) {
            return std::make_shared<VKTexture>(
                halfW, halfH, fmt,
                /*arrayLayers*/ 1,
                /*createFlags*/ 0u,
                /*mipLevels*/   1,
                VK_IMAGE_USAGE_STORAGE_BIT);
        };

        m_GTAOLinearDepth = makeStorage(TextureFormat::R32_Float);
        m_GTAORawAO       = makeStorage(TextureFormat::R8);
        m_GTAOEdges       = makeStorage(TextureFormat::R8);
        m_GTAOFinal       = makeStorage(TextureFormat::R8);

        // ---- Shared linear-clamp sampler for GTAO compute reads ----
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_GTAOSampler);

        // ---- Shared descriptor pool for all GTAO sets ----
        // Enough capacity for prefilter + main + denoise (sub-task E).
        VkDescriptorPoolSize poolSizes[3] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          8 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         4 },
        };
        VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolCI.maxSets       = 8;
        poolCI.poolSizeCount = 3;
        poolCI.pPoolSizes    = poolSizes;
        vkCreateDescriptorPool(device, &poolCI, nullptr, &m_GTAODescPool);

        // ---- GTAO UBO (GTAOUBO std140, 48B) ----
        m_GTAOUBOBuffer = std::make_shared<VKUniformBuffer>(sizeof(GTAOUBO));

        // ---- Depth prefilter: [sampler2D sceneDepth, image2D linearDepth] ----
        {
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
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_GTAOPrefilterDescLayout);

            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool     = m_GTAODescPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_GTAOPrefilterDescLayout;
            vkAllocateDescriptorSets(device, &allocInfo, &m_GTAOPrefilterDescSet);

            VkPushConstantRange pcRange{};
            pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcRange.offset     = 0;
            pcRange.size       = sizeof(i32) * 2 + sizeof(float) * 6; // halfResSize + invFullRes + nearZ + farZ + pads

            if (auto sh = ShaderLibrary::LoadEngine("shaders/gtao_depth_prefilter.comp"))
                m_GTAOPrefilterSpv = sh->GetSpirV();
            if (m_GTAOPrefilterSpv.empty())
            {
                LH_CORE_ERROR("RenderingSystem: Failed to load gtao_depth_prefilter.comp!");
                return;
            }
            m_GTAOPrefilterPipeline = std::make_unique<VKComputePipeline>(
                m_GTAOPrefilterSpv,
                std::vector<VkDescriptorSetLayout>{ m_GTAOPrefilterDescLayout },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // ---- Main pass: [sampler2D linearDepth, image2D rawAO, UBO] ----
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[2].binding         = 2;
            bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.bindingCount = 3;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_GTAOMainDescLayout);

            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool     = m_GTAODescPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_GTAOMainDescLayout;
            vkAllocateDescriptorSets(device, &allocInfo, &m_GTAOMainDescSet);

            VkPushConstantRange pcRange{};
            pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcRange.offset     = 0;
            pcRange.size       = sizeof(float) * 4 + sizeof(u32) * 4; // projParams + near/far + frameIndex + pads

            if (auto sh = ShaderLibrary::LoadEngine("shaders/gtao_main.comp"))
                m_GTAOMainSpv = sh->GetSpirV();
            if (m_GTAOMainSpv.empty())
            {
                LH_CORE_ERROR("RenderingSystem: Failed to load gtao_main.comp!");
                return;
            }
            m_GTAOMainPipeline = std::make_unique<VKComputePipeline>(
                m_GTAOMainSpv,
                std::vector<VkDescriptorSetLayout>{ m_GTAOMainDescLayout },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // ---- Denoise pass: [sampler2D rawAO, sampler2D linDepth, image2D finalAO] ----
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[2].binding         = 2;
            bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.bindingCount = 3;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_GTAODenoiseDescLayout);

            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool     = m_GTAODescPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_GTAODenoiseDescLayout;
            vkAllocateDescriptorSets(device, &allocInfo, &m_GTAODenoiseDescSet);

            // No push constants — resolution derived from textureSize() inside the shader.
            if (auto sh = ShaderLibrary::LoadEngine("shaders/gtao_denoise.comp"))
                m_GTAODenoiseSpv = sh->GetSpirV();
            if (m_GTAODenoiseSpv.empty())
            {
                LH_CORE_ERROR("RenderingSystem: Failed to load gtao_denoise.comp!");
                return;
            }
            m_GTAODenoisePipeline = std::make_unique<VKComputePipeline>(
                m_GTAODenoiseSpv,
                std::vector<VkDescriptorSetLayout>{ m_GTAODenoiseDescLayout },
                std::vector<VkPushConstantRange>{});
        }

        UpdateAODescriptors();
    }

    void RenderPipeline::UpdateAODescriptors()
    {
        if (m_GTAOPrefilterDescSet == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        auto vkSceneDepth = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneDepth());
        auto vkLinDepth   = std::static_pointer_cast<VKTexture>(m_GTAOLinearDepth);
        auto vkRawAO      = std::static_pointer_cast<VKTexture>(m_GTAORawAO);
        auto vkFinalAO    = std::static_pointer_cast<VKTexture>(m_GTAOFinal);

        // Shared VkDescriptorImageInfo / buffer-info slots reused across passes.
        VkDescriptorImageInfo  sceneDepthInfo{};
        sceneDepthInfo.sampler     = m_GTAOSampler;
        sceneDepthInfo.imageView   = vkSceneDepth->GetImageView();
        sceneDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo  linDepthSampledInfo{};
        linDepthSampledInfo.sampler     = m_GTAOSampler;
        linDepthSampledInfo.imageView   = vkLinDepth->GetImageView();
        linDepthSampledInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo  linDepthStorageInfo{};
        linDepthStorageInfo.sampler     = VK_NULL_HANDLE;
        linDepthStorageInfo.imageView   = vkLinDepth->GetImageView();
        linDepthStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo  rawAOStorageInfo{};
        rawAOStorageInfo.sampler     = VK_NULL_HANDLE;
        rawAOStorageInfo.imageView   = vkRawAO->GetImageView();
        rawAOStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = m_GTAOUBOBuffer ? m_GTAOUBOBuffer->GetVulkanBuffer() : VK_NULL_HANDLE;
        uboInfo.offset = 0;
        uboInfo.range  = VK_WHOLE_SIZE;

        // ---- Prefilter pass: [sceneDepth (sampler), linDepth (storage)] ----
        VkWriteDescriptorSet preWrites[2]{};
        preWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        preWrites[0].dstSet          = m_GTAOPrefilterDescSet;
        preWrites[0].dstBinding      = 0;
        preWrites[0].descriptorCount = 1;
        preWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        preWrites[0].pImageInfo      = &sceneDepthInfo;

        preWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        preWrites[1].dstSet          = m_GTAOPrefilterDescSet;
        preWrites[1].dstBinding      = 1;
        preWrites[1].descriptorCount = 1;
        preWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        preWrites[1].pImageInfo      = &linDepthStorageInfo;

        vkUpdateDescriptorSets(device, 2, preWrites, 0, nullptr);

        // ---- Main pass: [linDepth (sampler), rawAO (storage), UBO] ----
        if (m_GTAOMainDescSet == VK_NULL_HANDLE || uboInfo.buffer == VK_NULL_HANDLE) return;

        VkWriteDescriptorSet mainWrites[3]{};
        mainWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mainWrites[0].dstSet          = m_GTAOMainDescSet;
        mainWrites[0].dstBinding      = 0;
        mainWrites[0].descriptorCount = 1;
        mainWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mainWrites[0].pImageInfo      = &linDepthSampledInfo;

        mainWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mainWrites[1].dstSet          = m_GTAOMainDescSet;
        mainWrites[1].dstBinding      = 1;
        mainWrites[1].descriptorCount = 1;
        mainWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        mainWrites[1].pImageInfo      = &rawAOStorageInfo;

        mainWrites[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mainWrites[2].dstSet          = m_GTAOMainDescSet;
        mainWrites[2].dstBinding      = 2;
        mainWrites[2].descriptorCount = 1;
        mainWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mainWrites[2].pBufferInfo     = &uboInfo;

        vkUpdateDescriptorSets(device, 3, mainWrites, 0, nullptr);

        // ---- Denoise pass: [rawAO (sampler), linDepth (sampler), finalAO (storage)] ----
        if (m_GTAODenoiseDescSet == VK_NULL_HANDLE) return;

        VkDescriptorImageInfo rawAOSampledInfo{};
        rawAOSampledInfo.sampler     = m_GTAOSampler;
        rawAOSampledInfo.imageView   = vkRawAO->GetImageView();
        rawAOSampledInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo finalAOStorageInfo{};
        finalAOStorageInfo.sampler     = VK_NULL_HANDLE;
        finalAOStorageInfo.imageView   = vkFinalAO->GetImageView();
        finalAOStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet denoiseWrites[3]{};
        denoiseWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        denoiseWrites[0].dstSet          = m_GTAODenoiseDescSet;
        denoiseWrites[0].dstBinding      = 0;
        denoiseWrites[0].descriptorCount = 1;
        denoiseWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        denoiseWrites[0].pImageInfo      = &rawAOSampledInfo;

        denoiseWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        denoiseWrites[1].dstSet          = m_GTAODenoiseDescSet;
        denoiseWrites[1].dstBinding      = 1;
        denoiseWrites[1].descriptorCount = 1;
        denoiseWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        denoiseWrites[1].pImageInfo      = &linDepthSampledInfo;

        denoiseWrites[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        denoiseWrites[2].dstSet          = m_GTAODenoiseDescSet;
        denoiseWrites[2].dstBinding      = 2;
        denoiseWrites[2].descriptorCount = 1;
        denoiseWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        denoiseWrites[2].pImageInfo      = &finalAOStorageInfo;

        vkUpdateDescriptorSets(device, 3, denoiseWrites, 0, nullptr);

        // ---- Set 0 GTAO bindings (sampled by pbr.frag) ----
        if (m_GlobalDescriptorSet == VK_NULL_HANDLE) return;

        VkDescriptorImageInfo gtaoFinalInfo{};
        gtaoFinalInfo.sampler     = m_GTAOSampler;
        gtaoFinalInfo.imageView   = vkFinalAO->GetImageView();
        gtaoFinalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet globalWrites[2]{};
        globalWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        globalWrites[0].dstSet          = m_GlobalDescriptorSet;
        globalWrites[0].dstBinding      = 4;
        globalWrites[0].descriptorCount = 1;
        globalWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        globalWrites[0].pImageInfo      = &gtaoFinalInfo;

        globalWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        globalWrites[1].dstSet          = m_GlobalDescriptorSet;
        globalWrites[1].dstBinding      = 5;
        globalWrites[1].descriptorCount = 1;
        globalWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        globalWrites[1].pBufferInfo     = &uboInfo;

        vkUpdateDescriptorSets(device, 2, globalWrites, 0, nullptr);
    }

    void RenderPipeline::UpdateGTAOUBO()
    {
        if (!m_GTAOUBOBuffer) return;

        const auto& s = m_System.m_PostProcessSettings.gtao;
        GTAOUBO ubo{};
        ubo.intensity      = s.intensity;
        ubo.radius         = s.radius;
        ubo.falloff        = s.falloff;
        ubo.power          = s.power;
        ubo.sliceCount     = s.sliceCount;
        ubo.stepsPerSlice  = s.stepsPerSlice;
        ubo.enabled        = s.enabled  ? 1 : 0;
        ubo.visualize      = s.visualize ? 1 : 0;

        const u32 halfW = m_GTAOLinearDepth ? m_GTAOLinearDepth->GetWidth()  : 1u;
        const u32 halfH = m_GTAOLinearDepth ? m_GTAOLinearDepth->GetHeight() : 1u;
        const u32 fullW = m_System.m_Targets.GetSceneColor() ? m_System.m_Targets.GetSceneColor()->GetWidth()  : 1u;
        const u32 fullH = m_System.m_Targets.GetSceneColor() ? m_System.m_Targets.GetSceneColor()->GetHeight() : 1u;
        ubo.invResolution[0]     = 1.0f / float(halfW);
        ubo.invResolution[1]     = 1.0f / float(halfH);
        ubo.invFullResolution[0] = 1.0f / float(fullW);
        ubo.invFullResolution[1] = 1.0f / float(fullH);

        m_GTAOUBOBuffer->SetData(&ubo, sizeof(GTAOUBO));
    }
}
