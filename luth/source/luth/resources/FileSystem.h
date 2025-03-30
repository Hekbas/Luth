#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>


namespace Luth
{
    namespace fs = std::filesystem;

    enum class Resource {
        Model,
        Texture,
        Material,
        Shader,
        Font,
        Config,
        Unknown
    };

    class FileSystem
    {
    public:
        static void Init(const fs::path& engineRoot = "");

        // Path operations
        static fs::path GetPath(Resource type, const fs::path& name, bool addExtension = true);
        static fs::path EnginePath(const fs::path& relative = "");
        static fs::path ProjectPath(const fs::path& relative = "");
        static fs::path AssetsPath(const fs::path& relative = "");

        // Platform paths
        static fs::path PlatformAssetsPath();
        static fs::path SaveGamePath();
        static fs::path LogPath();

        // File utilities
        static bool Exists(const fs::path& path);
        static size_t FileSize(const fs::path& path);
        static bool Validate(const fs::path& path);

        // Directory management
        static void CreateDirectories(const fs::path& path);
        static void EnsureBaseStructure();

    private:
        static fs::path s_EngineRoot;
        static fs::path s_ProjectRoot;
        static fs::path s_AssetsRoot;

        struct ResourceTypeInfo {
            fs::path directory;
            std::string extension;
        };

        static const std::unordered_map<Resource, ResourceTypeInfo>& GetTypeInfo();
    };
}
