#include "luthpch.h"
#include "ResourceManager.h"

#include <fstream>
#include <iostream>
#include <regex>

fs::path ResourceManager::s_EngineRoot;
fs::path ResourceManager::s_ProjectRoot;
fs::path ResourceManager::s_AssetBasePath;

void ResourceManager::Initialize(const fs::path& engineRoot)
{
    s_EngineRoot = engineRoot.empty() ? fs::current_path() : engineRoot;
    s_ProjectRoot = fs::current_path();
    s_AssetBasePath = s_ProjectRoot / "assets";

    CreateDefaultDirectoryStructure();
}

void ResourceManager::SetBasePath(const fs::path& basePath) {
    s_AssetBasePath = basePath.is_absolute() ? basePath : s_ProjectRoot / basePath;
}

const fs::path& ResourceManager::GetBasePath() {
    return s_AssetBasePath;
}

fs::path ResourceManager::GetPath(ResourceType type, const fs::path& resourceName)
{
    const auto& directories = GetResourceDirectories();
    const auto& extensions = GetDefaultExtensions();

    fs::path finalPath = s_AssetBasePath / directories.at(type) / resourceName;

    // Add default extension if missing
    if (!resourceName.has_extension() && extensions.count(type)) {
        finalPath += extensions.at(type);
    }

    return finalPath.lexically_normal();
}

fs::path ResourceManager::GetEnginePath(const fs::path& relativePath) {
    return (s_EngineRoot / relativePath).lexically_normal();
}

fs::path ResourceManager::GetProjectPath(const fs::path& relativePath) {
    return (s_ProjectRoot / relativePath).lexically_normal();
}

bool ResourceManager::FileExists(const fs::path& path) {
    return fs::exists(path) && fs::is_regular_file(path);
}

bool ResourceManager::ValidateResourcePath(const fs::path& path) {
    return FileExists(path) && (path.extension() != ".tmp");
}

size_t ResourceManager::GetFileSize(const fs::path& path) {
    return fs::exists(path) ? fs::file_size(path) : 0;
}

std::string ResourceManager::GetResourceTypeName(ResourceType type)
{
    static const std::unordered_map<ResourceType, std::string> typeNames = {
        { ResourceType::Model,      "Model"     },
        { ResourceType::Texture,    "Texture"   },
        { ResourceType::Material,   "Material"  },
        { ResourceType::Shader,     "Shader"    },
        { ResourceType::Font,       "Font"      },
        { ResourceType::Config,     "Configuration" }
    };
    return typeNames.at(type);
}

std::vector<fs::path> ResourceManager::FindResources(ResourceType type, const std::string& pattern)
{
    std::vector<fs::path> results;
    const fs::path searchDir = GetPath(type, "");
    const std::regex re(pattern);

    for (const auto& entry : fs::directory_iterator(searchDir)) {
        if (std::regex_match(entry.path().filename().string(), re)) {
            results.push_back(entry.path());
        }
    }

    return results;
}

fs::path ResourceManager::GetPlatformAssetPath()
{
#if defined(_WIN32)
    return GetPath(ResourceType::Config, "windows");
#elif defined(__APPLE__)
    return GetPath(ResourceType::Config, "macos");
#else
    return GetPath(ResourceType::Config, "linux");
#endif
}

fs::path ResourceManager::GetSaveGameDirectory()
{
#if defined(_WIN32)
    return GetProjectPath("SavedGames");
#else
    return GetProjectPath(".saves");
#endif
}

fs::path ResourceManager::GetLogDirectory() {
    return GetProjectPath("Logs");
}

// Internal implementation
const std::unordered_map<ResourceManager::ResourceType, fs::path>& ResourceManager::GetResourceDirectories()
{
    static const std::unordered_map<ResourceType, fs::path> directories = {
        { ResourceType::Model,      "models"    },
        { ResourceType::Texture,    "textures"  },
        { ResourceType::Material,   "materials" },
        { ResourceType::Shader,     "shaders"   },
        { ResourceType::Font,       "fonts"     },
        { ResourceType::Config,     "config"    }
    };
    return directories;
}

const std::unordered_map<ResourceManager::ResourceType, std::string>& ResourceManager::GetDefaultExtensions()
{
    static const std::unordered_map<ResourceType, std::string> extensions = {
        { ResourceType::Shader,     ".glsl" },
        { ResourceType::Texture,    ".png"  },
        { ResourceType::Model,      ".fbx"  },
        { ResourceType::Font,       ".ttf"  },
        { ResourceType::Config,     ".json" },
        { ResourceType::Material,   ".mat"  }
    };
    return extensions;
}

void ResourceManager::CreateDefaultDirectoryStructure()
{
    for (const auto& [type, path] : GetResourceDirectories()) {
        fs::create_directories(s_AssetBasePath / path);
    }
    fs::create_directories(GetSaveGameDirectory());
    fs::create_directories(GetLogDirectory());
}
