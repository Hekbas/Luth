#include "luthpch.h"
#include "luth/resources/ResourceManager.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RendererAPI.h"
//#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"
//#include "luth/renderer/vulkan/VKMesh.h"

#include <regex>

namespace Luth
{
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

    fs::path ResourceManager::GetPath(Resource type, const fs::path& resourceName)
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

    std::string ResourceManager::GetResourceTypeName(Resource type)
    {
        static const std::unordered_map<Resource, std::string> typeNames = {
            { Resource::Model,      "Model"     },
            { Resource::Texture,    "Texture"   },
            { Resource::Material,   "Material"  },
            { Resource::Shader,     "Shader"    },
            { Resource::Font,       "Font"      },
            { Resource::Config,     "Configuration" }
        };
        return typeNames.at(type);
    }

    std::vector<fs::path> ResourceManager::FindResources(Resource type, const std::string& pattern)
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
        return GetPath(Resource::Config, "windows");
#elif defined(__APPLE__)
        return GetPath(Resource::Config, "macos");
#else
        return GetPath(Resource::Config, "linux");
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
    const std::unordered_map<Resource, fs::path>& ResourceManager::GetResourceDirectories()
    {
        static const std::unordered_map<Resource, fs::path> directories = {
            { Resource::Model,      "models"    },
            { Resource::Texture,    "textures"  },
            { Resource::Material,   "materials" },
            { Resource::Shader,     "shaders"   },
            { Resource::Font,       "fonts"     },
            { Resource::Config,     "config"    }
        };
        return directories;
    }

    const std::unordered_map<Resource, std::string>& ResourceManager::GetDefaultExtensions()
    {
        static const std::unordered_map<Resource, std::string> extensions = {
            { Resource::Shader,     ".glsl" },
            { Resource::Texture,    ".png"  },
            { Resource::Model,      ".fbx"  },
            { Resource::Font,       ".ttf"  },
            { Resource::Config,     ".json" },
            { Resource::Material,   ".mat"  }
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
}
