#include "luthpch.h"
#include "luth/resources/FileSystem.h"

#include <regex>

namespace Luth
{
    fs::path FileSystem::s_EngineRoot;
    fs::path FileSystem::s_EngineAssetsRoot;
    fs::path FileSystem::s_ProjectRoot;
    fs::path FileSystem::s_AssetsRoot;
    bool     FileSystem::s_HasProject = false;

    // ── Phase 1: Engine-only init (called at startup) ──

    void FileSystem::InitEngine(const fs::path& engineRoot)
    {
        s_EngineRoot       = engineRoot.empty() ? fs::current_path() : fs::absolute(engineRoot);
        s_EngineAssetsRoot = s_EngineRoot / "assets";

        // No project yet
        s_ProjectRoot.clear();
        s_AssetsRoot.clear();
        s_HasProject = false;

        LH_CORE_INFO("FileSystem: Engine root = {}", s_EngineRoot.string());
    }

    // ── Phase 2: Set project root (called when user selects a project) ──

    void FileSystem::SetProjectRoot(const fs::path& projectRoot)
    {
        s_ProjectRoot = fs::absolute(projectRoot);
        s_AssetsRoot  = s_ProjectRoot / "assets";
        s_HasProject  = true;

        LH_CORE_INFO("FileSystem: Project root = {}", s_ProjectRoot.string());
        LH_CORE_INFO("FileSystem: Assets root  = {}", s_AssetsRoot.string());

        EnsureBaseStructure();
    }

    void FileSystem::ClearProject()
    {
        s_ProjectRoot.clear();
        s_AssetsRoot.clear();
        s_HasProject = false;
    }

    bool FileSystem::HasProject()
    {
        return s_HasProject;
    }

    // ── Path Operations ──

    fs::path FileSystem::GetPath(AssetType type, const fs::path& name, bool addExtension)
    {
        const auto& info = GetTypeInfo().at(type);

        // For engine-internal asset types (fonts, shaders), check engine assets first
        fs::path enginePath = s_EngineAssetsRoot / info.directory / name;
        if (fs::exists(addExtension && !name.has_extension() && !info.extension.empty()
                ? fs::path(enginePath).replace_extension(info.extension)
                : enginePath))
        {
            fs::path path = enginePath;
            if (addExtension && !name.has_extension() && !info.extension.empty())
                path += info.extension;
            return path.lexically_normal();
        }

        // Fall back to project assets (if a project is loaded)
        if (s_HasProject)
        {
            fs::path path = s_AssetsRoot / info.directory / name;
            if (addExtension && !name.has_extension() && !info.extension.empty()) {
                path += info.extension;
            }
            return path.lexically_normal();
        }

        // No project loaded — return engine path as default
        fs::path path = enginePath;
        if (addExtension && !name.has_extension() && !info.extension.empty())
            path += info.extension;
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

    fs::path FileSystem::EngineAssetsPath(const fs::path& relative) {
        return (s_EngineAssetsRoot / relative).lexically_normal();
    }

    fs::path FileSystem::ResolveAsset(const fs::path& relative)
    {
        // Check project assets first (user can override engine defaults)
        if (s_HasProject)
        {
            fs::path projectPath = s_AssetsRoot / relative;
            if (fs::exists(projectPath))
                return projectPath.lexically_normal();
        }

        // Fall back to engine assets
        fs::path enginePath = s_EngineAssetsRoot / relative;
        if (fs::exists(enginePath))
            return enginePath.lexically_normal();

        // Return project path as default (even if it doesn't exist)
        if (s_HasProject)
            return (s_AssetsRoot / relative).lexically_normal();

        return enginePath.lexically_normal();
    }

    // ── Platform / Utility ──

    fs::path FileSystem::PlatformAssetsPath()
    {
        #if defined(_WIN32)
            const fs::path rel = fs::path("config") / "windows";
        #elif defined(__APPLE__)
            const fs::path rel = fs::path("config") / "macos";
        #else
            const fs::path rel = fs::path("config") / "linux";
        #endif

        // Project override first; engine fallback otherwise.
        if (s_HasProject)
        {
            fs::path projectPath = s_AssetsRoot / rel;
            if (fs::exists(projectPath))
                return projectPath.lexically_normal();
        }
        return (s_EngineAssetsRoot / rel).lexically_normal();
    }

    fs::path FileSystem::LogPath() {
        // Logs go alongside the project when one is loaded; otherwise next to
        // the engine so the launcher / pre-project boot still has somewhere to write.
        return s_HasProject ? ProjectPath("Logs") : EnginePath("Logs");
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
            { ".physmat", AssetType::PhysicsMaterial },
            // .glsl is reserved for #include headers (common/*.glsl) loaded by the shader compiler's
            // Includer at compile time, NOT standalone shader assets — kept out of the asset map so
            // the asset DB skips importing them.
            { ".vert",    AssetType::Shader   },
            { ".frag",    AssetType::Shader   },
            { ".comp",    AssetType::Shader   },
            { ".rgen",    AssetType::Shader   },
            { ".rmiss",   AssetType::Shader   },
            { ".rchit",   AssetType::Shader   },
            { ".rahit",   AssetType::Shader   },
            { ".rint",    AssetType::Shader   },
            { ".rcall",   AssetType::Shader   },
            // .slang carries its stage in a [shader("...")] attribute, not the extension; the importer
            // resolves it via reflection. Coexists with the GLSL stage extensions above.
            { ".slang",   AssetType::Shader   },
            { ".ttf",     AssetType::Font     },
            { ".luth",    AssetType::Scene    },
            { ".anim",    AssetType::Animation },
        };

        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return std::tolower(c); });

        auto it = extensionMap.find(ext);
        AssetType type = (it != extensionMap.end()) ? it->second : AssetType::None;

        // common/*.slang are import-only modules (like common/*.glsl includes) — compiling a no-entry
        // module standalone collides in Slang's cache; consumers resolve the import from disk. Skip it.
        if (type == AssetType::Shader && ext == ".slang" && path.parent_path().filename() == "common")
            return AssetType::None;

        return type;
    }

    void FileSystem::CreateDirectories(const fs::path& path) {
        fs::create_directories(path);
    }

    void FileSystem::EnsureBaseStructure()
    {
        if (!s_HasProject) return;

        for (const auto& [type, info] : GetTypeInfo()) {
            CreateDirectories(s_AssetsRoot / info.directory);
        }
        CreateDirectories(LogPath());
    }

    const std::unordered_map<AssetType, FileSystem::ResourceTypeInfo>& FileSystem::GetTypeInfo()
    {
        static const std::unordered_map<AssetType, ResourceTypeInfo> typeInfo = {
            { AssetType::Model,    { "Model",    "models",    ".fbx",  Vec4(0.4f, 0.8f, 1.0f, 1.0f) } },
            { AssetType::Texture,  { "Texture",  "textures",  ".png",  Vec4(0.8f, 0.6f, 0.2f, 1.0f) } },
            { AssetType::Material,        { "Material",        "materials",        ".mat",     Vec4(0.2f, 0.9f, 0.4f, 1.0f) } },
            { AssetType::PhysicsMaterial, { "Physics Material", "physics_materials", ".physmat", Vec4(0.95f, 0.55f, 0.2f, 1.0f) } },
            { AssetType::Shader,          { "Shader",          "shaders",          ".glsl",    Vec4(0.9f, 0.3f, 0.3f, 1.0f) } },
            { AssetType::Font,      { "Font",      "fonts",      ".ttf",  Vec4(0.5f, 0.5f, 0.5f, 1.0f) } },
            { AssetType::Scene,     { "Scene",     "scenes",     ".luth", Vec4(0.6f, 0.4f, 0.9f, 1.0f) } },
            { AssetType::Animation, { "Animation", "animations", ".anim", Vec4(0.3f, 0.8f, 0.7f, 1.0f) } },
        };
        return typeInfo;
    }
}
