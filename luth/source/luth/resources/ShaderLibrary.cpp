#include "luthpch.h"
#include "luth/resources/ShaderLibrary.h"

namespace Luth
{
    void ShaderLibrary::Init()
    {
        LH_CORE_INFO("Initialized Shader Library");
    }

    void ShaderLibrary::Shutdown()
    {
        s_Shaders.clear();
        LH_CORE_INFO("Cleared Shader Library");
    }

    void ShaderLibrary::Add(const std::string& name, const std::shared_ptr<Shader>& shader)
    {
        if (Exists(name))
        {
            LH_CORE_WARN("Shader '{0}' already exists! Overwriting...", name);
        }
        s_Shaders[name] = shader;
    }

    std::shared_ptr<Shader> ShaderLibrary::Load(const std::string& filePath)
    {
        auto shader = Shader::Create(filePath);
        Add(filePath, shader); // Use file path as default name
        return shader;
    }

    std::shared_ptr<Shader> ShaderLibrary::Load(const std::string& name, const std::string& filePath)
    {
        auto shader = Shader::Create(filePath);
        Add(name, shader);
        return shader;
    }

    std::shared_ptr<Shader> ShaderLibrary::Get(const std::string& name)
    {
        LH_CORE_ASSERT(Exists(name), "Shader '{0}' not found!", name);
        return s_Shaders.at(name);
    }

    bool ShaderLibrary::Exists(const std::string& name)
    {
        return s_Shaders.find(name) != s_Shaders.end();
    }

    void ShaderLibrary::ReloadAll()
    {
        for (auto& [name, shader] : s_Shaders)
        {
            // TODO: Implement shader reloading
            LH_CORE_INFO("Reloaded shader: {0}", name);
        }
    }
}
