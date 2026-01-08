#include "luthpch.h"
#include "luth/renderer/RendererAPI.h"
#include "luth/renderer/Renderer.h"

namespace Luth
{
    RendererAPI::API RendererAPI::s_API = API::None;

    std::unique_ptr<RendererAPI> RendererAPI::Create(API api)
    {
        LH_CORE_INFO("Initializing {0} renderer...", APIToString(api));
        s_API = api;

        switch (api)
        {
            case RendererAPI::API::None:
                LH_CORE_ASSERT(false, "{0} is not supported!", APIToString(api));
                return nullptr;

            case RendererAPI::API::Vulkan:
                return nullptr; // Will be implemented in Phase 3

            default:
                LH_CORE_ASSERT(false, "{1} Unknown RendererAPI!", APIToString(api));
                return nullptr;
        }
    }

    const char* RendererAPI::APIToString(API api)
    {
        switch (api)
        {
            case API::None:    return "None";
            case API::Vulkan:  return "Vulkan";
            default: return "Unknown";
        }
    }

    void RendererAPI::SetWindow(void* window) {
        s_Window = window;
    }
}
