#include "luthpch.h"
#include "luth/renderer/Shader.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanShader.h"

namespace Luth
{
    std::shared_ptr<Shader> Shader::Create(const fs::path& filePath)
    {
        switch (Renderer::GetBackend()->GetAPI())
        {
        case RenderBackend::API::Vulkan:
            return std::make_shared<VulkanShader>(filePath);
        default:
            LH_CORE_ASSERT(false, "Unknown RenderBackend!");
            return nullptr;
        }
    }
}
