#include "luthpch.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/OpenGL/GLRenderer.h"

namespace Luth
{
    RendererAPI Renderer::s_API = RendererAPI::OpenGL;

    RendererAPI Renderer::GetAPI()
    {
        return s_API;
    }

    std::unique_ptr<Renderer> Renderer::Create()
    {
        switch (s_API)
        {
            case RendererAPI::OpenGL:
                return std::make_unique<GLRenderer>();
            case RendererAPI::None:
                LH_CORE_ASSERT(false, "RendererAPI::None is not supported!");
                return nullptr;
            default:
                LH_CORE_ASSERT(false, "Unknown RendererAPI!");
                return nullptr;
        }
    }
}
