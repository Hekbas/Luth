#pragma once

#include "luth/renderer/Material.h"

#include <filesystem>
#include <shared_mutex>
#include <unordered_set>

namespace Luth
{
    class MaterialLibrary
    {
    public:
        static void Init();
        static void Shutdown();

        static bool Add(const std::string& name, std::shared_ptr<Material> material);
        static bool Remove(const std::string& name);
        static std::shared_ptr<Material> Get(const std::string& name);
        static bool Contains(const std::string& name);
        static std::vector<std::string> GetMaterialNames();

        static std::shared_ptr<Material> Load(const std::filesystem::path& path);
        static std::shared_ptr<Material> Load(const std::string& name, const std::filesystem::path& path);
        static std::shared_ptr<Material> LoadOrGet(const std::filesystem::path& path);
        static std::shared_ptr<Material> LoadOrGet(const std::string& name, const std::filesystem::path& path);

        static bool Reload(const std::string& name);
        static void ReloadAll();

    private:
        struct MaterialRecord {
            std::shared_ptr<Material> material;
            std::filesystem::path sourcePath;
            std::filesystem::file_time_type lastModified;
            std::unordered_set<std::string> dependencies; // Shaders/textures
        };

        static inline std::shared_mutex s_Mutex;
        static inline std::unordered_map<std::string, MaterialRecord> s_Materials;
    };
}

//Resurces::Load<T>(...)