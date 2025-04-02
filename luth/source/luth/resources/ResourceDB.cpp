#include "luthpch.h"
#include "luth/resources/ResourceDB.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"

#include <ranges>

namespace Luth
{
    std::unordered_map<UUID, fs::path, UUIDHash> ResourceDB::s_UuidToPath;
    std::unordered_map<fs::path, UUID> ResourceDB::s_PathToUuid;

    void ResourceDB::Init(const fs::path& projectRoot)
    {
        s_UuidToPath.clear();
        s_PathToUuid.clear();

        for (const auto& entry : fs::recursive_directory_iterator(projectRoot)) {
            auto path = entry.path();
            if (path.extension() == ".meta") {
                if (exists(path)) {
                    ProcessMetaFile(path);
                }
                else {
                    // Orphan .meta, delete?
                }
            }
            else {
                auto type = FileSystem::ClassifyFileType(path);
                if (type == ResourceType::Unknown) continue;

                MetaFile::Create(path, type);
                ProcessMetaFile(path);
            }
        }
    }

    fs::path ResourceDB::ResolveUuid(const UUID& uuid)
    {
        auto it = s_UuidToPath.find(uuid);
        return (it != s_UuidToPath.end()) ? it->second : fs::path();
    }

    UUID ResourceDB::GetUuidForPath(const fs::path& assetPath)
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
        s_UuidToPath[uuid] = path;
        s_PathToUuid[path] = uuid;
    }

    void ResourceDB::UnregisterAsset(const fs::path& path)
    {
        if (auto it = s_PathToUuid.find(path); it != s_PathToUuid.end()) {
            s_UuidToPath.erase(it->second);
            s_PathToUuid.erase(it);
        }
    }

    std::vector<UUID> ResourceDB::GetAllDependencies(const UUID& uuid)
    {
        std::vector<UUID> dependencies;
        fs::path assetPath = ResolveUuid(uuid);

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

    void ResourceDB::ProcessMetaFile(const fs::path& metaPath)
    {
        try {
            // Get corresponding asset path
            fs::path assetPath = metaPath;
            assetPath.replace_extension("");

            if (!exists(assetPath)) {
                // Orphaned meta file
                return;
            }

            MetaFile meta(UUID(0));
            if (meta.Load(metaPath)) {
                RegisterAsset(assetPath, meta.GetUUID());
            }
        }
        catch (...) {
            // Invalid meta file
        }
    }
}
