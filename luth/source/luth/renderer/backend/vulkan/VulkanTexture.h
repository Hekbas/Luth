#pragma once

#include "luth/renderer/resources/Texture.h"
#include "VulkanAllocator.h"
#include "VulkanDescriptors.h" // for BindlessDescriptorSet::INVALID_BINDLESS_SLOT

namespace Luth
{
    class VKTexture : public Texture
    {
    public:
        VKTexture(u32 width, u32 height, TextureFormat format, const void* data);
        VKTexture(u32 width, u32 height, TextureFormat format, const void* data, const TextureSettings& settings);
        // Cubemap / storage image constructor (no data upload — filled via compute or blit)
        VKTexture(u32 width, u32 height, TextureFormat format, u32 arrayLayers,
                  VkImageCreateFlags createFlags, u32 mipLevels, VkImageUsageFlags extraUsage = 0);
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

        virtual int GetMipLevels() const override { return m_MipLevels; }
        virtual void GenerateMipmaps() override {}

        VkImage GetImage() const { return m_Image; }
        VkImageView GetImageView() const { return m_ImageView; }
        VkSampler GetSampler() const { return m_Sampler; }
        u32 GetArrayLayers() const { return m_ArrayLayers; }

        // Create a view for a single mip level (all array layers). Caller must destroy the returned view.
        // forStorage: use VK_IMAGE_VIEW_TYPE_2D_ARRAY instead of CUBE for compute storage image bindings.
        VkImageView CreateMipView(u32 mipLevel, bool forStorage = false) const;

        // Create a 2D view for a single array layer (all mips). Caller must destroy the returned view.
        VkImageView CreateLayerView(u32 layer) const;

        // Bindless Support
        virtual u32 GetBindlessIndex() const override { return m_BindlessIndex; }

    private:
        void CreateImage(const void* data);
        void CreateViewAndSampler();
        void RegisterBindless();
        void UnregisterBindless();

        fs::path m_Path;
        u32 m_Width, m_Height;
        TextureFormat m_Format;
        TextureWrapMode m_WrapMode = TextureWrapMode::Repeat;
        TextureFilterMode m_MinFilter = TextureFilterMode::Linear, m_MagFilter = TextureFilterMode::Linear;

        u32 m_MipLevels = 1;
        u32 m_ArrayLayers = 1;
        VkImageCreateFlags m_CreateFlags = 0;
        VkImageUsageFlags m_ExtraUsage = 0;

        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        VkImageView m_ImageView = VK_NULL_HANDLE;
        VkSampler m_Sampler = VK_NULL_HANDLE;
        u32 m_BindlessIndex = BindlessDescriptorSet::INVALID_BINDLESS_SLOT;

        // Set when CreateImage routed pixel data through UploadContext (color non-depth path);
        // CreateViewAndSampler then defers bindless registration via the pending-bind pump.
        u64 m_LastUploadFence = 0;
        bool m_DidAsyncUpload = false;
    };
}
