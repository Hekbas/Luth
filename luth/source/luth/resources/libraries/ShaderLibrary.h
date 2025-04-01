#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/Shader.h"

#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace Luth
{
    class ShaderLibrary
    {
    public:
        static void Init();
        static void Shutdown();

        // Basic operations
        static bool Add(const std::string& name, std::shared_ptr<Shader> shader);
        static bool Remove(const std::string& name);
        static std::shared_ptr<Shader> Get(const std::string& name);
        static bool Contains(const std::string& name);
        static std::vector<std::string> GetShaderNames();

        // Smart loading
        static std::shared_ptr<Shader> Load(const std::string& filePath);
        static std::shared_ptr<Shader> Load(const std::string& name, const std::string& filePath);
        static std::shared_ptr<Shader> LoadOrGet(const std::string& filePath);
        static std::shared_ptr<Shader> LoadOrGet(const std::string& name, const std::string& filePath);

        // Reload functionality
        static bool Reload(const std::string& name);
        static void ReloadAll();

    private:
        struct ShaderRecord
        {
            std::shared_ptr<Shader> shader;
            std::filesystem::path sourcePath;
            std::chrono::file_clock::time_point lastModified;
        };

        static inline std::shared_mutex s_Mutex;
        static inline std::unordered_map<std::string, ShaderRecord> s_Shaders;
    };
}
