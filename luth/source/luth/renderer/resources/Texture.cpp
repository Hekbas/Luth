#include "luthpch.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"

namespace Luth
{
    std::shared_ptr<Texture> Texture::Create(const fs::path& path)
    {
        switch (Renderer::GetBackend()->GetAPI()) {
            case RenderBackend::API::Vulkan: return std::make_shared<VKTexture>(path);
            default:
                LH_CORE_ASSERT(false, "Unknown renderer API!");
                return nullptr;
        }
    }

    std::shared_ptr<Texture> Texture::Create(u32 width, u32 height, TextureFormat format, const void* data)
    {
        if (width == 0 || height == 0) {
            LH_CORE_ERROR("Texture creation failed: Invalid dimensions {0}x{1}", width, height);
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
            LH_CORE_ERROR("Texture creation failed: Invalid dimensions {0}x{1}", width, height);
            return nullptr;
        }

        switch (Renderer::GetBackend()->GetAPI()) {
            case RenderBackend::API::Vulkan: return std::make_shared<VKTexture>(width, height, format, data, settings);
            default:
                LH_CORE_ASSERT(false, "Unknown renderer API!");
                return nullptr;
        }
    }
}
