#include "luthpch.h"
#include "RenderResourceCache.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/core/diagnostics/Log.h"
#include <vma/vk_mem_alloc.h>

namespace Luth::RG
{
    namespace
    {
        // Default usage for transient render targets: sampled + transfer + (color | depth).
        // Returned when caller passes desc.usage == 0. Compute-storage callers must specify.
        VkImageUsageFlags ResolveUsage(const TextureDesc& desc)
        {
            if (desc.usage != 0) return desc.usage;

            VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT
                                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (desc.format == TextureFormat::D32_Float || desc.format == TextureFormat::D24_Unorm_S8_Uint)
                u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            else
                u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            return u;
        }

        // Boost-style 64-bit hash combine. Cheap, well-mixed; multimap handles residual collisions.
        inline u64 HashCombine(u64 a, u64 b)
        {
            return a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2));
        }

        u64 HashKey(const TextureDesc& d)
        {
            u64 h = std::hash<u32>{}(d.width);
            h = HashCombine(h, std::hash<u32>{}(d.height));
            h = HashCombine(h, std::hash<u32>{}(static_cast<u32>(d.format)));
            h = HashCombine(h, std::hash<u32>{}(d.usage));
            return h;
        }

        // Final equality check in case two distinct tuples collided to the same hash.
        bool DescMatches(const TextureDesc& a, const TextureDesc& b)
        {
            return a.width == b.width
                && a.height == b.height
                && a.format == b.format
                && a.usage  == b.usage;
        }
    }

    void RenderResourceCache::Init()
    {
    }

    void RenderResourceCache::Shutdown()
    {
        for (auto& [_, res] : m_Pool)
        {
            vkDestroyImageView(VulkanContext::Get().GetDevice(), res.view, nullptr);
            VulkanAllocator::FreeImage(res.image, res.allocation);
        }
        m_Pool.clear();

        for (auto& buf : m_BufferPool)
            VulkanAllocator::FreeBuffer(buf.buffer, buf.allocation);
        m_BufferPool.clear();
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
            if (m_FrameIndex - it->second.lastUsedFrame > k_StaleFrameThreshold)
            {
                vkDestroyImageView(VulkanContext::Get().GetDevice(), it->second.view, nullptr);
                VulkanAllocator::FreeImage(it->second.image, it->second.allocation);
                it = m_Pool.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = m_BufferPool.begin(); it != m_BufferPool.end(); )
        {
            if (m_FrameIndex - it->lastUsedFrame > k_StaleFrameThreshold)
            {
                VulkanAllocator::FreeBuffer(it->buffer, it->allocation);
                it = m_BufferPool.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    PooledResource RenderResourceCache::GetTexture(const TextureDesc& desc)
    {
        TextureDesc resolved = desc;
        resolved.usage = ResolveUsage(desc);
        const u64 key = HashKey(resolved);

        // Bucket lookup: hash collapses 4-field comparison to one. The final DescMatches guards
        // against rare hash collisions producing a wrong-tuple match.
        auto range = m_Pool.equal_range(key);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (DescMatches(it->second.desc, resolved))
            {
                PooledResource res = it->second;
                m_Pool.erase(it);
                res.lastUsedFrame = m_FrameIndex;
                return res;
            }
        }

        // Create new if not found
        LH_CORE_WARN("Allocating new transient texture: {0} ({1}x{2})", desc.name, desc.width, desc.height);

        PooledResource res;
        res.desc = resolved;
        res.lastUsedFrame = m_FrameIndex;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = resolved.width;
        imageInfo.extent.height = resolved.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;

        // Map format
        if (resolved.format == TextureFormat::RGBA8_Unorm) imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        else if (resolved.format == TextureFormat::BGRA8_Unorm) imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        else if (resolved.format == TextureFormat::R8_Unorm) imageInfo.format = VK_FORMAT_R8_UNORM;
        else if (resolved.format == TextureFormat::RGBA16_Float) imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        else if (resolved.format == TextureFormat::RG16_Float) imageInfo.format = VK_FORMAT_R16G16_SFLOAT;
        else if (resolved.format == TextureFormat::R32_Float) imageInfo.format = VK_FORMAT_R32_SFLOAT;
        else if (resolved.format == TextureFormat::D32_Float) imageInfo.format = VK_FORMAT_D32_SFLOAT;
        else if (resolved.format == TextureFormat::D24_Unorm_S8_Uint) imageInfo.format = VK_FORMAT_D24_UNORM_S8_UINT;
        else if (resolved.format == TextureFormat::R16_Uint) imageInfo.format = VK_FORMAT_R16_UINT;
        else imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM; // Fallback

        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = resolved.usage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        // Transient images may end up as compute-pass outputs sampled by a later graphics pass — CONCURRENT keeps
        // those cross-queue cases legal without per-resource opt-in. Single-family GPUs collapse back to EXCLUSIVE.
        VulkanContext::Get().ApplyConcurrentSharing(imageInfo);

        res.allocation = VulkanAllocator::AllocateImage(imageInfo, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, res.image);

        // Create View
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = res.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageInfo.format;

        if (resolved.format == TextureFormat::D32_Float) viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        else if (resolved.format == TextureFormat::D24_Unorm_S8_Uint) viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
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
        resource.lastUsedFrame = m_FrameIndex;
        const u64 key = HashKey(resource.desc);
        m_Pool.emplace(key, resource);
    }

    PooledBuffer RenderResourceCache::GetBuffer(const BufferDesc& desc)
    {
        // Find a matching buffer in the pool (same size and usage)
        for (auto it = m_BufferPool.begin(); it != m_BufferPool.end(); ++it)
        {
            if (it->desc.size == desc.size && it->desc.usage == desc.usage)
            {
                PooledBuffer buf = *it;
                m_BufferPool.erase(it);
                buf.lastUsedFrame = m_FrameIndex;
                return buf;
            }
        }

        // Create new buffer
        LH_CORE_WARN("Allocating new transient buffer: {0} ({1} bytes)", desc.name, desc.size);

        PooledBuffer buf;
        buf.desc = desc;
        buf.lastUsedFrame = m_FrameIndex;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size  = desc.size;
        bufferInfo.usage = desc.usage;
        // Transient buffers (cluster light lists, particle SSBOs, etc.) may be written on compute and read on
        // graphics in the same frame. CONCURRENT covers the cross-queue case; single-family collapses to EXCLUSIVE.
        VulkanContext::Get().ApplyConcurrentSharing(bufferInfo);

        buf.allocation = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, buf.buffer);
        return buf;
    }

    void RenderResourceCache::ReturnBuffer(PooledBuffer buffer)
    {
        buffer.lastUsedFrame = m_FrameIndex;
        m_BufferPool.push_back(buffer);
    }
}
