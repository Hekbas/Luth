#pragma once

#include "luth/renderer/Texture.h"
#include "luth/renderer/vulkan/VKDevice.h"
#include "luth/renderer/vulkan/VKBuffer.h"

namespace Luth
{
    class VKTexture : public Texture
    {
    public:
        VKTexture(const fs::path& path);
        VKTexture(u32 width, u32 height, TextureFormat format, const void* data);
        ~VKTexture();

        void Bind(u32 slot = 0) const override;
        //void SetData(void* data, u32 size) override;

        u32 GetWidth() const override { return m_Width; }
        u32 GetHeight() const override { return m_Height; }
        u32 GetRendererID() const override { return 0; } // Vulkan doesn't use IDs
        const fs::path& GetPath() const override { return m_Path; }

        TextureFormat GetFormat() const override { return m_Format; }
        std::string GetFormatString() const override {
            switch (m_Format) {
            case TextureFormat::R8:      return "R8";
            case TextureFormat::RGB8:    return "RGB8";
            case TextureFormat::RGBA8:   return "RGBA8";
            case TextureFormat::RGBA32F: return "RGBA32F";
            default: return "Unknown";
            }
        }

        TextureWrapMode GetWrapMode() const override { return m_WrapMode; }
        void SetWrapMode(TextureWrapMode mode) override;

        std::pair<TextureFilterMode, TextureFilterMode> GetFilterMode() const override {
            return { m_MinFilter, m_MagFilter };
        }
        void SetFilterMode(TextureFilterMode min, TextureFilterMode mag) override;

        int GetMipLevels() const override { return m_MipLevels; }
        void GenerateMipmaps() override;

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

        u32 m_Width = 0, m_Height = 0;
        fs::path m_Path;
        int m_MipLevels = 0;
        TextureFormat m_Format = TextureFormat::RGBA8;
        TextureWrapMode m_WrapMode = TextureWrapMode::Repeat;
        TextureFilterMode m_MinFilter = TextureFilterMode::Linear;
        TextureFilterMode m_MagFilter = TextureFilterMode::Linear;
    };
}
