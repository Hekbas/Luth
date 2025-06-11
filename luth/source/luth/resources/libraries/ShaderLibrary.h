#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/UUID.h"
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
        struct ShaderRecord {
            std::shared_ptr<Shader> Shader;
            std::string Name;
            fs::path SourcePath;
            fs::file_time_type LastModified;
        };

        static void Init();
        static void Shutdown();

        // Basic operations
        static bool Add(std::shared_ptr<Shader> shader);
        static bool Remove(const UUID& uuid);
        static bool Contains(const UUID& uuid);

        static std::shared_ptr<Shader> Get(const UUID& uuid);
        static std::shared_ptr<Shader> Get(const std::string& name);
        static std::vector<UUID> GetAllUuids();
        static std::unordered_map<UUID, ShaderRecord, UUIDHash> GetAllShaders();

        // Smart loading
        static std::shared_ptr<Shader> Load(const fs::path& filePath);
        static std::shared_ptr<Shader> LoadOrGet(const fs::path& filePath);

        // Reload functionality
        static bool Reload(const UUID& uuid);
        static void ReloadAll();

    private:
        static std::shared_mutex s_Mutex;
        static std::unordered_map<UUID, ShaderRecord, UUIDHash> s_Shaders;
        static std::unordered_map<std::string, UUID> s_NameToUuidMap;
    };
}
