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
    // Post-process / outline / grid shared state. Per-view resources
    // (bloomA / bloomB textures + all descriptor sets) are allocated by
    // EnsureViewResources in ViewResources.cpp — this function only builds
    // the shared pieces:
    //   - Post-process UBO (contains scalar settings, view-independent).
    //   - PP sampler + descriptor-set layout.
    //   - Outline sampler + descriptor-set layout.
    //   - Grid depth sampler + descriptor-set layout.
    void RenderPipeline::InitPostProcessResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Post-process UBO — scalar settings only, shared across views.
        m_PostProcessUBOBuffer = std::make_shared<VKUniformBuffer>(sizeof(PostProcessUBO));

        // Linear clamp sampler — used by bloom + composite descriptors.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        vkCreateSampler(device, &samplerInfo, nullptr, &m_PPSampler);

        // PP descriptor-set layout (bloom extract / blur / composite):
        //   binding 0 = sampler2D (primary),
        //   binding 1 = sampler2D (secondary),
        //   binding 2 = UBO (shared PP settings).
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

        // Outline layout — 3 combined image samplers (mask, selection depth,
        // scene depth). Nearest-filter sampler shared across views.
        {
            VkSamplerCreateInfo outlineSamplerInfo{};
            outlineSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            outlineSamplerInfo.magFilter = VK_FILTER_NEAREST;
            outlineSamplerInfo.minFilter = VK_FILTER_NEAREST;
            outlineSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &outlineSamplerInfo, nullptr, &m_OutlineSampler);

            VkDescriptorSetLayoutBinding outBindings[3] = {};
            for (u32 i = 0; i < 3; ++i) {
                outBindings[i].binding = i;
                outBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                outBindings[i].descriptorCount = 1;
                outBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }

            VkDescriptorSetLayoutCreateInfo outLayoutInfo{};
            outLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            outLayoutInfo.bindingCount = 3;
            outLayoutInfo.pBindings = outBindings;
            vkCreateDescriptorSetLayout(device, &outLayoutInfo, nullptr, &m_OutlineDescSetLayout);
        }

        // Grid layout — binding 0 = per-view GlobalUBO (for viewProj),
        // binding 1 = scene depth sampler.
        {
            VkSamplerCreateInfo gridSamplerInfo{};
            gridSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            gridSamplerInfo.magFilter = VK_FILTER_NEAREST;
            gridSamplerInfo.minFilter = VK_FILTER_NEAREST;
            gridSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &gridSamplerInfo, nullptr, &m_GridDepthSampler);

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
        }
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
