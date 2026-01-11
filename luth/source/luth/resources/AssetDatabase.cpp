#include "luthpch.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"
#include "luth/core/Log.h"
#include "luth/resources/AssetManager.h"
#include <fstream>

namespace Luth
{
    std::unordered_map<UUID, AssetMetadata, UUIDHash> AssetDatabase::s_Assets;
    std::unordered_map<std::filesystem::path, UUID> AssetDatabase::s_PathToUuid;
    std::mutex AssetDatabase::s_Mutex;
    std::vector<UUID> AssetDatabase::s_DirtyAssets;
    
    static std::unordered_map<UUID, u64, UUIDHash> s_ArtifactHashes;

    void AssetDatabase::Init(const std::filesystem::path& projectRoot)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Assets.clear();
        s_PathToUuid.clear();
        s_DirtyAssets.clear();

        if (!fs::exists(projectRoot))
        {
            LH_CORE_WARN("AssetDatabase: Project root does not exist: {0}", projectRoot.string());
            return;
        }

        // Ensure Library exists
        fs::create_directories(FileSystem::ProjectPath("Library/Artifacts"));

        LoadLibraryState();

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

                // Check Hash
                u64 currentHash = CalculateAssetHash(path, metaPath);
                if (s_ArtifactHashes[uuid] != currentHash || !fs::exists(GetArtifactPath(uuid)))
                {
                    LH_CORE_INFO("AssetDatabase: Re-importing {0}", path.filename().string());
                    // Trigger synchronous import for simplicity at startup, or queue async
                    // For now, we just mark it as needing import by removing the hash, AssetManager will handle it on load
                    // Better: Trigger AssetManager to import it now to ensure Library is up to date
                    s_ArtifactHashes[uuid] = currentHash; // Optimistically update, assuming AssetManager will succeed or we force it
                    // We rely on AssetManager::LoadJob checking existence, but here we want to force update if hash changed.
                    // Since AssetManager::LoadAsync checks artifact existence, we need a way to force re-import.
                    // Actually, let's just delete the artifact if hash mismatch.
                    fs::path artifact = GetArtifactPath(uuid);
                    if (fs::exists(artifact)) fs::remove(artifact);
                    s_DirtyAssets.push_back(uuid);
                }
            }
        }

        // 3. Delete orphans
        for (const auto& path : metaFilesToDelete)
        {
            LH_CORE_WARN("AssetDatabase: Deleting orphaned meta file: {0}", path.filename().string());
            fs::remove(path);
        }

        LH_CORE_INFO("AssetDatabase: Initialized with {0} assets", s_Assets.size());
        SaveLibraryState();
    }

    void AssetDatabase::Shutdown()
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Assets.clear();
        s_PathToUuid.clear();
        s_DirtyAssets.clear();
        s_ArtifactHashes.clear();
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

    std::filesystem::path AssetDatabase::GetArtifactPath(UUID uuid)
    {
        return FileSystem::ProjectPath("Library/Artifacts") / (uuid.ToString() + ".luth");
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

    void AssetDatabase::LoadLibraryState()
    {
        fs::path path = FileSystem::ProjectPath("Library/State.json");
        if (!fs::exists(path)) return;

        std::ifstream file(path);
        nlohmann::json json;
        file >> json;

        for (auto& [key, value] : json.items()) {
            s_ArtifactHashes[UUID::FromString(key)] = value.get<u64>();
        }
    }

    void AssetDatabase::SaveLibraryState()
    {
        nlohmann::json json;
        for (const auto& [uuid, hash] : s_ArtifactHashes) {
            json[uuid.ToString()] = hash;
        }

        fs::path path = FileSystem::ProjectPath("Library/State.json");
        std::ofstream file(path);
        file << json.dump(4);
    }

    u64 AssetDatabase::CalculateAssetHash(const fs::path& source, const fs::path& meta)
    {
        // Simple timestamp + size hash for now. 
        // In production, read file content or use CRC32 of content.
        u64 hash = 0;
        if (fs::exists(source)) {
            hash ^= fs::last_write_time(source).time_since_epoch().count();
            hash ^= fs::file_size(source);
        }
        if (fs::exists(meta)) {
            hash ^= fs::last_write_time(meta).time_since_epoch().count();
        }
        return hash;
    }
}