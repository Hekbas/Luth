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

    std::unique_ptr<FileWatcher> AssetDatabase::s_FileWatcher;
    std::vector<std::pair<fs::path, FileWatcher::FileStatus>> AssetDatabase::s_PendingChanges;
    std::mutex AssetDatabase::s_PendingMutex;
    std::vector<AssetDatabase::ChangeCallback> AssetDatabase::s_ChangeCallbacks;
    fs::path AssetDatabase::s_ProjectRoot;
    fs::path AssetDatabase::s_EngineAssetsRoot;
    std::unordered_set<UUID, UUIDHash> AssetDatabase::s_EngineUUIDs;

    static std::unordered_map<UUID, u64, UUIDHash> s_ArtifactHashes;

    // ================================================================
    // Phase 1: Engine-only init (register shaders, fonts)
    // ================================================================

    void AssetDatabase::InitEngine(const std::filesystem::path& engineAssetsRoot)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);

        s_EngineAssetsRoot = engineAssetsRoot;
        s_Assets.clear();
        s_PathToUuid.clear();
        s_DirtyAssets.clear();
        s_EngineUUIDs.clear();

        if (engineAssetsRoot.empty() || !fs::exists(engineAssetsRoot))
        {
            LH_CORE_WARN("AssetDatabase: Engine assets root not found: {}", engineAssetsRoot.string());
            return;
        }

        u32 engineAssetCount = 0;
        for (const auto& entry : fs::recursive_directory_iterator(engineAssetsRoot))
        {
            if (!entry.is_regular_file()) continue;
            const auto& path = entry.path();
            if (path.extension() == ".meta") continue;

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
            }

            if (!uuid.IsValid())
            {
                uuid = MetaFile::Create(path, type);
                LH_CORE_INFO("AssetDatabase: Generated meta for engine asset {}", path.filename().string());
            }

            s_Assets[uuid] = { path, type };
            s_PathToUuid[path] = uuid;
            s_EngineUUIDs.insert(uuid);
            engineAssetCount++;
        }

        // Ensure engine artifact cache directory exists
        fs::create_directories(FileSystem::EnginePath("Library/Artifacts"));

        LH_CORE_INFO("AssetDatabase: Registered {} engine assets", engineAssetCount);
    }

    // ================================================================
    // Phase 2: Load project assets (called when user selects a project)
    // ================================================================

    void AssetDatabase::LoadProject(const std::filesystem::path& projectAssetsRoot)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);

        s_ProjectRoot = fs::absolute(projectAssetsRoot).parent_path();
        // projectAssetsRoot IS the <project>/assets/ path, but s_ProjectRoot should be <project>/
        // Actually, let's derive it from FileSystem which already has the project root
        s_ProjectRoot = FileSystem::ProjectPath();

        s_DirtyAssets.clear();

        if (!fs::exists(projectAssetsRoot))
        {
            LH_CORE_WARN("AssetDatabase: Project assets root does not exist: {}", projectAssetsRoot.string());
            return;
        }

        // Ensure Library exists for this project
        fs::create_directories(FileSystem::ProjectPath("Library/Artifacts"));
        LoadLibraryState();

        // Phase 1: Collect all file paths first.
        std::vector<fs::path> metaFiles;
        std::vector<fs::path> assetFiles;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(projectAssetsRoot))
        {
            if (!entry.is_regular_file()) continue;

            const auto& path = entry.path();
            if (path.extension() == ".meta")
                metaFiles.push_back(path);
            else
                assetFiles.push_back(path);
        }

        u32 projectAssetCount = 0;

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
                    LH_CORE_ERROR("AssetDatabase: Failed to load meta file: {}", metaPath.string());
            }

            if (!uuid.IsValid())
            {
                uuid = MetaFile::Create(path, type);
                LH_CORE_INFO("AssetDatabase: Generated meta file for {} -> UUID {}", path.filename().string(), uuid.ToString());
            }

            s_Assets[uuid] = { path, type };
            s_PathToUuid[path] = uuid;
            projectAssetCount++;

            // Only check hash/reimport for types that have importers
            if (AssetManager::HasImporter(type))
            {
                u64 currentHash = CalculateAssetHash(path, metaPath);
                if (s_ArtifactHashes[uuid] != currentHash || !fs::exists(GetArtifactPath(uuid)))
                {
                    LH_CORE_INFO("AssetDatabase: Re-importing {}", path.filename().string());
                    s_ArtifactHashes[uuid] = currentHash;
                    fs::path artifact = GetArtifactPath(uuid);
                    if (fs::exists(artifact)) fs::remove(artifact);
                    s_DirtyAssets.push_back(uuid);
                }
            }
        }

        // Phase 3: Delete orphaned .meta files (source asset was deleted)
        for (const auto& path : metaFiles)
        {
            fs::path assetPath = path;
            assetPath.replace_extension("");
            if (!fs::exists(assetPath))
            {
                LH_CORE_WARN("AssetDatabase: Deleting orphaned meta file: {}", path.filename().string());
                fs::remove(path);
            }
        }

        LH_CORE_INFO("AssetDatabase: Scanned {} project assets", projectAssetCount);
        SaveLibraryState();
    }

    // ================================================================
    // Unload project assets (keeps engine assets intact)
    // ================================================================

    void AssetDatabase::UnloadProject()
    {
        StopWatching();

        std::lock_guard<std::mutex> lock(s_Mutex);

        // Remove all non-engine assets from the registry
        std::vector<UUID> toRemove;
        for (const auto& [uuid, meta] : s_Assets)
        {
            // Keep assets whose path starts with the engine assets root
            std::string pathStr = meta.Path.string();
            std::string engineStr = s_EngineAssetsRoot.string();
            if (pathStr.rfind(engineStr, 0) != 0) // Not under engine root
                toRemove.push_back(uuid);
        }

        for (const auto& uuid : toRemove)
        {
            s_PathToUuid.erase(s_Assets[uuid].Path);
            s_Assets.erase(uuid);
        }

        s_DirtyAssets.clear();
        s_ArtifactHashes.clear();
        s_ProjectRoot.clear();

        LH_CORE_INFO("AssetDatabase: Project unloaded, {} engine assets remain", s_Assets.size());
    }

    // ================================================================
    // Shutdown
    // ================================================================

    void AssetDatabase::Shutdown()
    {
        StopWatching();

        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Assets.clear();
        s_PathToUuid.clear();
        s_DirtyAssets.clear();
        s_ArtifactHashes.clear();
        s_ChangeCallbacks.clear();
        s_EngineUUIDs.clear();
    }

    // ================================================================
    // Queries & Registration
    // ================================================================

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
        if (s_EngineUUIDs.count(uuid))
            return FileSystem::EnginePath("Library/Artifacts") / (uuid.ToString() + ".luth");
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

    // ================================================================
    // Library State Persistence
    // ================================================================

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
        if (!FileSystem::HasProject()) return;

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

    // ================================================================
    // File System Watching
    // ================================================================

    void AssetDatabase::StartWatching()
    {
        if (s_FileWatcher) return;
        if (s_ProjectRoot.empty()) return;

        s_FileWatcher = std::make_unique<FileWatcher>(1.0f);
        s_FileWatcher->AddWatch(s_ProjectRoot);

        s_FileWatcher->SetCallback([](const fs::path& path, FileWatcher::FileStatus status) {
            if (path.extension() == ".meta") return;
            if (path.string().find("Library") != std::string::npos) return;

            std::lock_guard<std::mutex> lock(s_PendingMutex);
            s_PendingChanges.push_back({ path, status });
        });

        s_FileWatcher->Start(true);
        LH_CORE_INFO("AssetDatabase: File watcher started on '{}'", s_ProjectRoot.string());
    }

    void AssetDatabase::StopWatching()
    {
        if (s_FileWatcher) {
            s_FileWatcher->Stop();
            s_FileWatcher.reset();
        }
    }

    void AssetDatabase::AddChangeCallback(ChangeCallback cb)
    {
        s_ChangeCallbacks.push_back(std::move(cb));
    }

    void AssetDatabase::ProcessPendingChanges()
    {
        std::vector<std::pair<fs::path, FileWatcher::FileStatus>> batch;
        {
            std::lock_guard<std::mutex> lock(s_PendingMutex);
            if (s_PendingChanges.empty()) return;
            batch.swap(s_PendingChanges);
        }

        bool anyChange = false;

        for (auto& [path, status] : batch)
        {
            if (status == FileWatcher::FileStatus::Created)
            {
                AssetType type = FileSystem::ClassifyFileType(path);
                if (type == AssetType::None) continue;
                if (GetUUID(path).IsValid()) continue;

                UUID uuid = UUID::Invalid();
                fs::path metaPath = path; metaPath += ".meta";
                if (fs::exists(metaPath)) {
                    MetaFile meta(UUID::Invalid());
                    if (meta.Load(metaPath))
                        uuid = meta.GetUUID();
                }
                if (!uuid.IsValid())
                    uuid = MetaFile::Create(path, type);

                RegisterAsset(path, uuid, type);
                LH_CORE_INFO("AssetDatabase: Hot-added '{}'", path.filename().string());

                if (AssetManager::HasImporter(type)) {
                    std::lock_guard<std::mutex> lock(s_Mutex);
                    s_DirtyAssets.push_back(uuid);
                }
                anyChange = true;
            }
            else if (status == FileWatcher::FileStatus::Modified)
            {
                UUID uuid = GetUUID(path);
                if (!uuid.IsValid()) continue;

                fs::path metaPath = path; metaPath += ".meta";
                u64 newHash = CalculateAssetHash(path, metaPath);

                {
                    std::lock_guard<std::mutex> lock(s_Mutex);
                    if (s_ArtifactHashes[uuid] == newHash) continue;
                    s_ArtifactHashes[uuid] = newHash;

                    fs::path artifact = GetArtifactPath(uuid);
                    if (fs::exists(artifact)) fs::remove(artifact);

                    s_DirtyAssets.push_back(uuid);
                }
                LH_CORE_INFO("AssetDatabase: Hot-modified '{}', queued for reimport", path.filename().string());
                anyChange = true;
            }
            else if (status == FileWatcher::FileStatus::Deleted)
            {
                UUID uuid = GetUUID(path);
                if (!uuid.IsValid()) continue;

                fs::path artifact = GetArtifactPath(uuid);
                if (fs::exists(artifact)) fs::remove(artifact);

                fs::path metaPath = path; metaPath += ".meta";
                if (fs::exists(metaPath)) fs::remove(metaPath);

                UnregisterAsset(uuid);
                LH_CORE_INFO("AssetDatabase: Hot-removed '{}'", path.filename().string());
                anyChange = true;
            }
        }

        if (anyChange) {
            SaveLibraryState();
            for (auto& cb : s_ChangeCallbacks)
                cb();
        }
    }
}