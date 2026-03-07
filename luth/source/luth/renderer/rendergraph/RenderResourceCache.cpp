#include "luthpch.h"
#include "RenderResourceCache.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/core/Log.h"
#include <vma/vk_mem_alloc.h>

namespace Luth::RG
{
    void RenderResourceCache::Init()
    {
    }

    void RenderResourceCache::Shutdown()
    {
        for (auto& res : m_Pool)
        {
            vkDestroyImageView(VulkanContext::Get().GetDevice(), res.view, nullptr);
            VulkanAllocator::FreeImage(res.image, res.allocation);
        }
        m_Pool.clear();
    }

    void RenderResourceCache::NewFrame()
    {
        m_FrameIndex++;
        PerformGarbageCollection();
    }

    void RenderResourceCache::PerformGarbageCollection()
    {
        for (auto it = m_Pool.begin(); it != m_Pool.end(); )
        {
            if (m_FrameIndex - it->lastUsedFrame > k_StaleFrameThreshold)
            {
                vkDestroyImageView(VulkanContext::Get().GetDevice(), it->view, nullptr);
                VulkanAllocator::FreeImage(it->image, it->allocation);
                it = m_Pool.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    PooledResource RenderResourceCache::GetTexture(const TextureDesc& desc)
    {
        // Find in pool
        for (auto it = m_Pool.begin(); it != m_Pool.end(); ++it)
        {
            if (it->desc.width == desc.width &&
                it->desc.height == desc.height &&
                it->desc.format == desc.format)
            {
                PooledResource res = *it;
                m_Pool.erase(it);
                res.lastUsedFrame = m_FrameIndex;
                return res;
            }
        }

        // Create new if not found
        LH_CORE_WARN("Allocating new transient texture: {0} ({1}x{2})", desc.name, desc.width, desc.height);
        
        PooledResource res;
        res.desc = desc;
        res.lastUsedFrame = m_FrameIndex;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = desc.width;
        imageInfo.extent.height = desc.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;

        // Map format
        if (desc.format == TextureFormat::RGBA8_Unorm) imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        else if (desc.format == TextureFormat::BGRA8_Unorm) imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        else if (desc.format == TextureFormat::D32_Float) imageInfo.format = VK_FORMAT_D32_SFLOAT;
        else if (desc.format == TextureFormat::D24_Unorm_S8_Uint) imageInfo.format = VK_FORMAT_D24_UNORM_S8_UINT;
        else imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM; // Fallback
        
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // Apply usage flags based on format to avoid validation errors
        if (desc.format == TextureFormat::D32_Float || desc.format == TextureFormat::D24_Unorm_S8_Uint)
            imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        else
            imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        res.allocation = VulkanAllocator::AllocateImage(imageInfo, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, res.image);

        // Create View
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = res.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageInfo.format;
        
        if (desc.format == TextureFormat::D32_Float) viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        else if (desc.format == TextureFormat::D24_Unorm_S8_Uint) viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        else viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(VulkanContext::Get().GetDevice(), &viewInfo, nullptr, &res.view);

        return res;
    }

    void RenderResourceCache::ReturnTexture(PooledResource resource)
    {
        resource.lastUsedFrame = m_FrameIndex; // Mark as used this frame
        m_Pool.push_back(resource);
    }
}
