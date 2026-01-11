#include "luthpch.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"
#include "luth/core/Log.h"

namespace Luth
{
    std::unordered_map<UUID, AssetMetadata, UUIDHash> AssetDatabase::s_Assets;
    std::unordered_map<std::filesystem::path, UUID> AssetDatabase::s_PathToUuid;
    std::mutex AssetDatabase::s_Mutex;

    void AssetDatabase::Init(const std::filesystem::path& projectRoot)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Assets.clear();
        s_PathToUuid.clear();

        if (!fs::exists(projectRoot))
        {
            LH_CORE_WARN("AssetDatabase: Project root does not exist: {0}", projectRoot.string());
            return;
        }

        std::vector<fs::path> metaFilesToDelete;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(projectRoot))
        {
            if (!entry.is_regular_file()) continue;

            const auto& path = entry.path();

            // 1. Check for orphaned .meta files
            if (path.extension() == ".meta")
            {
                fs::path assetPath = path;
                assetPath.replace_extension(""); // Remove .meta to get original asset path
                if (!fs::exists(assetPath))
                {
                    metaFilesToDelete.push_back(path);
                }
                continue;
            }

            // 2. Process Asset files
            AssetType type = FileSystem::ClassifyFileType(path);
            if (type != AssetType::None)
            {
                UUID uuid;
                fs::path metaPath = path;
                metaPath += ".meta";

                if (fs::exists(metaPath))
                {
                    MetaFile meta(UUID::Invalid());
                    if (meta.Load(metaPath))
                        uuid = meta.GetUUID();
                    else
                        LH_CORE_ERROR("AssetDatabase: Failed to load meta file: {0}", metaPath.string());
                }
                
                if (!uuid.IsValid())
                {
                    uuid = MetaFile::Create(path, type);
                    LH_CORE_INFO("AssetDatabase: Generated meta file for {0}", path.filename().string());
                }

                s_Assets[uuid] = { path, type };
                s_PathToUuid[path] = uuid;
            }
        }

        // 3. Delete orphans
        for (const auto& path : metaFilesToDelete)
        {
            LH_CORE_WARN("AssetDatabase: Deleting orphaned meta file: {0}", path.filename().string());
            fs::remove(path);
        }

        LH_CORE_INFO("AssetDatabase: Initialized with {0} assets", s_Assets.size());
    }

    void AssetDatabase::Shutdown()
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Assets.clear();
        s_PathToUuid.clear();
    }

    const AssetMetadata& AssetDatabase::GetMetadata(UUID uuid)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        static AssetMetadata empty;
        auto it = s_Assets.find(uuid);
        return (it != s_Assets.end()) ? it->second : empty;
    }

    UUID AssetDatabase::GetUUID(const std::filesystem::path& path)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        auto it = s_PathToUuid.find(path);
        return (it != s_PathToUuid.end()) ? it->second : UUID::Invalid();
    }

    bool AssetDatabase::Exists(UUID uuid)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        return s_Assets.find(uuid) != s_Assets.end();
    }

    void AssetDatabase::RegisterAsset(const std::filesystem::path& path, UUID uuid, AssetType type)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Assets[uuid] = { path, type };
        s_PathToUuid[path] = uuid;
    }

    void AssetDatabase::UnregisterAsset(UUID uuid)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        auto it = s_Assets.find(uuid);
        if (it != s_Assets.end())
        {
            s_PathToUuid.erase(it->second.Path);
            s_Assets.erase(it);
        }
    }
}