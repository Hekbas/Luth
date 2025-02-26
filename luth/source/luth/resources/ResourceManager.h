#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

class ResourceManager
{
public:
    enum class ResourceType
    {
        Model,
        Texture,
        Material,
        Shader,
        Font,
        Config
    };

    // Initialize resource system
    static void Initialize(const fs::path& engineRoot = "");

    // Path management
    static void SetBasePath(const fs::path& basePath);
    static const fs::path& GetBasePath();

    // Resource path resolution
    static fs::path GetPath(ResourceType type, const fs::path& resourceName);
    static fs::path GetEnginePath(const fs::path& relativePath);
    static fs::path GetProjectPath(const fs::path& relativePath);

    // File utilities
    static bool FileExists(const fs::path& path);
    static bool ValidateResourcePath(const fs::path& path);
    static size_t GetFileSize(const fs::path& path);

    // Resource information
    static std::string GetResourceTypeName(ResourceType type);
    static std::vector<fs::path> FindResources(ResourceType type, const std::string& pattern = "*");

    // Platform utilities
    static fs::path GetPlatformAssetPath();
    static fs::path GetSaveGameDirectory();
    static fs::path GetLogDirectory();

private:
    static fs::path s_EngineRoot;
    static fs::path s_ProjectRoot;
    static fs::path s_AssetBasePath;

    // Resource type mapping
    static const std::unordered_map<ResourceType, fs::path>& GetResourceDirectories();
    static const std::unordered_map<ResourceType, std::string>& GetDefaultExtensions();

    // Internal initialization
    static void CreateDefaultDirectoryStructure();
};
