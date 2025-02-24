#include "luthpch.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/OpenGL/GLRenderer.h"
#include "luth/renderer/vulkan/VKRenderer.h"

namespace Luth
{
    RendererAPI Renderer::s_API = RendererAPI::Vulkan;

    RendererAPI Renderer::GetAPI() { return s_API; }
    void Renderer::SetAPI(RendererAPI API) { s_API = API; }

    std::string Renderer::APIToString()
    {
        switch (s_API)
        {
            case Luth::RendererAPI::None:   return "None";
            case Luth::RendererAPI::OpenGL: return "OpenGL";
            case Luth::RendererAPI::Vulkan: return "Vulkan";
            default: return "Unknown";
        }
    }

    std::unique_ptr<Renderer> Renderer::Create(RendererAPI API)
    {
        switch (API)
        {
            case RendererAPI::OpenGL:
                return std::make_unique<GLRenderer>();

            case RendererAPI::Vulkan:
                return std::make_unique<VKRenderer>();

            case RendererAPI::None:
                LH_CORE_ASSERT(false, "RendererAPI::None is not supported!");
                return nullptr;

            default:
                LH_CORE_ASSERT(false, "Unknown RendererAPI!");
                return nullptr;
        }
    }
}
