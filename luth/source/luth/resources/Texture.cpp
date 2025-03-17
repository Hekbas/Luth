#include "Luthpch.h"
#include "luth/resources/Texture.h"
#include "luth/resources/GLTexture.h"
#include "luth/resources/VKTexture.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RendererAPI.h"

namespace Luth
{
    std::shared_ptr<Texture> Texture::Create(const fs::path& path)
    {
        // Validate path through ResourceManager
        const fs::path fullPath = ResourceManager::GetPath(Resource::Texture, path);

        if (!ResourceManager::ValidateResourcePath(fullPath)) {
            LH_CORE_ERROR("Texture creation failed: Invalid path {}", fullPath);
            return nullptr;
        }

        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::OpenGL:
                return std::make_shared<GLTexture>(fullPath);
            case RendererAPI::API::Vulkan:
                return std::make_shared<VKTexture>(fullPath);
            default:
                LH_CORE_ASSERT(false, "Unknown renderer API!");
                return nullptr;
        }
    }

    std::shared_ptr<Texture> Texture::Create(uint32_t width, uint32_t height, TextureFormat format)
    {
        if (width == 0 || height == 0) {
            LH_CORE_ERROR("Texture creation failed: Invalid dimensions {}x{}", width, height);
            return nullptr;
        }

        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::OpenGL:
                return std::make_shared<GLTexture>(width, height, format);
            case RendererAPI::API::Vulkan:
                return std::make_shared<VKTexture>(width, height, format);
            default:
                LH_CORE_ASSERT(false, "Unknown renderer API!");
                return nullptr;
        }
    }
}
