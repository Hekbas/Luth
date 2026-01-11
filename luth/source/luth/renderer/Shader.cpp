#include "luthpch.h"
#include "luth/renderer/Shader.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanShader.h"

namespace Luth
{
    std::shared_ptr<Shader> Shader::Create(const fs::path& filePath)
    {
        switch (Renderer::GetAPI())
        {
        case RendererAPI::API::Vulkan:
            return std::make_shared<VulkanShader>(filePath);
        default:
            LH_CORE_ASSERT(false, "Unknown RendererAPI!");
            return nullptr;
        }
    }
}
