#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"
#include "luth/resources/FileWatcher.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace Luth
{
    struct AssetMetadata
    {
        std::filesystem::path Path;
        AssetType Type = AssetType::None;
        bool IsLoaded = false;
    };

    class AssetDatabase
    {
    public:
        /// Phase 1: Register engine-internal assets (shaders, fonts). No project needed.
        static void InitEngine(const std::filesystem::path& engineAssetsRoot);

        /// Phase 2: Scan a project's assets directory. Can be called multiple times (project switching).
        static void LoadProject(const std::filesystem::path& projectAssetsRoot);

        /// Clear project-specific assets (keeps engine assets). Used during project switching.
        static void UnloadProject();

        static void Shutdown();

        // Queries
        static const AssetMetadata& GetMetadata(UUID uuid);
        static UUID GetUUID(const std::filesystem::path& path);
        static std::filesystem::path GetArtifactPath(UUID uuid);
        static bool Exists(UUID uuid);

        // Registry Management
        static void RegisterAsset(const std::filesystem::path& path, UUID uuid, AssetType type);
        static void UnregisterAsset(UUID uuid);

        static const std::unordered_map<UUID, AssetMetadata, UUIDHash>& GetRegistry() { return s_Assets; }
        static const std::vector<UUID>& GetDirtyAssets() { return s_DirtyAssets; }

        // Hot reload — file system watching
        using ChangeCallback = std::function<void()>;
        static void StartWatching();
        static void StopWatching();
        static void ProcessPendingChanges();
        static void AddChangeCallback(ChangeCallback cb);

    private:
        static void LoadLibraryState();
        static void SaveLibraryState();
        static u64 CalculateAssetHash(const fs::path& source, const fs::path& meta);

        static std::unordered_map<UUID, AssetMetadata, UUIDHash> s_Assets;
        static std::unordered_map<std::filesystem::path, UUID> s_PathToUuid;
        static std::vector<UUID> s_DirtyAssets;
        static std::mutex s_Mutex;

        // File watcher state
        static std::unique_ptr<FileWatcher> s_FileWatcher;
        static std::vector<std::pair<fs::path, FileWatcher::FileStatus>> s_PendingChanges;
        static std::mutex s_PendingMutex;
        static std::vector<ChangeCallback> s_ChangeCallbacks;

        static fs::path s_ProjectRoot;
        static fs::path s_EngineAssetsRoot;
    };
}