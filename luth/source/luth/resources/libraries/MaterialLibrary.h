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

        static std::shared_ptr<Material> CreateNew();
        static std::shared_ptr<Material> LoadOrGet(const fs::path& path);
        static std::shared_ptr<Material> Get(const UUID& uuid);
        static std::unordered_map<UUID, std::shared_ptr<Material>, UUIDHash> GetAllMaterials();

        static bool Save(const UUID& materialUUID);
        static void Reload(const UUID& materialUUID);

    private:
        static std::shared_mutex s_Mutex;
        static std::unordered_map<UUID, std::shared_ptr<Material>, UUIDHash> s_Materials;
    };
}
