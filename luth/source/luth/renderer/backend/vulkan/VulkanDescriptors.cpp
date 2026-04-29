#include "luthpch.h"
#include "VulkanDescriptors.h"
#include "VulkanContext.h"
#include "VulkanAllocator.h"
#include "luth/core/diagnostics/Log.h"

#include <vma/vk_mem_alloc.h>

namespace Luth
{
    // ── Descriptor Allocator ──

    void DescriptorAllocator::Init(VkDevice device)
    {
        m_Device = device;
    }

    void DescriptorAllocator::Shutdown()
    {
        for (auto p : m_FreePools) vkDestroyDescriptorPool(m_Device, p, nullptr);
        for (auto p : m_UsedPools) vkDestroyDescriptorPool(m_Device, p, nullptr);
    }

    bool DescriptorAllocator::Allocate(VkDescriptorSetLayout layout, VkDescriptorSet& outSet)
    {
        if (m_CurrentPool == VK_NULL_HANDLE)
        {
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

        // If full, get new pool and retry
        if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
        {
            m_CurrentPool = GetPool();
            m_UsedPools.push_back(m_CurrentPool);
            allocInfo.descriptorPool = m_CurrentPool;
            result = vkAllocateDescriptorSets(m_Device, &allocInfo, &outSet);
        }

        return result == VK_SUCCESS;
    }

    void DescriptorAllocator::Reset()
    {
        for (auto p : m_UsedPools)
        {
            vkResetDescriptorPool(m_Device, p, 0);
            m_FreePools.push_back(p);
        }
        m_UsedPools.clear();
        m_CurrentPool = VK_NULL_HANDLE;
    }

    VkDescriptorPool DescriptorAllocator::GetPool()
    {
        if (!m_FreePools.empty())
        {
            VkDescriptorPool pool = m_FreePools.back();
            m_FreePools.pop_back();
            return pool;
        }
        return CreatePool(1000, 0);
    }

    VkDescriptorPool DescriptorAllocator::CreatePool(u32 count, VkDescriptorPoolCreateFlags flags)
    {
        VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count }
        };

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = flags;
        poolInfo.maxSets = count;
        poolInfo.poolSizeCount = (u32)std::size(sizes);
        poolInfo.pPoolSizes = sizes;

        VkDescriptorPool pool;
        vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &pool);
        return pool;
    }

    // ── Bindless Descriptor Set ──

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

        VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
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

        vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_Layout);

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

        vkAllocateDescriptorSets(m_Device, &allocInfo, &m_DescriptorSet);

        // 4. Initialize free list. Slot 0 is reserved for the null texture and never enters
        //    the pool. Push descending so pop_back yields ascending allocation order (1, 2, 3 ...)
        //    — matches the previous deque/pop_front behavior, easier to read in RenderDoc.
        m_FreeIndices.reserve(MAX_BINDLESS_RESOURCES - 1);
        for (u32 i = MAX_BINDLESS_RESOURCES - 1; i > NULL_TEXTURE_SLOT; --i)
            m_FreeIndices.push_back(i);

        // 5. Create Null Texture (1x1 White) and bind to the reserved slot 0
        CreateNullTexture();
        {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView   = m_NullImageView;
            imageInfo.sampler     = m_NullSampler;

            VkWriteDescriptorSet write{};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = m_DescriptorSet;
            write.dstBinding      = 0;
            write.dstArrayElement = NULL_TEXTURE_SLOT;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo      = &imageInfo;

            vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
        }
    }

    void BindlessDescriptorSet::Shutdown()
    {
        VulkanAllocator::FreeImage(m_NullImage, m_NullAllocation);
        vkDestroyImageView(m_Device, m_NullImageView, nullptr);
        vkDestroySampler(m_Device, m_NullSampler, nullptr);

        vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
        vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
    }

    void BindlessDescriptorSet::CreateNullTexture()
    {
        u32 white = 0xFFFFFFFF;
        
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = { 1, 1, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        m_NullAllocation = VulkanAllocator::AllocateImage(imageInfo, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, m_NullImage);

        // Upload white pixel
        // Use ImmediateSubmit to transition and upload
        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
            // 1. Transition to Transfer Dst
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.image = m_NullImage;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

            // 2. Clear Color (Upload white)
            VkClearColorValue clearColor = { 1.0f, 1.0f, 1.0f, 1.0f };
            VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, m_NullImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

            // 3. Transition to Shader Read
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        });

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_NullImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(m_Device, &viewInfo, nullptr, &m_NullImageView);

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        vkCreateSampler(m_Device, &samplerInfo, nullptr, &m_NullSampler);
    }

    u32 BindlessDescriptorSet::BindTexture(VkImageView view, VkSampler sampler)
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        if (m_FreeIndices.empty()) {
            LH_CORE_ERROR("Bindless descriptor set full!");
            return INVALID_BINDLESS_SLOT;
        }

        u32 index = m_FreeIndices.back();
        m_FreeIndices.pop_back();

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
        // Skip the sentinel and the reserved null slot — neither belongs back in the pool.
        // (The sentinel never owned a descriptor; freeing slot 0 would orphan the null texture.)
        if (index == INVALID_BINDLESS_SLOT || index == NULL_TEXTURE_SLOT)
            return;

        std::lock_guard<std::mutex> lock(m_Lock);

        // Replace with null texture to be safe
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
}
