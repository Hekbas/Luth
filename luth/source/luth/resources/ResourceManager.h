#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>


namespace Luth
{
    namespace fs = std::filesystem;

    enum class Resource
    {
        Model,
        Texture,
        Material,
        Shader,
        Font,
        Config
    };

    class ResourceManager
    {
    public:
        // Initialize resource system
        static void Init(const fs::path& engineRoot = "");

        // Path management
        static void SetBasePath(const fs::path& basePath);
        static const fs::path& GetBasePath();

        // Resource path resolution
        static fs::path GetPath(Resource type, const fs::path& resourceName, bool extension = true);
        static fs::path GetEnginePath(const fs::path& relativePath);
        static fs::path GetProjectPath(const fs::path& relativePath);

        // File utilities
        static bool FileExists(const fs::path& path);
        static bool ValidateResourcePath(const fs::path& path);
        static size_t GetFileSize(const fs::path& path);

        // Resource information
        static std::string GetResourceTypeName(Resource type);
        static std::vector<fs::path> FindResources(Resource type, const std::string& pattern = "*", bool recursive = false);
        static std::vector<fs::path> FindResourceByName(Resource type, const std::string& targetName);

        // Platform utilities
        static fs::path GetPlatformAssetPath();
        static fs::path GetSaveGameDirectory();
        static fs::path GetLogDirectory();

    private:
        static fs::path s_EngineRoot;
        static fs::path s_ProjectRoot;
        static fs::path s_AssetBasePath;

        // Resource type mapping
        static const std::unordered_map<Resource, fs::path>& GetResourceDirectories();
        static const std::unordered_map<Resource, std::string>& GetDefaultExtensions();

        // Internal initialization
        static void CreateDefaultDirectoryStructure();
    };
}
