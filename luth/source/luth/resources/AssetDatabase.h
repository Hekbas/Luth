#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"
#include "luth/resources/FileWatcher.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>

namespace Luth
{
    // UUID-to-path index for engine and project assets. Two-phase lifecycle: InitEngine registers
    // built-ins (engine shaders, fonts) at startup, then LoadProject scans the user's .luthproj
    // assets directory once selected. File-watching invalidates entries on external edits;
    // subscribers see batched dirty-UUID lists through ChangeCallback.
    struct AssetMetadata
    {
        std::filesystem::path Path;
        AssetType Type = AssetType::None;
        bool IsLoaded = false;
    };

    class AssetDatabase
    {
    public:
        // Engine-only init: register engine-internal assets (shaders, fonts). No project needed.
        // Call at startup before any project is loaded.
        static void InitEngine(const std::filesystem::path& engineAssetsRoot);

        // Scan a project's assets directory and register everything found. Safe to call multiple
        // times (project switching) — UnloadProject is the matching teardown.
        static void LoadProject(const std::filesystem::path& projectAssetsRoot);

        // Clear project-specific assets while keeping the engine built-ins intact. Used during
        // project switching to preserve shader/font state across the swap.
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
        static void ClearDirtyAssets();

        // Copy a source file into the project assets directory, create the .meta sidecar, register
        // in the database, and run import if a matching importer exists. For model assets this
        // also discovers and copies adjacent textures so the import lands self-contained.
        static void IngestFile(const std::filesystem::path& sourcePath, const std::filesystem::path& destDir);

        // Hot reload — file system watching
        using ChangeCallback = std::function<void()>;
        static void StartWatching();
        static void StopWatching();
        static void ProcessPendingChanges();
        static void AddChangeCallback(ChangeCallback cb);

        // Editor self-write hint: the next file-change event for this asset is the editor saving its
        // own in-memory state — skip the reimport (which would evict the live instance being edited).
        static void SuppressNextReimport(const UUID& uuid);

    private:
        static void LoadLibraryState_Unlocked();
        static void SaveLibraryState_Unlocked();
        static u64 CalculateAssetHash(const fs::path& source, const fs::path& meta);

        // _Unlocked variants — caller must hold s_Mutex
        static UUID GetUUID_Unlocked(const fs::path& path);
        static void RegisterAsset_Unlocked(const fs::path& path, UUID uuid, AssetType type);
        static void UnregisterAsset_Unlocked(UUID uuid);

        static std::unordered_map<UUID, AssetMetadata, UUIDHash> s_Assets;
        static std::unordered_map<std::filesystem::path, UUID> s_PathToUuid;
        static std::vector<UUID> s_DirtyAssets;
        static std::unordered_map<UUID, u64, UUIDHash> s_ArtifactHashes;
        static std::mutex s_Mutex;

        // File watcher state
        static std::unique_ptr<FileWatcher> s_FileWatcher;
        static std::vector<std::pair<fs::path, FileWatcher::FileStatus>> s_PendingChanges;
        static std::mutex s_PendingMutex;
        static std::vector<ChangeCallback> s_ChangeCallbacks;

        // Self-write suppression (see SuppressNextReimport) — consumed in ProcessPendingChanges.
        static std::unordered_set<UUID, UUIDHash> s_SelfWrites;
        static std::mutex s_SelfWriteMutex;
        static bool ConsumeSelfWrite(const UUID& uuid);

        static fs::path s_ProjectRoot;
        static fs::path s_EngineAssetsRoot;
        static std::unordered_set<UUID, UUIDHash> s_EngineUUIDs;
    };
}