#include "Luthpch.h"
#include "luth/renderer/vulkan/VKTexture.h"
#include "luth/renderer/vulkan/VKCommon.h"
#include "luth/renderer/vulkan/VKBuffer.h"
#include "luth/renderer/vulkan/VKCommandPool.h"
#include "luth/resources/Resource.h"
#include "luth/resources/FileSystem.h"

namespace Luth
{
    VKTexture::VKTexture(const fs::path& path)
        : m_Path(FileSystem::GetPath(ResourceType::Texture, path))
    {
        /*int width, height, channels;
        stbi_uc* pixels = stbi_load(m_Path.string().c_str(),
            &width, &height, &channels, STBI_rgb_alpha);

        if (pixels) {
            m_Width = width;
            m_Height = height;
            VkDeviceSize imageSize = m_Width * m_Height * 4;

            VKBuffer stagingBuffer(
                m_Device,
                imageSize,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            );

            stagingBuffer.Map();
            stagingBuffer.WriteToBuffer(pixels);
            stagingBuffer.Unmap();

            CreateImage();
            TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            CopyBufferToImage(stagingBuffer);
            TransitionImageLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            CreateImageView();
            CreateTextureSampler();

            stbi_image_free(pixels);
        }*/
    }

    VKTexture::VKTexture(uint32_t width, uint32_t height, TextureFormat format) {}

    VKTexture::~VKTexture() {}

    void VKTexture::CreateImage()
    {
        //VkImageCreateInfo imageInfo{};
        //imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        //imageInfo.imageType = VK_IMAGE_TYPE_2D;
        //imageInfo.extent = { m_Width, m_Height, 1 };
        //imageInfo.mipLevels = 1;
        //imageInfo.arrayLayers = 1;
        //imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        //imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        //imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        //imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        //    VK_IMAGE_USAGE_SAMPLED_BIT;
        //imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        //imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        //VK_CHECK_RESULT(vkCreateImage(m_Device, &imageInfo, nullptr, &m_Image),
        //    "Failed to Create Image!");

        //// Allocate memory and bind image
        //VkMemoryRequirements memRequirements;
        //vkGetImageMemoryRequirements(m_Device, m_Image, &memRequirements);

        //VkMemoryAllocateInfo allocInfo{};
        //allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        //allocInfo.allocationSize = memRequirements.size;
        //allocInfo.memoryTypeIndex = VKUtils::FindMemoryType(
        //    memRequirements.memoryTypeBits,
        //    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        //);

        //VK_CHECK_RESULT(vkAllocateMemory(m_Device, &allocInfo, nullptr, &m_ImageMemory),
        //    "Failed to Allocate Memory!");
        //VK_CHECK_RESULT(vkBindImageMemory(m_Device, m_Image, m_ImageMemory, 0),
        //    "Failed to Bind Image Memory!");
    }

    void VKTexture::Bind(uint32_t slot) const {}
}
