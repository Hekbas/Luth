#include "luthpch.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/core/Log.h"

namespace Luth
{
    std::unordered_map<std::string, std::shared_ptr<Shader>> ShaderLibrary::s_Shaders;
    std::function<void(const std::string&)> ShaderLibrary::s_ReloadCallback;

    void ShaderLibrary::Init()
    {
        s_Shaders.clear();
        s_ReloadCallback = nullptr;
    }

    void ShaderLibrary::Shutdown()
    {
        s_ReloadCallback = nullptr;
        s_Shaders.clear();
    }

    void ShaderLibrary::Register(const std::string& name, std::shared_ptr<Shader> shader)
    {
        if (s_Shaders.count(name))
            LH_CORE_WARN("ShaderLibrary: overwriting existing shader '{}'", name);

        s_Shaders[name] = shader;
        LH_CORE_INFO("ShaderLibrary: registered '{}'", name);
    }

    std::shared_ptr<Shader> ShaderLibrary::Get(const std::string& name)
    {
        auto it = s_Shaders.find(name);
        if (it == s_Shaders.end())
        {
            LH_CORE_ERROR("ShaderLibrary: shader '{}' not found", name);
            return nullptr;
        }
        return it->second;
    }

    const std::unordered_map<std::string, std::shared_ptr<Shader>>& ShaderLibrary::GetAll()
    {
        return s_Shaders;
    }

    bool ShaderLibrary::Reload(const std::string& name)
    {
        auto it = s_Shaders.find(name);
        if (it == s_Shaders.end())
        {
            LH_CORE_ERROR("ShaderLibrary::Reload: shader '{}' not found", name);
            return false;
        }

        LH_CORE_INFO("ShaderLibrary: reloading '{}'...", name);
        it->second->Reload();

        if (!it->second->IsValid())
        {
            LH_CORE_ERROR("ShaderLibrary: reload of '{}' failed — keeping old pipeline", name);
            return false;
        }

        if (s_ReloadCallback)
            s_ReloadCallback(name);

        return true;
    }

    void ShaderLibrary::SetReloadCallback(std::function<void(const std::string&)> cb)
    {
        s_ReloadCallback = std::move(cb);
    }
}
