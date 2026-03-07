#include "luthpch.h"
#include "RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"

namespace Luth
{
    RenderBackend::API RenderBackend::s_API = RenderBackend::API::Vulkan;

    std::unique_ptr<RenderBackend> RenderBackend::Create()
    {
        switch (s_API)
        {
            case RenderBackend::API::None:    return nullptr;
            case RenderBackend::API::Vulkan:  return std::make_unique<VulkanBackend>();
        }
        return nullptr;
    }
}
