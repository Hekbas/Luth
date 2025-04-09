#include "luthpch.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/resources/libraries/ShaderLibrary.h"

#include <regex>

namespace Luth
{
    fs::path FileSystem::s_EngineRoot;
    fs::path FileSystem::s_ProjectRoot;
    fs::path FileSystem::s_AssetsRoot;

    void FileSystem::Init(const fs::path& engineRoot)
    {
        LH_CORE_INFO("Initialized FileSystem");
        s_EngineRoot = engineRoot.empty() ? fs::current_path() : engineRoot;
        s_ProjectRoot = fs::current_path();
        s_AssetsRoot = s_ProjectRoot / "assets";
        EnsureBaseStructure();
    }

    fs::path FileSystem::GetPath(ResourceType type, const fs::path& name, bool addExtension)
    {
        const auto& info = GetTypeInfo().at(type);
        fs::path path = s_ProjectRoot / "assets" / info.directory / name;

        if (addExtension && !name.has_extension() && !info.extension.empty()) {
            path += info.extension;
        }

        return path.lexically_normal();
    }

    fs::path FileSystem::EnginePath(const fs::path& relative) {
        return (s_EngineRoot / relative).lexically_normal();
    }

    fs::path FileSystem::ProjectPath(const fs::path& relative) {
        return (s_ProjectRoot / relative).lexically_normal();
    }

    fs::path FileSystem::AssetsPath(const fs::path& relative) {
        return (s_AssetsRoot / relative).lexically_normal();
    }

    // Platform-specific implementations
    fs::path FileSystem::PlatformAssetsPath()
    {
        #if defined(_WIN32)
            return GetPath(ResourceType::Config, "windows");
        #elif defined(__APPLE__)
            return GetPath(ResourceType::Config, "macos");
        #else
            return GetPath(ResourceType::Config, "linux");
        #endif
    }

    fs::path FileSystem::SaveGamePath()
    {
        #if defined(_WIN32)
            return ProjectPath("SavedGames");
        #else
            return ProjectPath(".saves");
        #endif
    }

    fs::path FileSystem::LogPath() {
        return ProjectPath("Logs");
    }

    bool FileSystem::Exists(const fs::path& path) {
        return fs::exists(path);
    }

    size_t FileSystem::FileSize(const fs::path& path) {
        return fs::exists(path) ? fs::file_size(path) : 0;
    }

    bool FileSystem::Validate(const fs::path& path) {
        return Exists(path) && path.extension() != ".tmp";
    }

    ResourceType FileSystem::ClassifyFileType(const fs::path& path)
    {
        static const std::unordered_map<std::string, ResourceType> extensionMap = {
            { ".fbx",   ResourceType::Model    },
            { ".obj",   ResourceType::Model    },
            { ".gltf",  ResourceType::Model    },
            { ".dae",   ResourceType::Model    },
            { ".blend", ResourceType::Model    },
            { ".png",   ResourceType::Texture  },
            { ".jpg",   ResourceType::Texture  },
            { ".tga",   ResourceType::Texture  },
            { ".mat",   ResourceType::Material },
            { ".glsl",  ResourceType::Shader   },
            { ".ttf",   ResourceType::Font     },
            { ".ini",   ResourceType::Config   }
        };

        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return std::tolower(c); });

        auto it = extensionMap.find(ext);
        return (it != extensionMap.end()) ? it->second : ResourceType::Unknown;
    }

    void FileSystem::CreateDirectories(const fs::path& path) {
        fs::create_directories(path);
    }

    void FileSystem::EnsureBaseStructure()
    {
        for (const auto& [type, info] : GetTypeInfo()) {
            CreateDirectories(s_ProjectRoot / "assets" / info.directory);
        }
        CreateDirectories(SaveGamePath());
        CreateDirectories(LogPath());
    }

    const std::unordered_map<ResourceType, FileSystem::ResourceTypeInfo>& FileSystem::GetTypeInfo()
    {
        static const std::unordered_map<ResourceType, ResourceTypeInfo> typeInfo = {
            { ResourceType::Model,    { "Model",    "models",    ".fbx",  Vec4(0.4f, 0.8f, 1.0f, 1.0f) } },
            { ResourceType::Texture,  { "Texture",  "textures",  ".png",  Vec4(0.8f, 0.6f, 0.2f, 1.0f) } },
            { ResourceType::Material, { "Material", "materials", ".mat",  Vec4(0.2f, 0.9f, 0.4f, 1.0f) } },
            { ResourceType::Shader,   { "Shader",   "shaders",   ".glsl", Vec4(0.9f, 0.3f, 0.3f, 1.0f) } },
            { ResourceType::Font,     { "Font",     "fonts",     ".ttf",  Vec4(0.5f, 0.5f, 0.5f, 1.0f) } },
            { ResourceType::Config,   { "Config",   "config",    ".json", Vec4(0.5f, 0.5f, 0.5f, 1.0f) } }
        };
        return typeInfo;
    }
}
