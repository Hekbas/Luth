#include "luthpch.h"
#include "luth/renderer/Shader.h"
#include "luth/renderer/Renderer.h"

namespace Luth
{
    // Simple concrete class for now, just to hold the path
    class VulkanShaderResource : public Shader
    {
    public:
        VulkanShaderResource(const fs::path& path) : m_Path(path) {}
        virtual const fs::path& GetPath() const override { return m_Path; }
    private:
        fs::path m_Path;
    };

    std::shared_ptr<Shader> Shader::Create(const fs::path& filePath)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::Vulkan:
                return std::make_shared<VulkanShaderResource>(filePath);
            default:
                LH_CORE_ASSERT(false, "Unknown RendererAPI!");
                return nullptr;
        }
    }

    std::string Shader::Load(const fs::path& filePath)
    {
        std::ifstream in(filePath, std::ios::in | std::ios::binary);
        if (!in) {
            LH_CORE_ERROR("Could not open shader file: {0}", filePath.string());
            return "";
        }

        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
}
