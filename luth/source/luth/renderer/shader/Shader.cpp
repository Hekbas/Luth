#include "luthpch.h"
#include "luth/renderer/shader/Shader.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanShader.h"

namespace Luth
{
    std::shared_ptr<Shader> Shader::Create(ShaderStage stage, const std::vector<u32>& spirv, const fs::path& path)
    {
        switch (Renderer::GetBackend()->GetAPI())
        {
        case RenderBackend::API::Vulkan:
            return std::make_shared<VulkanShader>(stage, spirv, path);
        default:
            LH_CORE_ASSERT(false, "Unknown RenderBackend!");
            return nullptr;
        }
    }
}
