#pragma once

#include "luth/renderer/Texture.h"
#include "VulkanAllocator.h"

namespace Luth
{
    class VKTexture : public Texture
    {
    public:
        VKTexture(const fs::path& path);
        VKTexture(u32 width, u32 height, TextureFormat format, const void* data);
        virtual ~VKTexture();

        virtual void Bind(u32 slot = 0) const override;

        virtual u32 GetWidth() const override { return m_Width; }
        virtual u32 GetHeight() const override { return m_Height; }
        virtual u32 GetRendererID() const override { return 0; } // Not used in Vulkan
        virtual const fs::path& GetPath() const override { return m_Path; }

        virtual TextureFormat GetFormat() const override { return m_Format; }
        virtual std::string GetFormatString() const override;

        virtual TextureWrapMode GetWrapMode() const override { return m_WrapMode; }
        virtual void SetWrapMode(TextureWrapMode mode) override;

        virtual std::pair<TextureFilterMode, TextureFilterMode> GetFilterMode() const override { return { m_MinFilter, m_MagFilter }; }
        virtual void SetFilterMode(TextureFilterMode min, TextureFilterMode mag) override;

        virtual int GetMipLevels() const override { return 1; }
        virtual void GenerateMipmaps() override {}

        VkImage GetImage() const { return m_Image; }
        VkImageView GetImageView() const { return m_ImageView; }
        VkSampler GetSampler() const { return m_Sampler; }
        u32 GetBindlessIndex() const { return m_BindlessIndex; }

    private:
        void CreateImage(const void* data);
        void CreateViewAndSampler();

        fs::path m_Path;
        u32 m_Width, m_Height;
        TextureFormat m_Format;
        TextureWrapMode m_WrapMode = TextureWrapMode::Repeat;
        TextureFilterMode m_MinFilter = TextureFilterMode::Linear, m_MagFilter = TextureFilterMode::Linear;

        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        VkImageView m_ImageView = VK_NULL_HANDLE;
        VkSampler m_Sampler = VK_NULL_HANDLE;
        u32 m_BindlessIndex = 0;
    };
}