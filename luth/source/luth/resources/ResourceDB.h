#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/FileWatcher.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace Luth
{
    class ResourceDB
    {
    public:
        struct ResourceInfo {
            fs::path Path;
            ResourceType Type;
            bool Dirty;
        };

        static void Init(const fs::path& projectRoot);
        static void Update();
        static void Shutdown();

        // UUID <-> Info mapping
        static const ResourceInfo& UuidToInfo(const UUID& uuid);
        static UUID PathToUuid(const fs::path& path);

        // Update operations
        static void RegisterAsset(const fs::path& path, const UUID& uuid);
        static void UnregisterAsset(const fs::path& path);
        static void UpdateAssetPath(const fs::path& oldPath, const fs::path& newPath);

        // Dependency resolution
        static std::vector<UUID> GetAllDependencies(const UUID& uuid);

        // Set dirty
        static void SetDirty(UUID uuid);
        static void SaveDirty();

        // Utility functions
        static bool IsAssetPath(const fs::path& path);

    private:
        static bool ProcessMetaFile(const fs::path& path);
        static void CleanOrphanedMetaFiles(const fs::path& projectRoot);

        static void OnFileChanged(const fs::path& path, FileWatcher::FileStatus status);
        static void ProcessReloadQueue();
		static void HandleFileModified(const fs::path& path);
		static void HandleFileCreated(const fs::path& path);
		static void HandleFileDeleted(const fs::path& path);

    private:
        static std::unordered_map<UUID, ResourceInfo, UUIDHash> s_UuidToInfo;
        static std::unordered_map<fs::path, UUID> s_PathToUuid;

        static std::unique_ptr<FileWatcher> s_FileWatcher;
        static std::vector<std::pair<fs::path, FileWatcher::FileStatus>> s_ReloadQueue;
        static std::mutex s_QueueMutex;
    };
}
