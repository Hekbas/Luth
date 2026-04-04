#pragma once

#include "luth/resources/Asset.h"

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

        /// Phase 1: Initialize engine root only. Call at startup before any project is loaded.
        static void InitEngine(const fs::path& engineRoot);

        /// Phase 2: Set the project root. Called when a project is selected.
        static void SetProjectRoot(const fs::path& projectRoot);

        /// Clear project root (when switching away from a project before loading a new one).
        static void ClearProject();

        /// Returns true if a project is currently loaded.
        static bool HasProject();

        // Path operations
        static fs::path GetPath(AssetType type, const fs::path& name, bool addExtension = true);
        static fs::path EnginePath(const fs::path& relative = "");
        static fs::path ProjectPath(const fs::path& relative = "");
        static fs::path AssetsPath(const fs::path& relative = "");

        /// Returns the engine's internal assets directory (luth/assets/).
        static fs::path EngineAssetsPath(const fs::path& relative = "");

        /// Search project assets first, then engine assets.
        static fs::path ResolveAsset(const fs::path& relative);

        // Platform paths
        static fs::path PlatformAssetsPath();
        static fs::path LogPath();

        // File utilities
        static bool Exists(const fs::path& path);
        static size_t FileSize(const fs::path& path);
        static bool Validate(const fs::path& path);
        static AssetType ClassifyFileType(const fs::path& path);

        // Directory management
        static void CreateDirectories(const fs::path& path);
        static void EnsureBaseStructure();

        static const std::unordered_map<AssetType, ResourceTypeInfo>& GetTypeInfo();

    private:
        static fs::path s_EngineRoot;        // luth/ engine directory
        static fs::path s_EngineAssetsRoot;   // luth/assets/
        static fs::path s_ProjectRoot;        // User project directory
        static fs::path s_AssetsRoot;         // <project>/assets/
        static bool     s_HasProject;
    };
}
