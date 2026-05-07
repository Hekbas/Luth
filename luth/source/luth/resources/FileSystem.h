#pragma once

#include "luth/resources/Asset.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace Luth
{
    class MetaFile;

    // Engine-and-project root manager. Two-phase init: InitEngine pins the engine-assets root at
    // startup, then SetProjectRoot binds the active .luthproj root once the user selects one.
    // ResolveAsset searches project-first then falls back to engine, so projects can shadow engine
    // assets by relative path without recompiling the engine.
    class FileSystem
    {
    public:
        struct ResourceTypeInfo {
            std::string name;
            fs::path directory;
            std::string extension;
            Vec4 color;
        };

        // Engine-only init: pin the engine root. Call at startup before any project is loaded.
        static void InitEngine(const fs::path& engineRoot);

        // Bind the project root. Called when a project is selected, either at startup from a CLI
        // arg or later through the project launcher.
        static void SetProjectRoot(const fs::path& projectRoot);

        // Clear the project root when switching away from a project, before loading a new one.
        static void ClearProject();

        // True if a project is currently loaded.
        static bool HasProject();

        // Path operations
        static fs::path GetPath(AssetType type, const fs::path& name, bool addExtension = true);
        static fs::path EnginePath(const fs::path& relative = "");
        static fs::path ProjectPath(const fs::path& relative = "");
        static fs::path AssetsPath(const fs::path& relative = "");

        // Engine-internal assets directory (luth/assets/), with optional relative subpath.
        static fs::path EngineAssetsPath(const fs::path& relative = "");

        // Search project assets first, then fall back to engine assets. The lookup that lets a
        // project shadow an engine-shipped asset by relative path.
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
