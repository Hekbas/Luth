#include "luthpch.h"
#include "luth/renderer/VertexBuffer.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/vulkan/VKRendererAPI.h"
#include "luth/renderer/openGL/GLVertexBuffer.h"
#include "luth/renderer/vulkan/VKVertexBuffer.h"

#include <memory>

namespace Luth
{
    std::unique_ptr<VertexBuffer> VertexBuffer::Create(uint32_t size)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
                LH_CORE_ASSERT(false, "RendererAPI::None is not supported!");
                return nullptr;

            case RendererAPI::API::OpenGL:
                return std::make_unique<GLVertexBuffer>(size);

            case RendererAPI::API::Vulkan:
            {
                auto vkRenderer = static_cast<VKRendererAPI*>(Renderer::GetRendererAPI());
                return std::make_unique<VKVertexBuffer>(
                    vkRenderer->GetLogicalDevice(),
                    vkRenderer->GetPhysicalDevice(),
                    size
                );
            }
        }

        LH_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    std::unique_ptr<VertexBuffer> VertexBuffer::Create(const void* data, uint32_t size)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
                LH_CORE_ASSERT(false, "RendererAPI::None is not supported!");
                return nullptr;

            case RendererAPI::API::OpenGL:
                return std::make_unique<GLVertexBuffer>(data, size);

            case RendererAPI::API::Vulkan:
            {
                auto vkRenderer = static_cast<VKRendererAPI*>(Renderer::GetRendererAPI());
                return std::make_unique<VKVertexBuffer>(
                    vkRenderer->GetLogicalDevice(),
                    vkRenderer->GetPhysicalDevice(),
                    data,
                    size
                );
            }
        }

        LH_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
}
