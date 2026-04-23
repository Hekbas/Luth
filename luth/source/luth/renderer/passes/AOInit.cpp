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
    // Shared GTAO resources:
    //   - 3 descriptor-set layouts (prefilter / main / denoise).
    //   - Linear clamp sampler (m_GTAOSampler).
    //   - 3 compute pipelines (built from .comp SPIR-V).
    //
    // Per-view GTAO resources (allocated by EnsureViewResources in
    // ViewResources.cpp):
    //   - 4 half-res textures (linearDepth, rawAO, edges, final).
    //   - 3 descriptor sets, allocated from the view's descPool.
    //   - GTAOUBO buffer (invResolution depends on view dimensions).
    void RenderPipeline::InitAOResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear clamp sampler — shared across views.
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_GTAOSampler);

        // Prefilter layout: [sampler2D sceneDepth, image2D linearDepth]
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

            VkPushConstantRange pcRange{};
            pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcRange.offset     = 0;
            pcRange.size       = sizeof(i32) * 2 + sizeof(float) * 6;

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

        // Main layout: [sampler2D linearDepth, image2D rawAO, UBO]
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

            VkPushConstantRange pcRange{};
            pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcRange.offset     = 0;
            pcRange.size       = sizeof(float) * 4 + sizeof(u32) * 4;

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

        // Denoise layout: [sampler2D rawAO, sampler2D linDepth, image2D finalAO]
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
    }

    void RenderPipeline::UpdateGTAOUBO()
    {
        if (!m_CurrentViewResources || !m_CurrentViewResources->gtaoUBOBuffer) return;

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

        // Per-view half-res GTAO textures — derive full-res from them
        // (halfW*2, halfH*2). Exact enough for invResolution ratios.
        const auto& lin = m_CurrentViewResources->gtaoLinearDepth;
        const u32 halfW = lin ? lin->GetWidth()  : 1u;
        const u32 halfH = lin ? lin->GetHeight() : 1u;
        const u32 fullW = halfW * 2;
        const u32 fullH = halfH * 2;
        ubo.invResolution[0]     = 1.0f / float(halfW);
        ubo.invResolution[1]     = 1.0f / float(halfH);
        ubo.invFullResolution[0] = 1.0f / float(fullW);
        ubo.invFullResolution[1] = 1.0f / float(fullH);

        m_CurrentViewResources->gtaoUBOBuffer->SetData(&ubo, sizeof(GTAOUBO));
    }
}
