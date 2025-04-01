#pragma once

#include "luth/core/UUID.h"
#include "luth/renderer/Model.h"

#include <filesystem>
#include <shared_mutex>
#include <unordered_map>

namespace Luth
{
    class ModelLibrary
    {
    public:
        struct ModelRecord {
            std::shared_ptr<Model> Model;
            std::filesystem::file_time_type LastModified;
        };

        static void Init();
        static void Shutdown();

        static bool Add(std::shared_ptr<Model> model);
        static bool Remove(const UUID& uuid);
        static bool Contains(const UUID& uuid);

        static std::shared_ptr<Model> Get(const UUID& uuid);
        static std::vector<std::shared_ptr<Model>> GetAllModels();
        static std::vector<UUID> GetAllUuids();

        static std::shared_ptr<Model> Load(const std::filesystem::path& path);
        static std::shared_ptr<Model> LoadOrGet(const std::filesystem::path& path);

        static bool Reload(const UUID& uuid);
        static void ReloadAll();

    private:
        static std::shared_mutex s_Mutex;
        static std::unordered_map<UUID, ModelRecord, UUIDHash> s_Models;
    };
}
