#include "luthpch.h"
#include "VulkanTexture.h"
#include "VulkanContext.h"
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    static VkFormat LuthFormatToVulkan(TextureFormat format)
    {
        switch (format) {
            case TextureFormat::RGBA8:  return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::RGB8:   return VK_FORMAT_R8G8B8A8_UNORM; // Force 4 channel for alignment
            default: return VK_FORMAT_R8G8B8A8_UNORM;
        }
    }

    VKTexture::VKTexture(const fs::path& path)
        : m_Path(path)
    {
        int width, height, channels;
        stbi_set_flip_vertically_on_load(1);
        stbi_uc* data = stbi_load(path.string().c_str(), &width, &height, &channels, 4); // Force 4 channels
        
        if (!data) {
            LH_CORE_ERROR("Failed to load texture: {0}", path.string());
            return;
        }

        m_Width = width;
        m_Height = height;
        m_Format = TextureFormat::RGBA8;

        CreateImage(data);
        CreateViewAndSampler();
        m_BindlessIndex = VulkanContext::Get().GetBindlessSet().BindTexture(m_ImageView, m_Sampler);

        stbi_image_free(data);
    }

    VKTexture::VKTexture(u32 width, u32 height, TextureFormat format, const void* data)
        : m_Width(width), m_Height(height), m_Format(format)
    {
        CreateImage(data);
        CreateViewAndSampler();
        m_BindlessIndex = VulkanContext::Get().GetBindlessSet().BindTexture(m_ImageView, m_Sampler);
    }

    VKTexture::~VKTexture()
    {
        VulkanContext::Get().GetBindlessSet().UnbindTexture(m_BindlessIndex);
        
        VulkanContext::Get().PushDeletion([img = m_Image, alloc = m_Allocation, view = m_ImageView, samp = m_Sampler]() {
            VkDevice device = VulkanContext::Get().GetDevice();
            vkDestroySampler(device, samp, nullptr);
            vkDestroyImageView(device, view, nullptr);
            VulkanAllocator::FreeImage(img, alloc);
        });
    }

    void VKTexture::CreateImage(const void* data)
    {
        VkDeviceSize imageSize = m_Width * m_Height * 4; // Assuming RGBA8

        // 1. Create Staging Buffer
        VkBufferCreateInfo stagingInfo = {};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = imageSize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VkBuffer stagingBuffer;
        VmaAllocation stagingAlloc = VulkanAllocator::AllocateBuffer(stagingInfo, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer);

        void* mappedData = VulkanAllocator::Map(stagingAlloc);
        memcpy(mappedData, data, static_cast<size_t>(imageSize));
        VulkanAllocator::Unmap(stagingAlloc);

        // 2. Create Image
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = m_Width;
        imageInfo.extent.height = m_Height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = LuthFormatToVulkan(m_Format);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        m_Allocation = VulkanAllocator::AllocateImage(imageInfo, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, m_Image);

        // 3. Transition and Copy
        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
            // Transition Undefined -> Transfer Dst
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = m_Image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

            // Copy Buffer to Image
            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = { 0, 0, 0 };
            region.imageExtent = { m_Width, m_Height, 1 };

            vkCmdCopyBufferToImage(cmd, stagingBuffer, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            // Transition Transfer Dst -> Shader Read Only
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        });

        VulkanAllocator::FreeBuffer(stagingBuffer, stagingAlloc);
    }

    void VKTexture::CreateViewAndSampler()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // View
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = LuthFormatToVulkan(m_Format);
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(device, &viewInfo, nullptr, &m_ImageView);

        // Sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = VulkanContext::Get().GetPhysicalDeviceProperties().limits.maxSamplerAnisotropy;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

        vkCreateSampler(device, &samplerInfo, nullptr, &m_Sampler);
    }

    void VKTexture::Bind(u32 slot) const {}
    void VKTexture::SetWrapMode(TextureWrapMode mode) {}
    void VKTexture::SetFilterMode(TextureFilterMode min, TextureFilterMode mag) {}
    std::string VKTexture::GetFormatString() const { return "RGBA8"; }
}