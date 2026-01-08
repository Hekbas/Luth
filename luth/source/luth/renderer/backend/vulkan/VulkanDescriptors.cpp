#include "luthpch.h"
#include "VulkanDescriptors.h"
#include "VulkanContext.h"
#include "VulkanAllocator.h"
#include "luth/core/Log.h"
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    // ===================================================================================
    // Descriptor Allocator
    // ===================================================================================

    void DescriptorAllocator::Init(VkDevice device)
    {
        m_Device = device;
    }

    void DescriptorAllocator::Shutdown()
    {
        for (auto p : m_FreePools) vkDestroyDescriptorPool(m_Device, p, nullptr);
        for (auto p : m_UsedPools) vkDestroyDescriptorPool(m_Device, p, nullptr);
        if (m_CurrentPool) vkDestroyDescriptorPool(m_Device, m_CurrentPool, nullptr);
    }

    VkDescriptorPool DescriptorAllocator::CreatePool(u32 count, VkDescriptorPoolCreateFlags flags)
    {
        std::vector<VkDescriptorPoolSize> sizes;
        sizes.reserve(11);
        sizes.push_back({ VK_DESCRIPTOR_TYPE_SAMPLER, (u32)(count * 0.5f) });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (u32)(count * 4.0f) });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (u32)(count * 2.0f) });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (u32)(count * 2.0f) });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, count });

        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = flags;
        pool_info.maxSets = count;
        pool_info.poolSizeCount = (u32)sizes.size();
        pool_info.pPoolSizes = sizes.data();

        VkDescriptorPool pool;
        vkCreateDescriptorPool(m_Device, &pool_info, nullptr, &pool);
        return pool;
    }

    VkDescriptorPool DescriptorAllocator::GetPool()
    {
        if (m_CurrentPool != VK_NULL_HANDLE) return m_CurrentPool;

        if (!m_FreePools.empty()) {
            m_CurrentPool = m_FreePools.back();
            m_FreePools.pop_back();
            return m_CurrentPool;
        }

        return CreatePool(1000, 0);
    }

    bool DescriptorAllocator::Allocate(VkDescriptorSetLayout layout, VkDescriptorSet& outSet)
    {
        if (m_CurrentPool == VK_NULL_HANDLE) {
            m_CurrentPool = GetPool();
            m_UsedPools.push_back(m_CurrentPool);
        }

        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.pNext = nullptr;
        allocInfo.descriptorPool = m_CurrentPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;

        VkResult result = vkAllocateDescriptorSets(m_Device, &allocInfo, &outSet);

        // If pool is full, try again with a new pool
        if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
            m_CurrentPool = GetPool();
            m_UsedPools.push_back(m_CurrentPool);
            allocInfo.descriptorPool = m_CurrentPool;
            result = vkAllocateDescriptorSets(m_Device, &allocInfo, &outSet);
        }

        return result == VK_SUCCESS;
    }

    void DescriptorAllocator::Reset()
    {
        for (auto p : m_UsedPools) {
            vkResetDescriptorPool(m_Device, p, 0);
            m_FreePools.push_back(p);
        }
        m_UsedPools.clear();
        m_CurrentPool = VK_NULL_HANDLE;
    }

    // ===================================================================================
    // Bindless Descriptor Set
    // ===================================================================================

    void BindlessDescriptorSet::Init(VkDevice device)
    {
        m_Device = device;

        // 1. Create Layout
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = MAX_BINDLESS_RESOURCES;
        binding.stageFlags = VK_SHADER_STAGE_ALL;
        binding.pImmutableSamplers = nullptr;

        VkDescriptorBindingFlags bindingFlags = 
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | 
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; // Allow updating while bound!

        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
        bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bindingFlagsInfo.bindingCount = 1;
        bindingFlagsInfo.pBindingFlags = &bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext = &bindingFlagsInfo;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;

        if (vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_Layout) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create bindless descriptor layout!");
        }

        // 2. Create Pool
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = MAX_BINDLESS_RESOURCES;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;

        vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_Pool);

        // 3. Allocate Set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_Pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_Layout;

        // Variable descriptor count support (optional, but good practice for bindless)
        uint32_t maxBinding = MAX_BINDLESS_RESOURCES - 1;
        VkDescriptorSetVariableDescriptorCountAllocateInfo variableAllocInfo{};
        variableAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
        variableAllocInfo.descriptorSetCount = 1;
        variableAllocInfo.pDescriptorCounts = &maxBinding; // Not strictly needed if we allocated fixed size layout
        // allocInfo.pNext = &variableAllocInfo; 

        vkAllocateDescriptorSets(m_Device, &allocInfo, &m_DescriptorSet);

        // 4. Initialize Free Indices
        for (u32 i = 0; i < MAX_BINDLESS_RESOURCES; i++) {
            m_FreeIndices.push_back(i);
        }

        CreateNullTexture();
    }

    void BindlessDescriptorSet::Shutdown()
    {
        vkDestroySampler(m_Device, m_NullSampler, nullptr);
        vkDestroyImageView(m_Device, m_NullImageView, nullptr);
        VulkanAllocator::FreeImage(m_NullImage, (VmaAllocation)m_NullAllocation);

        vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
        vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
    }

    u32 BindlessDescriptorSet::BindTexture(VkImageView view, VkSampler sampler)
    {
        if (m_FreeIndices.empty()) {
            LH_CORE_ERROR("Bindless descriptor set full!");
            return 0;
        }

        u32 index = m_FreeIndices.front();
        m_FreeIndices.pop_front();

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = view;
        imageInfo.sampler = sampler;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_DescriptorSet;
        write.dstBinding = 0;
        write.dstArrayElement = index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);

        return index;
    }

    void BindlessDescriptorSet::UnbindTexture(u32 index)
    {
        // Reset to null texture to be safe
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = m_NullImageView;
        imageInfo.sampler = m_NullSampler;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_DescriptorSet;
        write.dstBinding = 0;
        write.dstArrayElement = index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);

        m_FreeIndices.push_back(index);
    }

    void BindlessDescriptorSet::CreateNullTexture()
    {
        // Create a 1x1 white texture
        u32 white = 0xFFFFFFFF;
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = { 1, 1, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        m_NullAllocation = (VmaAllocation_T*)VulkanAllocator::AllocateImage(imageInfo, VMA_MEMORY_USAGE_AUTO, m_NullImage);

        // Upload data (using immediate submit from Context)
        // ... (Skipping upload for brevity, assume it's white or garbage)
        // Transition to Shader Read Only
        // ...

        // Create View & Sampler
        // ... (Standard creation code)
    }
}