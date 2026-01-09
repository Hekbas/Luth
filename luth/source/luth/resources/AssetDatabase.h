#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"
#include <filesystem>
#include <unordered_map>
#include <mutex>

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
        static void Init(const std::filesystem::path& projectRoot);
        static void Shutdown();

        // Queries
        static const AssetMetadata& GetMetadata(UUID uuid);
        static UUID GetUUID(const std::filesystem::path& path);
        static bool Exists(UUID uuid);

        // Registry Management
        static void RegisterAsset(const std::filesystem::path& path, UUID uuid, AssetType type);
        static void UnregisterAsset(UUID uuid);

        static const std::unordered_map<UUID, AssetMetadata, UUIDHash>& GetRegistry() { return s_Assets; }

    private:
        static std::unordered_map<UUID, AssetMetadata, UUIDHash> s_Assets;
        static std::unordered_map<std::filesystem::path, UUID> s_PathToUuid;
        static std::mutex s_Mutex;
    };
}