#include "luthpch.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/ModelLibrary.h"
#include "luth/resources/MaterialLibrary.h"
#include "luth/resources/ShaderLibrary.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RendererAPI.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"
//#include "luth/renderer/vulkan/VKMesh.h"

#include <regex>

namespace Luth
{
    fs::path FileSystem::s_EngineRoot;
    fs::path FileSystem::s_ProjectRoot;
    fs::path FileSystem::s_AssetsRoot;

    void FileSystem::Init(const fs::path& engineRoot)
    {
        s_EngineRoot = engineRoot.empty() ? fs::current_path() : engineRoot;
        s_ProjectRoot = fs::current_path();
        s_AssetsRoot = s_ProjectRoot / "assets";
        EnsureBaseStructure();
    }

    fs::path FileSystem::GetPath(Resource type, const fs::path& name, bool addExtension)
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
            return GetPath(Resource::Config, "windows");
        #elif defined(__APPLE__)
            return GetPath(Resource::Config, "macos");
        #else
            return GetPath(Resource::Config, "linux");
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

    const std::unordered_map<Resource, FileSystem::ResourceTypeInfo>& FileSystem::GetTypeInfo()
    {
        static const std::unordered_map<Resource, ResourceTypeInfo> typeInfo = {
            {Resource::Model,    {"models",    ".fbx"}},
            {Resource::Texture,  {"textures",  ".png"}},
            {Resource::Material, {"materials", ".mat"}},
            {Resource::Shader,   {"shaders",   ".glsl"}},
            {Resource::Font,     {"fonts",     ".ttf"}},
            {Resource::Config,   {"config",    ".json"}}
        };
        return typeInfo;
    }
}
