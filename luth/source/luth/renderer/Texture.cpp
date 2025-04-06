#include "Luthpch.h"
#include "luth/renderer/Texture.h"
#include "luth/renderer/openGL/GLTexture.h"
#include "luth/renderer/vulkan/VKTexture.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RendererAPI.h"

namespace Luth
{
    std::shared_ptr<Texture> Texture::Create(const fs::path& path)
    {
        switch (Renderer::GetAPI()) {
            case RendererAPI::API::OpenGL: return std::make_shared<GLTexture>(path);
            case RendererAPI::API::Vulkan: return std::make_shared<VKTexture>(path);
            default:
                LH_CORE_ASSERT(false, "Unknown renderer API!");
                return nullptr;
        }
    }

    std::shared_ptr<Texture> Texture::Create(u32 width, u32 height, u32 format, const unsigned char* data, const std::string& name)
    {
        if (width == 0 || height == 0) {
            LH_CORE_ERROR("Texture creation failed: Invalid dimensions {0}x{1}", width, height);
            return nullptr;
        }

        switch (Renderer::GetAPI()) {
            case RendererAPI::API::OpenGL: return std::make_shared<GLTexture>(width, height, format, data, name);
            case RendererAPI::API::Vulkan: return std::make_shared<VKTexture>(width, height, format, data, name);
            default:
                LH_CORE_ASSERT(false, "Unknown renderer API!");
                return nullptr;
        }
    }
}
