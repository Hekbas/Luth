#include "luthpch.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"
#include "luth/core/diagnostics/Log.h"
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

    std::unordered_map<UUID, u64, UUIDHash> AssetDatabase::s_ArtifactHashes;

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

            // Check hash and mark dirty if artifact is stale or missing
            if (AssetManager::HasImporter(type))
            {
                fs::path metaP = path; metaP += ".meta";
                u64 currentHash = CalculateAssetHash(path, metaP);
                if (s_ArtifactHashes[uuid] != currentHash || !fs::exists(GetArtifactPath(uuid)))
                {
                    s_ArtifactHashes[uuid] = currentHash;
                    fs::path artifact = GetArtifactPath(uuid);
                    if (fs::exists(artifact)) fs::remove(artifact);
                    s_DirtyAssets.push_back(uuid);
                }
            }
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
        LoadLibraryState_Unlocked();

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
        SaveLibraryState_Unlocked();
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

    void AssetDatabase::ClearDirtyAssets()
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_DirtyAssets.clear();
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
        return GetUUID_Unlocked(path);
    }

    UUID AssetDatabase::GetUUID_Unlocked(const std::filesystem::path& path)
    {
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
        RegisterAsset_Unlocked(path, uuid, type);
    }

    void AssetDatabase::RegisterAsset_Unlocked(const std::filesystem::path& path, UUID uuid, AssetType type)
    {
        s_Assets[uuid] = { path, type };
        s_PathToUuid[path] = uuid;
    }

    void AssetDatabase::UnregisterAsset(UUID uuid)
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        UnregisterAsset_Unlocked(uuid);
    }

    void AssetDatabase::UnregisterAsset_Unlocked(UUID uuid)
    {
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

    void AssetDatabase::LoadLibraryState_Unlocked()  // caller must hold s_Mutex
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

    void AssetDatabase::SaveLibraryState_Unlocked()  // caller must hold s_Mutex
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
        // FNV-1a over timestamps and file size — order-dependent, no information loss.
        u64 hash = 14695981039346656037ULL;  // FNV-1a offset basis
        auto mix = [&hash](u64 val) {
            const u8* bytes = reinterpret_cast<const u8*>(&val);
            for (size_t i = 0; i < sizeof(u64); ++i) {
                hash ^= bytes[i];
                hash *= 1099511628211ULL;  // FNV-1a prime
            }
        };

        if (fs::exists(source)) {
            mix(static_cast<u64>(fs::last_write_time(source).time_since_epoch().count()));
            mix(fs::file_size(source));
        }
        if (fs::exists(meta)) {
            mix(static_cast<u64>(fs::last_write_time(meta).time_since_epoch().count()));
        }

        // For .vert shaders, include the companion .frag in the hash so that
        // cold-start reimport detects when only the .frag changed.
        if (source.extension() == ".vert") {
            fs::path fragPath = source;
            fragPath.replace_extension(".frag");
            if (fs::exists(fragPath)) {
                mix(static_cast<u64>(fs::last_write_time(fragPath).time_since_epoch().count()));
                mix(fs::file_size(fragPath));
            }
        }

        return hash;
    }

    // ================================================================
    // File Ingestion (drag-and-drop, external import)
    // ================================================================

    static bool IsImageExtension(const fs::path& ext)
    {
        static const std::unordered_set<std::string> s_Exts = {
            ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".tiff", ".tif"
        };
        std::string lower = ext.string();
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        return s_Exts.contains(lower);
    }

    void AssetDatabase::IngestFile(const fs::path& sourcePath, const fs::path& destDir)
    {
        try {
            if (!fs::exists(sourcePath)) {
                LH_CORE_ERROR("IngestFile: source not found: {0}", sourcePath.string());
                return;
            }

            AssetType resType = FileSystem::ClassifyFileType(sourcePath);
            if (resType == AssetType::None) {
                LH_CORE_WARN("IngestFile: unsupported file type: {0}", sourcePath.string());
                return;
            }

            fs::path destPath = destDir / sourcePath.filename();
            FileSystem::CreateDirectories(destDir);
            fs::copy_file(sourcePath, destPath, fs::copy_options::overwrite_existing);
            LH_CORE_INFO("Imported {0} to {1}", sourcePath.filename().string(), destPath.string());

            // For model assets, discover and copy adjacent textures
            if (resType == AssetType::Model) {
                fs::path texDestDir = destDir / (sourcePath.stem().string() + "_Textures");
                fs::path srcDir = sourcePath.parent_path();

                std::vector<fs::path> scanDirs = { srcDir };
                static const char* k_SiblingDirs[] = {
                    "textures", "Textures", "texture", "Texture",
                    "tex",      "Tex",      "maps",   "Maps",
                    "images",   "Images"
                };
                for (const char* sub : k_SiblingDirs) {
                    fs::path candidate = srcDir / sub;
                    if (fs::exists(candidate) && fs::is_directory(candidate))
                        scanDirs.push_back(candidate);
                }

                bool copiedAny = false;
                for (const auto& dir : scanDirs) {
                    for (const auto& entry : fs::directory_iterator(dir)) {
                        if (!entry.is_regular_file()) continue;
                        if (!IsImageExtension(entry.path().extension())) continue;

                        if (!copiedAny) {
                            fs::create_directories(texDestDir);
                            copiedAny = true;
                        }

                        fs::path imgDest = texDestDir / entry.path().filename();
                        if (!fs::exists(imgDest))
                            fs::copy_file(entry.path(), imgDest, fs::copy_options::skip_existing);
                    }
                }
                if (copiedAny)
                    LH_CORE_INFO("Copied adjacent textures to {0}", texDestDir.filename().string());
            }

            UUID newUuid = MetaFile::Create(destPath, resType);
            RegisterAsset(destPath, newUuid, resType);

            if (AssetManager::HasImporter(resType))
                AssetManager::Import(newUuid);

            LH_CORE_INFO("Created asset {0} with UUID {1}", destPath.filename().string(), newUuid.ToString());
        }
        catch (const fs::filesystem_error& err) {
            LH_CORE_ERROR("IngestFile failed: {0} - {1}", sourcePath.string(), err.what());
        }
        catch (const std::exception& ex) {
            LH_CORE_ERROR("IngestFile error: {0} - {1}", sourcePath.string(), ex.what());
        }
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

        // Also watch engine assets so that engine shader edits trigger reimport
        if (!s_EngineAssetsRoot.empty() && fs::exists(s_EngineAssetsRoot))
            s_FileWatcher->AddWatch(s_EngineAssetsRoot);

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

        {
            std::lock_guard<std::mutex> lock(s_Mutex);

            for (auto& [path, status] : batch)
            {
                if (status == FileWatcher::FileStatus::Created)
                {
                    AssetType type = FileSystem::ClassifyFileType(path);
                    if (type == AssetType::None) continue;
                    if (GetUUID_Unlocked(path).IsValid()) continue;

                    UUID uuid = UUID::Invalid();
                    fs::path metaPath = path; metaPath += ".meta";
                    if (fs::exists(metaPath)) {
                        MetaFile meta(UUID::Invalid());
                        if (meta.Load(metaPath))
                            uuid = meta.GetUUID();
                    }
                    if (!uuid.IsValid())
                        uuid = MetaFile::Create(path, type);

                    RegisterAsset_Unlocked(path, uuid, type);
                    LH_CORE_INFO("AssetDatabase: Hot-added '{}'", path.filename().string());

                    if (AssetManager::HasImporter(type))
                        s_DirtyAssets.push_back(uuid);

                    anyChange = true;
                }
                else if (status == FileWatcher::FileStatus::Modified)
                {
                    UUID uuid = GetUUID_Unlocked(path);
                    if (!uuid.IsValid()) continue;

                    fs::path metaPath = path; metaPath += ".meta";
                    u64 newHash = CalculateAssetHash(path, metaPath);

                    if (s_ArtifactHashes[uuid] == newHash) continue;
                    s_ArtifactHashes[uuid] = newHash;

                    fs::path artifact = GetArtifactPath(uuid);
                    if (fs::exists(artifact)) fs::remove(artifact);

                    // Drop the in-memory asset so the next GetAsset/LoadAsync
                    // picks up the freshly-cooked artifact. Anything still
                    // holding a shared_ptr keeps the old data alive until
                    // it lets go (no use-after-free).
                    AssetManager::Evict(uuid);

                    s_DirtyAssets.push_back(uuid);
                    LH_CORE_INFO("AssetDatabase: Hot-modified '{}', queued for reimport", path.filename().string());
                    anyChange = true;

                    // If a .frag changed, also mark the paired .vert dirty so its artifact is refreshed
                    if (path.extension() == ".frag")
                    {
                        fs::path vertPath = path;
                        vertPath.replace_extension(".vert");
                        UUID vertUuid = GetUUID_Unlocked(vertPath);
                        if (vertUuid.IsValid())
                        {
                            fs::path vertArtifact = GetArtifactPath(vertUuid);
                            if (fs::exists(vertArtifact)) fs::remove(vertArtifact);
                            s_DirtyAssets.push_back(vertUuid);
                            LH_CORE_INFO("AssetDatabase: .frag changed, cascading reimport to paired '{}'", vertPath.filename().string());
                        }
                    }
                }
                else if (status == FileWatcher::FileStatus::Deleted)
                {
                    UUID uuid = GetUUID_Unlocked(path);
                    if (!uuid.IsValid()) continue;

                    fs::path artifact = GetArtifactPath(uuid);
                    if (fs::exists(artifact)) fs::remove(artifact);

                    fs::path metaPath = path; metaPath += ".meta";
                    if (fs::exists(metaPath)) fs::remove(metaPath);

                    UnregisterAsset_Unlocked(uuid);
                    LH_CORE_INFO("AssetDatabase: Hot-removed '{}'", path.filename().string());
                    anyChange = true;
                }
            }

            if (anyChange)
                SaveLibraryState_Unlocked();
        }

        // Callbacks run outside s_Mutex to avoid holding the lock during user code
        if (anyChange) {
            for (auto& cb : s_ChangeCallbacks)
                cb();
        }
    }
}