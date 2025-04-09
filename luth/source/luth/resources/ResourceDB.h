#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/MetaFile.h"

#include <filesystem>
#include <unordered_map>

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

        // UUID <-> Info mapping
        static ResourceInfo UuidToInfo(const UUID& uuid);
        static UUID PathToUuid(const fs::path& path);

        // Update operations
        static void RegisterAsset(const fs::path& path, const UUID& uuid);
        static void UnregisterAsset(const fs::path& path);

        // Dependency resolution
        static std::vector<UUID> GetAllDependencies(const UUID& uuid);

        // Set dirty
        static void SetDirty(UUID uuid);
        static void SaveDirty();

    private:
        static bool ProcessMetaFile(const fs::path& path);

    private:
        static std::unordered_map<UUID, ResourceInfo, UUIDHash> s_UuidToInfo;
        static std::unordered_map<fs::path, UUID> s_PathToUuid;

    };
}
