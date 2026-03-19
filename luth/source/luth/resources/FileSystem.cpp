#include "luthpch.h"
#include "luth/resources/FileSystem.h"

#include <regex>

namespace Luth
{
    fs::path FileSystem::s_EngineRoot;
    fs::path FileSystem::s_ProjectRoot;
    fs::path FileSystem::s_AssetsRoot;

    void FileSystem::Init(const fs::path& engineRoot)
    {
        LH_CORE_INFO("Initialized FileSystem ({0})", fs::current_path());
        s_EngineRoot = engineRoot.empty() ? fs::current_path() : engineRoot;
        s_ProjectRoot = fs::current_path();
        s_AssetsRoot = s_ProjectRoot / "assets";
        EnsureBaseStructure();
    }

    fs::path FileSystem::GetPath(AssetType type, const fs::path& name, bool addExtension)
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
             return s_AssetsRoot / "config" / "windows";
        #elif defined(__APPLE__)
             return s_AssetsRoot / "config" / "macos";
        #else
             return s_AssetsRoot / "config" / "linux";
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

    AssetType FileSystem::ClassifyFileType(const fs::path& path)
    {
        static const std::unordered_map<std::string, AssetType> extensionMap = {
            { ".fbx",     AssetType::Model    },
            { ".obj",     AssetType::Model    },
            { ".gltf",    AssetType::Model    },
            { ".glb",     AssetType::Model    },
            { ".dae",     AssetType::Model    },
            { ".blend",   AssetType::Model    },
            { ".md5mesh", AssetType::Model    },
            { ".png",     AssetType::Texture  },
            { ".jpg",     AssetType::Texture  },
            { ".tga",     AssetType::Texture  },
            { ".mat",     AssetType::Material },
            { ".glsl",    AssetType::Shader   },
            { ".vert",    AssetType::Shader   },
            { ".ttf",     AssetType::Font     },
            // { ".ini",     AssetType::Config   }
        };

        // if (fs::is_directory(path)) {
        //     return AssetType::Directory;
        // }

        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return std::tolower(c); });

        auto it = extensionMap.find(ext);
        return (it != extensionMap.end()) ? it->second : AssetType::None;
    }

    void FileSystem::CreateDirectories(const fs::path& path) {
        fs::create_directories(path);
    }

    void FileSystem::EnsureBaseStructure()
    {
        for (const auto& [type, info] : GetTypeInfo()) {
            CreateDirectories(s_ProjectRoot / "assets" / info.directory);
        }
        CreateDirectories(LogPath());
    }

    const std::unordered_map<AssetType, FileSystem::ResourceTypeInfo>& FileSystem::GetTypeInfo()
    {
        static const std::unordered_map<AssetType, ResourceTypeInfo> typeInfo = {
            { AssetType::Model,    { "Model",    "models",    ".fbx",  Vec4(0.4f, 0.8f, 1.0f, 1.0f) } },
            { AssetType::Texture,  { "Texture",  "textures",  ".png",  Vec4(0.8f, 0.6f, 0.2f, 1.0f) } },
            { AssetType::Material, { "Material", "materials", ".mat",  Vec4(0.2f, 0.9f, 0.4f, 1.0f) } },
            { AssetType::Shader,   { "Shader",   "shaders",   ".glsl", Vec4(0.9f, 0.3f, 0.3f, 1.0f) } },
            { AssetType::Font,     { "Font",     "fonts",     ".ttf",  Vec4(0.5f, 0.5f, 0.5f, 1.0f) } },
            // { AssetType::Config,   { "Config",   "config",    ".json", Vec4(0.5f, 0.5f, 0.5f, 1.0f) } }
        };
        return typeInfo;
    }
}
