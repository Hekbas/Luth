#include "luthpch.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"

namespace Luth
{
    TextureFormatInfo GetTextureFormatInfo(TextureFormat fmt)
    {
        switch (fmt) {
            case TextureFormat::BC1_Unorm: return { true, 4, 8 };
            case TextureFormat::BC4_Unorm: return { true, 4, 8 };
            case TextureFormat::BC5_Unorm: return { true, 4, 16 };
            case TextureFormat::BC7_Unorm: return { true, 4, 16 };
            case TextureFormat::R8:        return { false, 1, 1 };
            case TextureFormat::RGB8:      return { false, 1, 3 };
            case TextureFormat::RGBA8:     return { false, 1, 4 };
            case TextureFormat::RGBA16F:   return { false, 1, 8 };
            case TextureFormat::RGBA32F:   return { false, 1, 16 };
            case TextureFormat::RG16F:     return { false, 1, 4 };
            case TextureFormat::R32_Float: return { false, 1, 4 };
            case TextureFormat::R32_Uint:  return { false, 1, 4 };
            case TextureFormat::R16_Uint:  return { false, 1, 2 };
            default:                       return { false, 1, 4 }; // depth/unknown: not size-queried here
        }
    }

    u64 TextureLevelBytes(TextureFormat fmt, u32 w, u32 h)
    {
        TextureFormatInfo info = GetTextureFormatInfo(fmt);
        if (info.compressed) {
            u64 bx = (w + info.blockDim - 1) / info.blockDim;
            u64 by = (h + info.blockDim - 1) / info.blockDim;
            return bx * by * info.blockBytes;
        }
        return (u64)w * h * info.blockBytes;
    }

    std::shared_ptr<Texture> Texture::Create(u32 width, u32 height, TextureFormat format, const void* data)
    {
        if (width == 0 || height == 0) {
            LH_LOG(Renderer, error, "Texture creation failed: Invalid dimensions {0}x{1}", width, height);
            return nullptr;
        }

        switch (Renderer::GetBackend()->GetAPI()) {
            case RenderBackend::API::Vulkan: return std::make_shared<VKTexture>(width, height, format, data);
            default:
                LH_CORE_ASSERT(false, "Unknown renderer API!");
                return nullptr;
        }
    }

    std::shared_ptr<Texture> Texture::Create(u32 width, u32 height, TextureFormat format, const void* data, const TextureSettings& settings)
    {
        if (width == 0 || height == 0) {
            LH_LOG(Renderer, error, "Texture creation failed: Invalid dimensions {0}x{1}", width, height);
            return nullptr;
        }

        switch (Renderer::GetBackend()->GetAPI()) {
            case RenderBackend::API::Vulkan: return std::make_shared<VKTexture>(width, height, format, data, settings);
            default:
                LH_CORE_ASSERT(false, "Unknown renderer API!");
                return nullptr;
        }
    }

    std::shared_ptr<Texture> Texture::Create(u32 width, u32 height, TextureFormat format,
        const void* data, u64 sizeBytes, u32 mipLevels, const TextureSettings& settings)
    {
        if (width == 0 || height == 0) {
            LH_LOG(Renderer, error, "Texture creation failed: Invalid dimensions {0}x{1}", width, height);
            return nullptr;
        }

        switch (Renderer::GetBackend()->GetAPI()) {
            case RenderBackend::API::Vulkan:
                return std::make_shared<VKTexture>(width, height, format, data, sizeBytes, mipLevels, settings);
            default:
                LH_CORE_ASSERT(false, "Unknown renderer API!");
                return nullptr;
        }
    }
}
