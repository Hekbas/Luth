#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/core/FrameData.h"
#include "luth/core/time/Time.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

namespace Luth
{
    // Shared PP / outline / grid state: UBO (scalar settings), samplers,
    // descriptor-set layouts. Per-view bloom textures + descriptor sets
    // are allocated by EnsureViewResources.
    void RenderPipeline::InitPostProcessResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // PostProcess UBO storage is allocated per render-stage from GPUTaggedPageAllocator
        // (see UpdatePostProcessUBO). Nothing to allocate up front.

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

        // Binding 2 (shared PostProcess UBO) is rebound per render-stage to a fresh tagged-heap region.
        VkDescriptorBindingFlags ppBindingFlags[3] = { 0, 0, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT };
        VkDescriptorSetLayoutBindingFlagsCreateInfo ppBindingFlagsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        ppBindingFlagsInfo.bindingCount  = 3;
        ppBindingFlagsInfo.pBindingFlags = ppBindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext = &ppBindingFlagsInfo;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_PPDescSetLayout);

        // Outline: 3 combined image samplers (mask, selection depth, scene depth).
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

        // Grid: binding 0 = per-view GlobalUBO (viewProj), 1 = scene depth sampler.
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

            // Binding 0 (per-view GlobalUBO viewProj) shares lifetime with Set 0 binding 0
            // — rebound per render-stage to the same fresh tagged-heap region.
            VkDescriptorBindingFlags gridBindingFlags[2] = { VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0 };
            VkDescriptorSetLayoutBindingFlagsCreateInfo gridBindingFlagsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            gridBindingFlagsInfo.bindingCount  = 2;
            gridBindingFlagsInfo.pBindingFlags = gridBindingFlags;

            VkDescriptorSetLayoutCreateInfo gridLayoutInfo{};
            gridLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            gridLayoutInfo.pNext = &gridBindingFlagsInfo;
            gridLayoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            gridLayoutInfo.bindingCount = 2;
            gridLayoutInfo.pBindings = gridBindings;
            vkCreateDescriptorSetLayout(device, &gridLayoutInfo, nullptr, &m_GridDescSetLayout);
        }
    }

    void RenderPipeline::UpdatePostProcessUBO()
    {
        if (!m_CurrentViewResources || m_CurrentViewResources->compositeDescSet == VK_NULL_HANDLE) return;

        PostProcessUBO ubo{};
        ubo.bloomThreshold      = m_System.GetPostProcessSettings().bloomThreshold;
        ubo.bloomStrength       = m_System.GetPostProcessSettings().bloomStrength;
        ubo.exposure            = m_System.GetPostProcessSettings().exposure;
        ubo.contrast            = m_System.GetPostProcessSettings().contrast;
        ubo.saturation          = m_System.GetPostProcessSettings().saturation;
        ubo.tonemapOp           = static_cast<int>(m_System.GetPostProcessSettings().tonemapOp);
        ubo.vignetteAmount      = m_System.GetPostProcessSettings().vignetteAmount;
        ubo.vignetteHardness    = m_System.GetPostProcessSettings().vignetteHardness;
        ubo.grainAmount         = m_System.GetPostProcessSettings().grainAmount;
        ubo.sharpness           = m_System.GetPostProcessSettings().sharpness;
        ubo.chromaticAberration = m_System.GetPostProcessSettings().chromaticAberration;
        ubo.time                = Time::GetTime();
        ubo.shadowBalance       = m_System.GetPostProcessSettings().shadowBalance;
        ubo.midtoneBalance      = m_System.GetPostProcessSettings().midtoneBalance;
        ubo.highlightBalance    = m_System.GetPostProcessSettings().highlightBalance;

        // Per-frame UBO from GPU tagged heap; the 4 PP descriptor sets share the region.
        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        jobCtx->GpuCache.CurrentTag = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());

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
            m_CurrentViewResources->bloomExtractDescSet,
            m_CurrentViewResources->bloomBlurHDescSet,
            m_CurrentViewResources->bloomBlurVDescSet,
            m_CurrentViewResources->compositeDescSet,
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
}
