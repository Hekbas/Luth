#pragma once

#include "luth/resources/Resource.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace Luth
{
    class MetaFile;

    class FileSystem
    {
    public:
        struct ResourceTypeInfo {
            std::string name;
            fs::path directory;
            std::string extension;
            Vec4 color;
        };

        static void Init(const fs::path& engineRoot = "");

        // Path operations
        static fs::path GetPath(ResourceType type, const fs::path& name, bool addExtension = true);
        static fs::path EnginePath(const fs::path& relative = "");
        static fs::path ProjectPath(const fs::path& relative = "");
        static fs::path AssetsPath(const fs::path& relative = "");

        // Platform paths
        static fs::path PlatformAssetsPath();
        static fs::path LogPath();

        // File utilities
        static bool Exists(const fs::path& path);
        static size_t FileSize(const fs::path& path);
        static bool Validate(const fs::path& path);
        static ResourceType ClassifyFileType(const fs::path& path);

        // Directory management
        static void CreateDirectories(const fs::path& path);
        static void EnsureBaseStructure();

        static const std::unordered_map<ResourceType, ResourceTypeInfo>& GetTypeInfo();

    private:
        static fs::path s_EngineRoot;
        static fs::path s_ProjectRoot;
        static fs::path s_AssetsRoot;
    };
}
