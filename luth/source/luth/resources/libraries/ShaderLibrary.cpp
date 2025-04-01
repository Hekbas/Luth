#include "luthpch.h"
#include "luth/resources/libraries/ShaderLibrary.h"

namespace Luth
{
    namespace fs = std::filesystem;

    //std::shared_mutex ShaderLibrary::s_Mutex;
    //std::unordered_map<std::string, ShaderLibrary::ShaderRecord> ShaderLibrary::s_Shaders;

    void ShaderLibrary::Init()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Initialized Shader Library");
    }

    void ShaderLibrary::Shutdown()
    {
        std::unique_lock lock(s_Mutex);
        s_Shaders.clear();
        LH_CORE_INFO("Cleared Shader Library");
    }

    bool ShaderLibrary::Add(const std::string& name, std::shared_ptr<Shader> shader)
    {
        std::unique_lock lock(s_Mutex);
        if (shader == nullptr)
        {
            LH_CORE_ERROR("Attempted to add null shader '{0}'", name);
            return false;
        }

        auto [it, inserted] = s_Shaders.try_emplace(name, ShaderRecord{ shader, "", {} });
        if (!inserted)
        {
            LH_CORE_WARN("Shader '{0}' already exists! Overwriting...", name);
            it->second = { shader, "", {} };
        }
        return true;
    }

    bool ShaderLibrary::Remove(const std::string& name)
    {
        std::unique_lock lock(s_Mutex);
        return s_Shaders.erase(name) > 0;
    }

    std::shared_ptr<Shader> ShaderLibrary::Get(const std::string& name)
    {
        std::shared_lock lock(s_Mutex);
        auto it = s_Shaders.find(name);
        if (it == s_Shaders.end())
        {
            LH_CORE_ERROR("Shader '{0}' not found in library", name);
            return nullptr;
        }
        return it->second.shader;
    }

    bool ShaderLibrary::Contains(const std::string& name)
    {
        std::shared_lock lock(s_Mutex);
        return s_Shaders.find(name) != s_Shaders.end();
    }

    std::vector<std::string> ShaderLibrary::GetShaderNames()
    {
        std::shared_lock lock(s_Mutex);
        std::vector<std::string> names;
        names.reserve(s_Shaders.size());
        for (const auto& [name, _] : s_Shaders)
            names.push_back(name);
        return names;
    }

    std::shared_ptr<Shader> ShaderLibrary::Load(const std::string& filePath)
    {
        const fs::path path(filePath);
        if (!fs::exists(path))
        {
            LH_CORE_ERROR("Shader file not found: {0}", filePath);
            return nullptr;
        }

        auto shader = Shader::Create(filePath);
        if (!shader)
        {
            LH_CORE_ERROR("Failed to create shader from {0}", filePath);
            return nullptr;
        }

        const std::string name = path.stem().string();
        const auto modTime = fs::last_write_time(path);

        std::unique_lock lock(s_Mutex);
        s_Shaders[name] = { shader, path, modTime };
        LH_CORE_INFO("Loaded shader '{0}' from {1}", name, filePath);
        return shader;
    }

    std::shared_ptr<Shader> ShaderLibrary::Load(const std::string& name, const std::string& filePath)
    {
        const fs::path path(filePath);
        if (!fs::exists(path))
        {
            LH_CORE_ERROR("Shader file not found: {0}", filePath);
            return nullptr;
        }

        auto shader = Shader::Create(filePath);
        if (!shader)
        {
            LH_CORE_ERROR("Failed to create shader '{0}' from {1}", name, filePath);
            return nullptr;
        }

        const auto modTime = fs::last_write_time(path);

        std::unique_lock lock(s_Mutex);
        s_Shaders[name] = { shader, path, modTime };
        LH_CORE_INFO("Loaded shader '{0}' from {1}", name, filePath);
        return shader;
    }

    std::shared_ptr<Shader> ShaderLibrary::LoadOrGet(const std::string& filePath)
    {
        const fs::path path(filePath);
        const std::string name = path.stem().string();

        std::shared_lock lock(s_Mutex);
        if (auto it = s_Shaders.find(name); it != s_Shaders.end())
            return it->second.shader;

        lock.unlock();
        return Load(filePath);
    }

    std::shared_ptr<Shader> ShaderLibrary::LoadOrGet(const std::string& name, const std::string& filePath)
    {
        std::shared_lock lock(s_Mutex);
        if (auto it = s_Shaders.find(name); it != s_Shaders.end())
            return it->second.shader;

        lock.unlock();
        return Load(name, filePath);
    }

    bool ShaderLibrary::Reload(const std::string& name)
    {
        std::unique_lock lock(s_Mutex);
        auto it = s_Shaders.find(name);
        if (it == s_Shaders.end())
        {
            LH_CORE_WARN("Cannot reload non-existent shader '{0}'", name);
            return false;
        }

        auto& record = it->second;
        if (record.sourcePath.empty())
        {
            LH_CORE_INFO("Shader '{0}' not reloadable (no source path)", name);
            return false;
        }

        if (!fs::exists(record.sourcePath))
        {
            LH_CORE_ERROR("Shader source file missing: {0}", record.sourcePath.string());
            return false;
        }

        const auto newTime = fs::last_write_time(record.sourcePath);
        if (newTime <= record.lastModified)
            return true; // Already up-to-date

        if (auto newShader = Shader::Create(record.sourcePath.string()))
        {
            record.shader = newShader;
            record.lastModified = newTime;
            LH_CORE_INFO("Successfully reloaded shader '{0}'", name);
            return true;
        }

        LH_CORE_ERROR("Failed to reload shader '{0}' (compilation failed)", name);
        return false;
    }

    void ShaderLibrary::ReloadAll()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Reloading all shaders...");

        size_t successCount = 0;
        size_t failCount = 0;

        for (auto& [name, record] : s_Shaders)
        {
            if (record.sourcePath.empty()) continue;

            if (!fs::exists(record.sourcePath))
            {
                LH_CORE_ERROR("Shader source missing: {0}", record.sourcePath.string());
                failCount++;
                continue;
            }

            const auto newTime = fs::last_write_time(record.sourcePath);
            if (newTime <= record.lastModified)
                continue;

            if (auto newShader = Shader::Create(record.sourcePath.string()))
            {
                record.shader = newShader;
                record.lastModified = newTime;
                successCount++;
            }
            else
            {
                failCount++;
            }
        }

        LH_CORE_INFO("Reloaded shaders: {0} succeeded, {1} failed", successCount, failCount);
    }
}
