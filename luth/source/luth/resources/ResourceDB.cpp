#include "luthpch.h"
#include "luth/resources/ResourceDB.h"
#include "luth/resources/Resources.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"

#include <ranges>

namespace Luth
{
    std::unordered_map<UUID, ResourceDB::ResourceInfo, UUIDHash> ResourceDB::s_UuidToInfo;
    std::unordered_map<fs::path, UUID> ResourceDB::s_PathToUuid;

    void ResourceDB::Init(const fs::path& projectRoot)
    {
        LH_CORE_INFO("Initializing Resource DataBase...");
        s_UuidToInfo.clear();
        s_PathToUuid.clear();

        for (const auto& entry : fs::recursive_directory_iterator(projectRoot)) {
            fs::path path = entry.path();
            fs::path extension = path.extension();

            // Orphan .meta, could delete here
            if (extension == ".meta") {
                fs::path assetPath = path;
                if (!exists(assetPath.replace_extension(""))) {
                    fs::remove(path);
                    continue;
                }
            }

            // Handle .meta files
            if (extension != ".meta") {
                if (!ProcessMetaFile(path)) {
                    continue;
                }
            }

            // Load to Database
            switch (FileSystem::ClassifyFileType(path)) {
                case Luth::ResourceType::Model:    Resources::Load<Model>(path);    break;
                case Luth::ResourceType::Texture:  Resources::Load<Texture>(path);  break;
                case Luth::ResourceType::Material: Resources::Load<Material>(path); break;
                case Luth::ResourceType::Shader:   Resources::Load<Shader>(path);   break;
                default: break;
            }
        }
    }

    ResourceDB::ResourceInfo ResourceDB::UuidToInfo(const UUID& uuid)
    {
        auto it = s_UuidToInfo.find(uuid);
        return (it != s_UuidToInfo.end()) ? it->second : ResourceInfo();
    }

    UUID ResourceDB::PathToUuid(const fs::path& assetPath)
    {
        auto it = s_PathToUuid.find(assetPath);
        if (it != s_PathToUuid.end()) {
            return it->second;
        }

        // Check for meta file
        fs::path metaPath = assetPath;
        metaPath += ".meta";

        if (exists(metaPath)) {
            MetaFile meta(UUID(0)); 
            if (meta.Load(metaPath)) {
                RegisterAsset(assetPath, meta.GetUUID());
                return meta.GetUUID();
            }
        }

        // Generate new UUID if missing
        UUID newUuid;
        RegisterAsset(assetPath, newUuid);
        return newUuid;
    }

    void ResourceDB::RegisterAsset(const fs::path& path, const UUID& uuid)
    {
        s_UuidToInfo[uuid] = { path, FileSystem::ClassifyFileType(path), false };
        s_PathToUuid[path] = uuid;
    }

    void ResourceDB::UnregisterAsset(const fs::path& path)
    {
        if (auto it = s_PathToUuid.find(path); it != s_PathToUuid.end()) {
            s_UuidToInfo.erase(it->second);
            s_PathToUuid.erase(it);
        }
    }

    std::vector<UUID> ResourceDB::GetAllDependencies(const UUID& uuid)
    {
        std::vector<UUID> dependencies;
        fs::path assetPath = UuidToInfo(uuid).Path;

        if (!assetPath.empty()) {
            fs::path metaPath = assetPath;
            metaPath += ".meta";

            MetaFile meta(uuid);
            if (meta.Load(metaPath)) {
                dependencies = meta.GetDependencies();
            }
        }

        return dependencies;
    }

    void ResourceDB::SetDirty(UUID uuid)
    {
        auto it = s_UuidToInfo.find(uuid);
        if (it != s_UuidToInfo.end()) {
            it->second.Dirty = true;
        }
    }

    void ResourceDB::SaveDirty()
    {
        for (auto& [uuid, info] : s_UuidToInfo) {
            if (info.Dirty) {
                switch (info.Type) {
                    //case ResourceType::Model:    ModelLibrary::Save(uuid);    break;
                    case ResourceType::Material: MaterialLibrary::Save(uuid); break;
                    //case ResourceType::Texture:  SaveTexture(info.Path);  break;
                    default: LH_CORE_ERROR("Unable to save Unknown ResourceType"); break;
                }

                info.Dirty = false;
            }
        }
    }

    bool ResourceDB::ProcessMetaFile(const fs::path& path)
    {
        try {
            // Get corresponding meta path
            fs::path metaPath = path.string() + ".meta";

            // Resource without .meta
            if (!exists(metaPath)) {
                const auto type = FileSystem::ClassifyFileType(path);
                if (type == ResourceType::Unknown) return false;
                MetaFile::Create(path, type);
            }

            MetaFile meta(UUID(0));
            if (meta.Load(metaPath)) {
                RegisterAsset(metaPath, meta.GetUUID());
                return true;
            }
        }
        catch (...) {
            // Invalid meta file
            return false;
        }
    }
}
