#include "luthpch.h"
#include "VulkanTexture.h"
#include "VulkanContext.h"
#include "VulkanAllocator.h"
#include "luth/core/Log.h"

#include <stb/stb_image.h>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    // -------------------------------------------------------------------------
    // Format helpers
    // -------------------------------------------------------------------------

    static VkFormat ToVkFormat(TextureFormat fmt)
    {
        switch (fmt)
        {
            case TextureFormat::R8:              return VK_FORMAT_R8_UNORM;
            case TextureFormat::RGB8:            return VK_FORMAT_R8G8B8_UNORM;
            case TextureFormat::RGBA8:           return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::RGBA16F:         return VK_FORMAT_R16G16B16A16_SFLOAT;
            case TextureFormat::RGBA32F:         return VK_FORMAT_R32G32B32A32_SFLOAT;
            case TextureFormat::RG16F:           return VK_FORMAT_R16G16_SFLOAT;
            case TextureFormat::D32_Float:       return VK_FORMAT_D32_SFLOAT;
            case TextureFormat::D24_Unorm_S8_Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
            case TextureFormat::R32_Uint:        return VK_FORMAT_R32_UINT;
            default:                             return VK_FORMAT_R8G8B8A8_UNORM;
        }
    }

    static bool IsDepthFormat(TextureFormat fmt)
    {
        return fmt == TextureFormat::D32_Float || fmt == TextureFormat::D24_Unorm_S8_Uint;
    }

    static VkSamplerAddressMode ToVkWrapMode(TextureWrapMode mode)
    {
        switch (mode)
        {
            case TextureWrapMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case TextureWrapMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case TextureWrapMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            default:                              return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }

    static VkFilter ToVkFilter(TextureFilterMode mode)
    {
        switch (mode)
        {
            case TextureFilterMode::Linear:
            case TextureFilterMode::LinearMipmapLinear:
                return VK_FILTER_LINEAR;
            case TextureFilterMode::Nearest:
            case TextureFilterMode::NearestMipmapNearest:
                return VK_FILTER_NEAREST;
            default:
                return VK_FILTER_LINEAR;
        }
    }

    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------

    VKTexture::VKTexture(const fs::path& path)
        : m_Path(path)
    {
        int width, height, channels;
        stbi_set_flip_vertically_on_load(1);
        stbi_uc* data = stbi_load(path.string().c_str(), &width, &height, &channels, 4);

        if (data)
        {
            m_Width = width;
            m_Height = height;
            m_Format = TextureFormat::RGBA8;

            // Default: generate mipmaps for textures loaded from file
            m_MipLevels = static_cast<u32>(std::floor(std::log2(std::max(m_Width, m_Height)))) + 1;

            CreateImage(data);
            CreateViewAndSampler();

            stbi_image_free(data);
        }
        else
        {
            LH_CORE_ERROR("Failed to load texture: {0}", path.string());
        }
    }

    VKTexture::VKTexture(u32 width, u32 height, TextureFormat format, const void* data)
        : m_Width(width), m_Height(height), m_Format(format)
    {
        // No settings: render targets / depth textures stay at mipLevels = 1
        CreateImage(data);
        CreateViewAndSampler();
    }

    VKTexture::VKTexture(u32 width, u32 height, TextureFormat format, const void* data, const TextureSettings& settings)
        : m_Width(width), m_Height(height), m_Format(format),
          m_WrapMode(settings.WrapMode), m_MinFilter(settings.MinFilter), m_MagFilter(settings.MagFilter)
    {
        if (settings.GenerateMipmaps && data && !IsDepthFormat(format))
            m_MipLevels = static_cast<u32>(std::floor(std::log2(std::max(m_Width, m_Height)))) + 1;

        CreateImage(data);
        CreateViewAndSampler();
    }

    VKTexture::VKTexture(u32 width, u32 height, TextureFormat format, u32 arrayLayers,
                         VkImageCreateFlags createFlags, u32 mipLevels, VkImageUsageFlags extraUsage)
        : m_Width(width), m_Height(height), m_Format(format),
          m_ArrayLayers(arrayLayers), m_CreateFlags(createFlags), m_MipLevels(mipLevels), m_ExtraUsage(extraUsage)
    {
        CreateImage(nullptr);
        CreateViewAndSampler();
    }

    VKTexture::~VKTexture()
    {
        // Unbind from global set (only color textures are in bindless)
        if (m_BindlessIndex != 0)
            VulkanContext::Get().GetBindlessSet().UnbindTexture(m_BindlessIndex);

        VulkanContext::Get().PushDeletion([img = m_Image, view = m_ImageView, samp = m_Sampler, alloc = m_Allocation]() {
            VkDevice device = VulkanContext::Get().GetDevice();
            if (samp) vkDestroySampler(device, samp, nullptr);
            vkDestroyImageView(device, view, nullptr);
            VulkanAllocator::FreeImage(img, alloc);
        });
    }

    void VKTexture::Bind(u32 slot) const
    {
        // No-op for bindless
    }

    // -------------------------------------------------------------------------
    // CreateImage
    // -------------------------------------------------------------------------

    void VKTexture::CreateImage(const void* data)
    {
        const bool isDepth = IsDepthFormat(m_Format);
        VkFormat vkFmt = ToVkFormat(m_Format);

        // Check blit support for mipmap generation
        if (m_MipLevels > 1)
        {
            VkFormatProperties formatProps;
            vkGetPhysicalDeviceFormatProperties(VulkanContext::Get().GetPhysicalDevice(), vkFmt, &formatProps);
            bool canBlit = (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)
                        && (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
            if (!canBlit)
            {
                LH_CORE_WARN("VKTexture: Format does not support blit, falling back to mipLevels=1");
                m_MipLevels = 1;
            }
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = m_Width;
        imageInfo.extent.height = m_Height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = m_MipLevels;
        imageInfo.arrayLayers = m_ArrayLayers;
        imageInfo.format = vkFmt;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.flags = m_CreateFlags;

        if (isDepth)
        {
            imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT;
        }
        else
        {
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT
                            | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        imageInfo.usage |= m_ExtraUsage;

        m_Allocation = VulkanAllocator::AllocateImage(imageInfo, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, m_Image);

        // Upload pixel data (color textures only; depth textures are never CPU-uploaded directly)
        if (data && !isDepth)
        {
            u32 bytesPerPixel = 4; // RGBA8, R8, etc. — stb always gives RGBA

            VkDeviceSize imageSize = (VkDeviceSize)m_Width * m_Height * bytesPerPixel;

            VkBuffer stagingBuffer;
            VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufferInfo.size = imageSize;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocation stagingAlloc = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer);

            void* mappedData = VulkanAllocator::Map(stagingAlloc);
            memcpy(mappedData, data, static_cast<size_t>(imageSize));
            VulkanAllocator::Unmap(stagingAlloc);

            VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                // Transition ALL mip levels to Transfer Dst
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = m_Image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = m_MipLevels;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = m_ArrayLayers;
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                // Copy mip 0 from staging buffer
                VkBufferImageCopy region{};
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = m_ArrayLayers;
                region.imageExtent = { m_Width, m_Height, 1 };

                vkCmdCopyBufferToImage(cmd, stagingBuffer, m_Image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                // Generate mip chain via vkCmdBlitImage
                if (m_MipLevels > 1)
                {
                    i32 mipWidth = static_cast<i32>(m_Width);
                    i32 mipHeight = static_cast<i32>(m_Height);

                    for (u32 i = 1; i < m_MipLevels; i++)
                    {
                        // Transition mip i-1: TRANSFER_DST → TRANSFER_SRC
                        barrier.subresourceRange.baseMipLevel = i - 1;
                        barrier.subresourceRange.levelCount = 1;
                        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

                        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &barrier);

                        // Blit from mip i-1 to mip i
                        VkImageBlit blit{};
                        blit.srcOffsets[0] = { 0, 0, 0 };
                        blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
                        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        blit.srcSubresource.mipLevel = i - 1;
                        blit.srcSubresource.baseArrayLayer = 0;
                        blit.srcSubresource.layerCount = m_ArrayLayers;

                        i32 nextWidth = mipWidth > 1 ? mipWidth / 2 : 1;
                        i32 nextHeight = mipHeight > 1 ? mipHeight / 2 : 1;

                        blit.dstOffsets[0] = { 0, 0, 0 };
                        blit.dstOffsets[1] = { nextWidth, nextHeight, 1 };
                        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        blit.dstSubresource.mipLevel = i;
                        blit.dstSubresource.baseArrayLayer = 0;
                        blit.dstSubresource.layerCount = m_ArrayLayers;

                        vkCmdBlitImage(cmd,
                            m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1, &blit, VK_FILTER_LINEAR);

                        // Transition mip i-1: TRANSFER_SRC → SHADER_READ_ONLY
                        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &barrier);

                        mipWidth = nextWidth;
                        mipHeight = nextHeight;
                    }

                    // Transition last mip level: TRANSFER_DST → SHADER_READ_ONLY
                    barrier.subresourceRange.baseMipLevel = m_MipLevels - 1;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
                }
                else
                {
                    // Single mip: just transition to Shader Read
                    barrier.subresourceRange.baseMipLevel = 0;
                    barrier.subresourceRange.levelCount = 1;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
                }
            });

            VulkanAllocator::FreeBuffer(stagingBuffer, stagingAlloc);
        }
        else
        {
            // No data (render target / shadow map): transition to a suitable initial layout.
            // Color render targets → SHADER_READ_ONLY_OPTIMAL (will be transitioned by RG as needed)
            // Depth textures → DEPTH_STENCIL_READ_ONLY_OPTIMAL (will be transitioned by RG as needed)
            VkImageAspectFlags aspect = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageLayout newLayout = isDepth
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkPipelineStageFlags dstStage = isDepth
                ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            VkAccessFlags dstAccess = isDepth
                ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                : VK_ACCESS_SHADER_READ_BIT;

            VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = newLayout;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = m_Image;
                barrier.subresourceRange.aspectMask = aspect;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = m_MipLevels;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = m_ArrayLayers;
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = dstAccess;

                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dstStage,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);
            });
        }
    }

    // -------------------------------------------------------------------------
    // CreateViewAndSampler
    // -------------------------------------------------------------------------

    void VKTexture::CreateViewAndSampler()
    {
        const bool isDepth = IsDepthFormat(m_Format);
        const bool isCubemap = (m_ArrayLayers == 6) && (m_CreateFlags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
        VkFormat vkFmt = ToVkFormat(m_Format);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Image;
        if (isCubemap)
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        else if (m_ArrayLayers > 1 && isDepth)
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        else
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = vkFmt;
        viewInfo.subresourceRange.aspectMask = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = m_MipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = m_ArrayLayers;

        vkCreateImageView(VulkanContext::Get().GetDevice(), &viewInfo, nullptr, &m_ImageView);

        // Depth textures don't get a generic sampler registered in bindless.
        // Their sampling (e.g. shadow PCF) is set up externally with a dedicated VkSampler.
        if (isDepth)
        {
            m_Sampler = VK_NULL_HANDLE;
            m_BindlessIndex = 0;
            return;
        }

        // Cubemap textures are bound to dedicated descriptor bindings, not bindless.
        if (isCubemap)
        {
            m_Sampler = VK_NULL_HANDLE;
            m_BindlessIndex = 0;
            return;
        }

        // Integer textures (e.g. R32_Uint entity ID buffer) need nearest filtering
        // and are not registered in bindless (sampled via dedicated descriptor).
        if (m_Format == TextureFormat::R32_Uint)
        {
            VkSamplerCreateInfo intSamplerInfo{};
            intSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            intSamplerInfo.magFilter = VK_FILTER_NEAREST;
            intSamplerInfo.minFilter = VK_FILTER_NEAREST;
            intSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            intSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            intSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            intSamplerInfo.anisotropyEnable = VK_FALSE;
            intSamplerInfo.unnormalizedCoordinates = VK_FALSE;
            intSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(VulkanContext::Get().GetDevice(), &intSamplerInfo, nullptr, &m_Sampler);
            m_BindlessIndex = 0;
            return;
        }

        VkSamplerAddressMode vkWrap = ToVkWrapMode(m_WrapMode);

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = ToVkFilter(m_MagFilter);
        samplerInfo.minFilter = ToVkFilter(m_MinFilter);
        samplerInfo.addressModeU = vkWrap;
        samplerInfo.addressModeV = vkWrap;
        samplerInfo.addressModeW = vkWrap;
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = VulkanContext::Get().GetPhysicalDeviceProperties().limits.maxSamplerAnisotropy;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = static_cast<float>(m_MipLevels);

        vkCreateSampler(VulkanContext::Get().GetDevice(), &samplerInfo, nullptr, &m_Sampler);

        // Register with Bindless Set
        m_BindlessIndex = VulkanContext::Get().GetBindlessSet().BindTexture(m_ImageView, m_Sampler);
    }

    VkImageView VKTexture::CreateLayerView(u32 layer) const
    {
        const bool isDepth = IsDepthFormat(m_Format);
        VkFormat vkFmt = ToVkFormat(m_Format);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = vkFmt;
        viewInfo.subresourceRange.aspectMask = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = m_MipLevels;
        viewInfo.subresourceRange.baseArrayLayer = layer;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(VulkanContext::Get().GetDevice(), &viewInfo, nullptr, &view);
        return view;
    }

    VkImageView VKTexture::CreateMipView(u32 mipLevel, bool forStorage) const
    {
        VkFormat vkFmt = ToVkFormat(m_Format);
        bool isCubemap = (m_ArrayLayers == 6) && (m_CreateFlags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Image;
        // Compute shaders use image2DArray (Dim=2D, Arrayed=1) → need VK_IMAGE_VIEW_TYPE_2D_ARRAY
        if (isCubemap && forStorage)
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        else if (isCubemap)
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        else
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = vkFmt;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = mipLevel;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = m_ArrayLayers;

        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(VulkanContext::Get().GetDevice(), &viewInfo, nullptr, &view);
        return view;
    }

    std::string VKTexture::GetFormatString() const
    {
        switch (m_Format)
        {
            case TextureFormat::RGBA8:   return "RGBA8";
            case TextureFormat::RGBA16F: return "RGBA16F";
            case TextureFormat::RGBA32F: return "RGBA32F";
            case TextureFormat::RG16F:   return "RG16F";
            case TextureFormat::D32_Float: return "D32_Float";
            case TextureFormat::R32_Uint:  return "R32_Uint";
            default: return "Unknown";
        }
    }

    void VKTexture::SetWrapMode(TextureWrapMode mode) {}
    void VKTexture::SetFilterMode(TextureFilterMode min, TextureFilterMode mag) {}
}
