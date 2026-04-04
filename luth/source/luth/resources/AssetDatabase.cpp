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

    static std::unordered_map<UUID, u64, UUIDHash> s_ArtifactHashes;

    void AssetDatabase::Init(const std::filesystem::path& projectRoot, const std::filesystem::path& engineAssetsRoot)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_ProjectRoot = fs::absolute(projectRoot);
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

            // Only check hash/reimport for types that have importers
            if (AssetManager::HasImporter(type))
            {
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

        LH_CORE_INFO("AssetDatabase: Initialized with {0} project assets", s_Assets.size());
        SaveLibraryState();

        // Phase 4: Scan engine assets (read-only — no reimport, no Library state)
        s_EngineAssetsRoot = engineAssetsRoot;
        if (!engineAssetsRoot.empty() && fs::exists(engineAssetsRoot))
        {
            u32 engineAssetCount = 0;
            for (const auto& entry : fs::recursive_directory_iterator(engineAssetsRoot))
            {
                if (!entry.is_regular_file()) continue;
                const auto& path = entry.path();
                if (path.extension() == ".meta") continue;

                AssetType type = FileSystem::ClassifyFileType(path);
                if (type == AssetType::None) continue;

                // Skip if already registered (shouldn't happen, but safe)
                if (s_PathToUuid.count(path)) continue;

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
                    LH_CORE_INFO("AssetDatabase: Generated meta for engine asset {0}", path.filename().string());
                }

                s_Assets[uuid] = { path, type };
                s_PathToUuid[path] = uuid;
                engineAssetCount++;
            }
            LH_CORE_INFO("AssetDatabase: Registered {0} engine assets from '{1}'",
                engineAssetCount, engineAssetsRoot.string());
        }

    }

    void AssetDatabase::Shutdown()
    {
        StopWatching();

        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Assets.clear();
        s_PathToUuid.clear();
        s_DirtyAssets.clear();
        s_ArtifactHashes.clear();
        s_ChangeCallbacks.clear();
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

    // ── File System Watching ───────────────────────────────────────────────────

    void AssetDatabase::StartWatching()
    {
        if (s_FileWatcher) return;
        if (s_ProjectRoot.empty()) return;

        s_FileWatcher = std::make_unique<FileWatcher>(1.0f);
        s_FileWatcher->AddWatch(s_ProjectRoot);

        s_FileWatcher->SetCallback([](const fs::path& path, FileWatcher::FileStatus status) {
            // Skip .meta files — managed internally
            if (path.extension() == ".meta") return;

            // Skip the Library/ directory (artifacts, state)
            if (path.string().find("Library") != std::string::npos) return;

            std::lock_guard<std::mutex> lock(s_PendingMutex);
            s_PendingChanges.push_back({ path, status });
        });

        // initialScan=true: populate baseline without firing callbacks for existing files
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
        // Drain queue under pending-mutex (short lock, watcher thread may be writing)
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

                // Might already be registered (importer wrote it, watcher caught it)
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
                    if (s_ArtifactHashes[uuid] == newHash) continue; // unchanged
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

                // Clean up artifact and .meta
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