#include "luthpch.h"
#include "luth/renderer/RendererAPI.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VKRendererAPI.h"

namespace Luth
{
    RendererAPI::API RendererAPI::s_API = API::Vulkan;

    std::unique_ptr<RendererAPI> RendererAPI::Create()
    {
        switch (s_API)
        {
            case RendererAPI::API::None:
                LH_CORE_ASSERT(false, "RendererAPI::None is not supported!");
                return nullptr;

            case RendererAPI::API::Vulkan:
                return std::make_unique<VKRendererAPI>();

            default:
                LH_CORE_ASSERT(false, "Unknown RendererAPI!");
                return nullptr;
        }
    }
}
