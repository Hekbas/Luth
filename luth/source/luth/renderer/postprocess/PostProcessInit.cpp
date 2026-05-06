#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"

namespace Luth
{
    // Outline + Grid descriptor layouts/samplers stay on RenderPipeline through sub-task D;
    // sub-task E folds them into EditorOverlaysSubsystem and deletes this file.
    void RenderPipeline::InitOverlayResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Outline: 3 combined image samplers (mask, selection depth, scene depth).
        {
            VkSamplerCreateInfo outlineSamplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            outlineSamplerInfo.magFilter    = VK_FILTER_NEAREST;
            outlineSamplerInfo.minFilter    = VK_FILTER_NEAREST;
            outlineSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &outlineSamplerInfo, nullptr, &m_OutlineSampler);

            VkDescriptorSetLayoutBinding outBindings[3] = {};
            for (u32 i = 0; i < 3; ++i) {
                outBindings[i].binding = i;
                outBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                outBindings[i].descriptorCount = 1;
                outBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }

            VkDescriptorSetLayoutCreateInfo outLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            outLayoutInfo.bindingCount = 3;
            outLayoutInfo.pBindings    = outBindings;
            vkCreateDescriptorSetLayout(device, &outLayoutInfo, nullptr, &m_OutlineDescSetLayout);
        }

        // Grid: binding 0 = per-view GlobalUBO (shared with Set 0 binding 0; rebound in
        // GlobalSubsystem::UpdateUBO), binding 1 = scene depth sampler.
        {
            VkSamplerCreateInfo gridSamplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            gridSamplerInfo.magFilter    = VK_FILTER_NEAREST;
            gridSamplerInfo.minFilter    = VK_FILTER_NEAREST;
            gridSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
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

            // invariant: binding 0 (per-view GlobalUBO) shares lifetime with Set 0 binding 0
            // — rebound per render-stage to the same fresh tagged-heap region.
            VkDescriptorBindingFlags gridBindingFlags[2] = { VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0 };
            VkDescriptorSetLayoutBindingFlagsCreateInfo gridBindingFlagsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            gridBindingFlagsInfo.bindingCount  = 2;
            gridBindingFlagsInfo.pBindingFlags = gridBindingFlags;

            VkDescriptorSetLayoutCreateInfo gridLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            gridLayoutInfo.pNext        = &gridBindingFlagsInfo;
            gridLayoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            gridLayoutInfo.bindingCount = 2;
            gridLayoutInfo.pBindings    = gridBindings;
            vkCreateDescriptorSetLayout(device, &gridLayoutInfo, nullptr, &m_GridDescSetLayout);
        }
    }
}
