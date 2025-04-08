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
        static void Init(const fs::path& projectRoot);

        // UUID <-> Path mapping
        static fs::path UuidToPath(const UUID& uuid);
        static UUID PathToUuid(const fs::path& path);

        // Update operations
        static void RegisterAsset(const fs::path& path, const UUID& uuid);
        static void UnregisterAsset(const fs::path& path);

        // Dependency resolution
        static std::vector<UUID> GetAllDependencies(const UUID& uuid);

    private:
        static bool ProcessMetaFile(const fs::path& path);

    private:
        static std::unordered_map<UUID, ResourceType, UUIDHash> s_UuidToType;
        static std::unordered_map<UUID, fs::path, UUIDHash> s_UuidToPath;
        static std::unordered_map<fs::path, UUID> s_PathToUuid;

    };
}
