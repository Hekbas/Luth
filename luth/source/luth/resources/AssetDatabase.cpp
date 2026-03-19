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

        // Phase 1: Collect all file paths first.
        // IMPORTANT: We must NOT create files during recursive_directory_iterator traversal,
        // as modifying a directory during iteration is undefined behavior and can cause skipped files.
        std::vector<fs::path> metaFiles;
        std::vector<fs::path> assetFiles;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(projectRoot))
        {
            if (!entry.is_regular_file()) continue;

            const auto& path = entry.path();
            if (path.extension() == ".meta")
                metaFiles.push_back(path);
            else
                assetFiles.push_back(path);
        }

        // Phase 2: Process asset files — create .meta if missing, register in DB
        for (const auto& path : assetFiles)
        {
            AssetType type = FileSystem::ClassifyFileType(path);
            if (type == AssetType::None) continue;

            UUID uuid = UUID::Invalid();
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
                LH_CORE_INFO("AssetDatabase: Generated meta file for {0} -> UUID {1}", path.filename().string(), uuid.ToString());
            }

            s_Assets[uuid] = { path, type };
            s_PathToUuid[path] = uuid;

            // Check Hash
            u64 currentHash = CalculateAssetHash(path, metaPath);
            if (s_ArtifactHashes[uuid] != currentHash || !fs::exists(GetArtifactPath(uuid)))
            {
                LH_CORE_INFO("AssetDatabase: Re-importing {0}", path.filename().string());
                s_ArtifactHashes[uuid] = currentHash;
                fs::path artifact = GetArtifactPath(uuid);
                if (fs::exists(artifact)) fs::remove(artifact);
                s_DirtyAssets.push_back(uuid);
            }
        }

        // Phase 3: Delete orphaned .meta files (source asset was deleted)
        for (const auto& path : metaFiles)
        {
            fs::path assetPath = path;
            assetPath.replace_extension(""); // Remove .meta to get original asset path
            if (!fs::exists(assetPath))
            {
                LH_CORE_WARN("AssetDatabase: Deleting orphaned meta file: {0}", path.filename().string());
                fs::remove(path);
            }
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