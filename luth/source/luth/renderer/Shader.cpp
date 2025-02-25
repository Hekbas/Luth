#include "luthpch.h"
#include "luth/renderer/Shader.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/openGL/GLShader.h"

namespace Luth
{
    std::shared_ptr<Shader> Shader::Create(const std::string& filePath)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::OpenGL:
                return std::make_shared<GLShader>(filePath);
            default:
                LH_CORE_ASSERT(false, "Unknown RendererAPI!");
                return nullptr;
        }
    }

    std::shared_ptr<Shader> Shader::Create(const std::string& vertexSrc, const std::string& fragmentSrc)
    {
        switch (Renderer::GetAPI())
	{
            case RendererAPI::OpenGL:
                return std::make_shared<GLShader>(vertexSrc, fragmentSrc);
            default:
                LH_CORE_ASSERT(false, "Unknown RendererAPI!");
                return nullptr;
	}
    }

    std::string Shader::Load(const std::string& filePath)
    {
        std::ifstream in(filePath, std::ios::in | std::ios::binary);
        if (!in)
	{
            LH_CORE_ERROR("Could not open shader file: {0}", filePath);
            return "";
        }

        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
	}
}
