#include "luthpch.h"
#include "VulkanDescriptors.h"
#include "VulkanContext.h"
#include "VulkanAllocator.h"
#include "luth/core/diagnostics/Log.h"

#include <vma/vk_mem_alloc.h>

namespace Luth
{
    // ── Bindless Descriptor Set ──

    void BindlessDescriptorSet::Init(VkDevice device)
    {
        m_Device = device;

        // 1. Layout — two bindings on the same set. Both partial-bound + UAB so writes can
        //    overlap in-flight reads (binding 0 retires textures via UploadContext's fence pump;
        //    binding 1 ad-hoc samplers may register from any thread post-Init).
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = MAX_BINDLESS_RESOURCES;
        bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[1].descriptorCount = MAX_BINDLESS_SAMPLERS;
        bindings[1].stageFlags = VK_SHADER_STAGE_ALL;

        VkDescriptorBindingFlags bindingFlags[2] = {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
        bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bindingFlagsInfo.bindingCount = 2;
        bindingFlagsInfo.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext = &bindingFlagsInfo;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;

        vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_Layout);

        // 2. Pool — one entry per descriptor type.
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = MAX_BINDLESS_RESOURCES;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
        poolSizes[1].descriptorCount = MAX_BINDLESS_SAMPLERS;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;

        vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_Pool);

        // 3. Allocate Set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_Pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_Layout;

        vkAllocateDescriptorSets(m_Device, &allocInfo, &m_DescriptorSet);

        // 4. Initialize free lists. Slot 0 of binding 0 is reserved for the null texture and
        //    never enters the pool. The canonical sampler block occupies slots 0..N-1 of binding 1
        //    and is also out of the LIFO. Both push descending so pop_back yields ascending
        //    allocation order — easier to read in RenderDoc.
        m_FreeIndices.reserve(MAX_BINDLESS_RESOURCES - 1);
        for (u32 i = MAX_BINDLESS_RESOURCES - 1; i > NULL_TEXTURE_SLOT; --i)
            m_FreeIndices.push_back(i);

        m_FreeSamplerIndices.reserve(MAX_BINDLESS_SAMPLERS - NUM_CANONICAL_SAMPLERS);
        for (u32 i = MAX_BINDLESS_SAMPLERS - 1; i >= NUM_CANONICAL_SAMPLERS; --i)
            m_FreeSamplerIndices.push_back(i);

        // 5. Null texture + canonical samplers.
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
        CreateCanonicalSamplers();
    }

    void BindlessDescriptorSet::Shutdown()
    {
        VulkanAllocator::FreeImage(m_NullImage, m_NullAllocation);
        vkDestroyImageView(m_Device, m_NullImageView, nullptr);
        vkDestroySampler(m_Device, m_NullSampler, nullptr);

        for (u32 i = 0; i < NUM_CANONICAL_SAMPLERS; ++i)
        {
            if (m_CanonicalSamplers[i] != VK_NULL_HANDLE)
                vkDestroySampler(m_Device, m_CanonicalSamplers[i], nullptr);
        }

        vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
        vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
    }

    void BindlessDescriptorSet::CreateCanonicalSamplers()
    {
        const float maxAniso = VulkanContext::Get().GetPhysicalDeviceProperties().limits.maxSamplerAnisotropy;

        auto MakeSampler = [&](VkFilter filt, VkSamplerMipmapMode mip, VkSamplerAddressMode addr, bool aniso) -> VkSampler {
            VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            info.magFilter    = filt;
            info.minFilter    = filt;
            info.mipmapMode   = mip;
            info.addressModeU = addr;
            info.addressModeV = addr;
            info.addressModeW = addr;
            info.maxLod       = VK_LOD_CLAMP_NONE;
            if (aniso)
            {
                info.anisotropyEnable = VK_TRUE;
                info.maxAnisotropy    = maxAniso;
            }
            info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
            VkSampler s = VK_NULL_HANDLE;
            vkCreateSampler(m_Device, &info, nullptr, &s);
            return s;
        };

        m_CanonicalSamplers[(u32)CanonicalSampler::LinearRepeatAnisoMip] =
            MakeSampler(VK_FILTER_LINEAR,  VK_SAMPLER_MIPMAP_MODE_LINEAR,  VK_SAMPLER_ADDRESS_MODE_REPEAT,        true);
        m_CanonicalSamplers[(u32)CanonicalSampler::LinearClampAnisoMip]  =
            MakeSampler(VK_FILTER_LINEAR,  VK_SAMPLER_MIPMAP_MODE_LINEAR,  VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, true);
        m_CanonicalSamplers[(u32)CanonicalSampler::NearestRepeatNoMip]   =
            MakeSampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT,        false);
        m_CanonicalSamplers[(u32)CanonicalSampler::NearestClampNoMip]    =
            MakeSampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false);

        for (u32 i = 0; i < NUM_CANONICAL_SAMPLERS; ++i)
            WriteSamplerSlot(i, m_CanonicalSamplers[i]);
    }

    void BindlessDescriptorSet::WriteSamplerSlot(u32 index, VkSampler sampler)
    {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = sampler;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = m_DescriptorSet;
        write.dstBinding      = 1;
        write.dstArrayElement = index;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imageInfo;

        vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
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

    u32 BindlessDescriptorSet::BindSampler(VkSampler sampler)
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        if (m_FreeSamplerIndices.empty())
        {
            LH_CORE_ERROR("Bindless sampler array full!");
            return INVALID_BINDLESS_SLOT;
        }

        u32 index = m_FreeSamplerIndices.back();
        m_FreeSamplerIndices.pop_back();
        WriteSamplerSlot(index, sampler);
        return index;
    }

    void BindlessDescriptorSet::UnbindSampler(u32 index)
    {
        // Sentinel and the canonical block (slots 0..N-1) are not vended by BindSampler and
        // must not return to the LIFO. Canonical slots are owned for the lifetime of the device.
        if (index == INVALID_BINDLESS_SLOT || index < NUM_CANONICAL_SAMPLERS)
            return;

        std::lock_guard<std::mutex> lock(m_Lock);
        // Restore the LinearRepeatAnisoMip canonical as the safe fallback — partial-bound covers
        // the "never sampled" case, but a defined value beats an unspecified one for renderdoc.
        WriteSamplerSlot(index, m_CanonicalSamplers[(u32)CanonicalSampler::LinearRepeatAnisoMip]);
        m_FreeSamplerIndices.push_back(index);
    }
}
