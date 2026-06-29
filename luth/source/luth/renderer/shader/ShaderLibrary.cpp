#include "luthpch.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/FileSystem.h"
#include "luth/core/diagnostics/Log.h"

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
            LH_LOG(Shaders, warn, "ShaderLibrary: overwriting existing shader '{}'", name);

        s_Shaders[name] = shader;
        LH_LOG(Shaders, debug, "ShaderLibrary: registered '{}'", name);
    }

    std::shared_ptr<Shader> ShaderLibrary::Get(const std::string& name)
    {
        auto it = s_Shaders.find(name);
        if (it == s_Shaders.end())
        {
            LH_LOG(Shaders, error, "ShaderLibrary: shader '{}' not found", name);
            return nullptr;
        }
        return it->second;
    }

    const std::unordered_map<std::string, std::shared_ptr<Shader>>& ShaderLibrary::GetAll()
    {
        return s_Shaders;
    }

    std::shared_ptr<Shader> ShaderLibrary::LoadEngine(const std::string& engineRelPath)
    {
        LH_PROFILE_FUNCTION();
        fs::path abs = FileSystem::EngineAssetsPath(engineRelPath);
        std::string key = abs.filename().string();

        // Idempotent: return cached entry
        if (auto it = s_Shaders.find(key); it != s_Shaders.end())
            return it->second;

        UUID uuid = AssetDatabase::GetUUID(abs);
        auto sh = std::static_pointer_cast<Shader>(AssetManager::LoadImmediate(uuid));
        if (!sh)
        {
            LH_LOG(Shaders, error, "ShaderLibrary::LoadEngine: failed to load '{0}'", engineRelPath);
            return nullptr;
        }

        Register(key, sh);
        return sh;
    }

    bool ShaderLibrary::Reload(const std::string& name)
    {
        LH_PROFILE_FUNCTION();
        auto it = s_Shaders.find(name);
        if (it == s_Shaders.end())
        {
            LH_LOG(Shaders, error, "ShaderLibrary::Reload: shader '{}' not found", name);
            return false;
        }

        LH_LOG(Shaders, info, "ShaderLibrary: reloading '{}'...", name);
        it->second->Reload();

        if (!it->second->IsValid())
        {
            LH_LOG(Shaders, error, "ShaderLibrary: reload of '{}' failed -- keeping old pipeline", name);
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
