#pragma once

#include "luth/renderer/Model.h"

#include <filesystem>
#include <shared_mutex>

namespace Luth
{
    class ModelLibrary
    {
    public:
        static void Init();
        static void Shutdown();

        static bool Add(const std::string& name, std::shared_ptr<Model> model);
        static bool Remove(const std::string& name);
        static std::shared_ptr<Model> Get(const std::string& name);
        static bool Contains(const std::string& name);
        static std::vector<std::string> GetModelNames();

        static std::shared_ptr<Model> Load(const std::filesystem::path& path);
        static std::shared_ptr<Model> Load(const std::string& name, const std::filesystem::path& path);
        static std::shared_ptr<Model> LoadOrGet(const std::filesystem::path& path);
        static std::shared_ptr<Model> LoadOrGet(const std::string& name, const std::filesystem::path& path);

        static bool Reload(const std::string& name);
        static void ReloadAll();

    private:
        struct ModelRecord {
            std::shared_ptr<Model> model;
            std::filesystem::path sourcePath;
            std::filesystem::file_time_type lastModified;
        };

        static inline std::shared_mutex s_Mutex;
        static inline std::unordered_map<std::string, ModelRecord> s_Models;
    };
}
