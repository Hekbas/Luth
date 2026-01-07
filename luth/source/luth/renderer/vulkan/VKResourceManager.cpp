#include "luthpch.h"
#include "luth/renderer/vulkan/VKResourceManager.h"
#include "luth/renderer/vulkan/VKCommon.h"

namespace Luth
{
    VKResourceManager::VKResourceManager(VkDevice device, VkPhysicalDevice physicalDevice)
        : m_Device(device), m_PhysicalDevice(physicalDevice)
    {
    }

    VKResourceManager::~VKResourceManager()
    {
        // Destroy all pooled resources
        for (auto& [hash, entries] : m_TexturePool)
        {
            for (auto& entry : entries)
            {
                vkDestroyImageView(m_Device, entry.resource.view, nullptr);
                vkDestroyImage(m_Device, entry.resource.image, nullptr);
                vkFreeMemory(m_Device, entry.resource.memory, nullptr);
            }
        }
    }

    VKImageResource* VKResourceManager::GetTexture(const RG::TextureDesc& desc)
    {
        // Simple hash for pooling (width/height/format)
        u64 hash = (u64)desc.width << 32 | (u64)desc.height << 16 | (u64)desc.format;

        auto& pool = m_TexturePool[hash];
        
        // Try to find a free resource
        if (!pool.empty())
        {
            VKImageResource* res = &pool.back().resource;
            // In a real pool, we'd pop it or mark it as used.
            // For this simple implementation, we assume the caller manages lifetime 
            // via ReleaseTexture (which pushes it back).
            // Wait, this logic is flawed for a pool. We need to move it out of the pool.
            
            // Correct logic:
            // We need a separate "Active" list and "Free" list.
            // For now, let's just create new if empty, and Release puts it into pool.
        }

        // Actually, let's implement the "Free List" properly.
        // The pool contains FREE resources.
        if (!pool.empty())
        {
            VKImageResource* res = new VKImageResource(pool.back().resource); // Copy out
            pool.pop_back();
            return res; // Caller owns this pointer for the frame? No, that leaks.
            // Let's return a pointer to a persistent object?
            // Better: Return by value or smart pointer?
            // Let's return a raw pointer to a heap allocated struct for now, 
            // and ReleaseTexture puts it back into the pool (and deletes the struct wrapper).
        }

        // Create new
        VKImageResource* newRes = new VKImageResource();
        CreateImage(desc, *newRes);
        return newRes;
    }

    void VKResourceManager::ReleaseTexture(VKImageResource* resource)
    {
        if (!resource) return;

        // Calculate hash again to put back in correct pool
        // We need to store the desc or hash in the resource to do this efficiently.
        // For now, we assume we can reconstruct or just store it.
        // Let's assume we just destroy it for this prototype to avoid leaks, 
        // until we implement a proper pool.
        
        // PROTOTYPE: Destroy immediately to be safe.
        vkDestroyImageView(m_Device, resource->view, nullptr);
        vkDestroyImage(m_Device, resource->image, nullptr);
        vkFreeMemory(m_Device, resource->memory, nullptr);
        delete resource;
    }

    void VKResourceManager::GarbageCollect()
    {
        m_CurrentFrame++;
        // Evict old resources
    }

    VkFormat VKResourceManager::ConvertFormat(RG::TextureFormat format)
    {
        switch (format)
        {
            case RG::TextureFormat::RGBA8_Unorm:       return VK_FORMAT_R8G8B8A8_UNORM;
            case RG::TextureFormat::RGBA16_Float:      return VK_FORMAT_R16G16B16A16_SFLOAT;
            case RG::TextureFormat::RGBA32_Float:      return VK_FORMAT_R32G32B32A32_SFLOAT;
            case RG::TextureFormat::D32_Float:         return VK_FORMAT_D32_SFLOAT;
            case RG::TextureFormat::D24_Unorm_S8_Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
            default: return VK_FORMAT_UNDEFINED;
        }
    }

    void VKResourceManager::CreateImage(const RG::TextureDesc& desc, VKImageResource& outResource)
    {
        outResource.format = ConvertFormat(desc.format);
        outResource.extent = { desc.width, desc.height, desc.depth };
        outResource.name = desc.name;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = outResource.extent;
        imageInfo.mipLevels = desc.mipLevels;
        imageInfo.arrayLayers = desc.arrayLayers;
        imageInfo.format = outResource.format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        
        // Usage flags: Assume everything can be Sampled, Transfer, and Attachment for now
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | 
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        
        if (desc.format == RG::TextureFormat::D32_Float || desc.format == RG::TextureFormat::D24_Unorm_S8_Uint)
        {
            imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        }

        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        VK_CHECK_RESULT(vkCreateImage(m_Device, &imageInfo, nullptr, &outResource.image), "Failed to create RG image");

        // Allocate Memory
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(m_Device, outResource.image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = VKUtils::FindMemoryType(m_PhysicalDevice, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VK_CHECK_RESULT(vkAllocateMemory(m_Device, &allocInfo, nullptr, &outResource.memory), "Failed to allocate RG image memory");
        vkBindImageMemory(m_Device, outResource.image, outResource.memory, 0);

        // Create View
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = outResource.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = outResource.format;
        
        if (imageInfo.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        else
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = desc.mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = desc.arrayLayers;

        VK_CHECK_RESULT(vkCreateImageView(m_Device, &viewInfo, nullptr, &outResource.view), "Failed to create RG image view");
    }
}
