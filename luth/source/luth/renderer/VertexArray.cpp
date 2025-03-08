#include "luthpch.h"
#include "luth/renderer/VertexArray.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/OpenGL/GLVertexArray.h"
#include "luth/renderer/Vulkan/VKRendererAPI.h"
#include "luth/renderer/Vulkan/VKVertexArray.h"

namespace Luth
{
    std::unique_ptr<VertexArray> VertexArray::Create()
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::OpenGL:
                return std::make_unique<GLVertexArray>();

            case RendererAPI::API::Vulkan:
            {
                auto vkRenderer = static_cast<VKRendererAPI*>(Renderer::GetRendererAPI());
                return std::make_unique<VKVertexArray>(vkRenderer->GetLogicalDevice());
            }

            default:
                LH_CORE_ASSERT(false, "Unknown RendererAPI!");
                return nullptr;
        }
    }
}
