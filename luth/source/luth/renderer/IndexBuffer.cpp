#include "luthpch.h"
#include "luth/renderer/IndexBuffer.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/OpenGL/GLVertexArray.h"
#include "luth/renderer/OpenGL/GLIndexBuffer.h"
#include "luth/renderer/Vulkan/VKRendererAPI.h"
#include "luth/renderer/Vulkan/VKVertexArray.h"
#include "luth/renderer/Vulkan/VKIndexBuffer.h"

namespace Luth
{
    std::unique_ptr<IndexBuffer> IndexBuffer::Create(const uint32_t* indices, uint32_t count)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::OpenGL:
                return std::make_unique<GLIndexBuffer>(indices, count);

            case RendererAPI::API::Vulkan:
            {
                auto vkRenderer = static_cast<VKRendererAPI*>(Renderer::GetRendererAPI());
                return std::make_unique<VKIndexBuffer>(
                    vkRenderer->GetLogicalDevice(),
                    vkRenderer->GetPhysicalDevice(),
                    indices,
                    count
                );
            }

            default:
                LH_CORE_ASSERT(false, "Unknown RendererAPI!");
                return nullptr;
        }
    }
}
