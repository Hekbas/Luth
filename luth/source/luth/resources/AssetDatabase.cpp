#include "luthpch.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"

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

        // Initial Scan
        for (const auto& entry : std::filesystem::recursive_directory_iterator(projectRoot))
        {
            if (entry.is_regular_file() && entry.path().extension() != ".meta")
            {
                AssetType type = FileSystem::ClassifyFileType(entry.path());
                if (type != AssetType::None)
                {
                    UUID uuid;
                    fs::path metaPath = entry.path();
                    metaPath += ".meta";

                    if (fs::exists(metaPath))
                    {
                        // Load existing UUID
                        MetaFile meta(UUID::Invalid());
                        if (meta.Load(metaPath))
                            uuid = meta.GetUUID();
                    }
                    
                    if (!uuid.IsValid())
                    {
                        // Generate new meta file
                        uuid = MetaFile::Create(entry.path(), type);
                    }

                    s_Assets[uuid] = { entry.path(), type };
                    s_PathToUuid[entry.path()] = uuid;
                }
            }
        }
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