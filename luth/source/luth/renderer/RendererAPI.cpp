#include "luthpch.h"
#include "luth/renderer/RendererAPI.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/OpenGL/GLRendererAPI.h"
#include "luth/renderer/vulkan/VKRendererAPI.h"

namespace Luth
{
    RendererAPI::API RendererAPI::s_API = API::None;

    std::unique_ptr<RendererAPI> RendererAPI::Create(API api)
    {
        switch (api)
        {
            case RendererAPI::API::None:
                LH_CORE_ASSERT(false, "{0} is not supported!", APIToString(api));
                return nullptr;

            case RendererAPI::API::OpenGL:
                return std::make_unique<GLRendererAPI>();

            case RendererAPI::API::Vulkan:
                return std::make_unique<VKRendererAPI>();

            default:
                LH_CORE_ASSERT(false, "{1} Unknown RendererAPI!", APIToString(api));
                return nullptr;
        }

        s_API = api;

        LH_CORE_INFO("Initialized {0} renderer", APIToString(api));
    }

    const char* RendererAPI::APIToString(API api)
    {
        switch (api)
        {
            case API::None:    return "None";
            case API::OpenGL:  return "OpenGL";
            case API::Vulkan:  return "Vulkan";
            default: return "Unknown";
        }
    }

    void RendererAPI::SetWindow(void* window) {
        s_Window = window;
    }
}
