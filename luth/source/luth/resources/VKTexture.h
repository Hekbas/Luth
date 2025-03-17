#pragma once

#include "luth/resources/Texture.h"
#include "luth/renderer/vulkan/VKDevice.h"
#include "luth/renderer/vulkan/VKBuffer.h"

namespace Luth
{
    class VKTexture : public Texture
    {
    public:
        VKTexture(const fs::path& path);
        VKTexture(uint32_t width, uint32_t height, TextureFormat format);
        ~VKTexture();

        void Bind(uint32_t slot = 0) const override;
        //void SetData(void* data, uint32_t size) override;

        uint32_t GetWidth() const override { return m_Width; }
        uint32_t GetHeight() const override { return m_Height; }
        uint32_t GetRendererID() const override { return 0; } // Vulkan doesn't use IDs
        const fs::path& GetPath() const override { return m_Path; }

        VkImageView GetImageView() const { return m_ImageView; }
        VkSampler GetSampler() const { return m_Sampler; }

    private:
        void CreateImage();
        //void TransitionImageLayout(VkImageLayout oldLayout, VkImageLayout newLayout);
        //void CopyBufferToImage(VKBuffer& buffer);
        //void CreateImageView();
        //void CreateTextureSampler();

        VkDevice m_Device;
        VkImage m_Image = VK_NULL_HANDLE;
        VkDeviceMemory m_ImageMemory = VK_NULL_HANDLE;
        VkImageView m_ImageView = VK_NULL_HANDLE;
        VkSampler m_Sampler = VK_NULL_HANDLE;

        uint32_t m_Width = 0, m_Height = 0;
        fs::path m_Path;
    };
}
