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

    std::unique_ptr<FileWatcher> ResourceDB::s_FileWatcher;
    std::vector<std::pair<fs::path, FileWatcher::FileStatus>> ResourceDB::s_ReloadQueue;
    std::mutex ResourceDB::s_QueueMutex;

    void ResourceDB::Init(const fs::path& projectRoot)
    {
        LH_CORE_INFO("Initializing Resource DataBase...");
        s_UuidToInfo.clear();
        s_PathToUuid.clear();

        // First pass: clean up orphaned .meta files
        CleanOrphanedMetaFiles(projectRoot);

        // Second pass: process actual assets
        for (const auto& entry : fs::recursive_directory_iterator(projectRoot)) {
            const fs::path& path = entry.path();

            // Skip .meta files
            if (path.extension() == ".meta") {
                continue;
            }

            if (!ProcessMetaFile(path)) {
                continue;
            }

            // Load to Database
            switch (FileSystem::ClassifyFileType(path)) {
                case ResourceType::Model:    Resources::Load<Model>(path);    break;
                case ResourceType::Texture:  Resources::Load<Texture>(path);  break;
                case ResourceType::Material: Resources::Load<Material>(path); break;
                case ResourceType::Shader:   Resources::Load<Shader>(path);   break;
                default: break;
            }
        }

        // Initialize file watcher
        s_FileWatcher = std::make_unique<FileWatcher>(1.0f);
        s_FileWatcher->SetCallback(&ResourceDB::OnFileChanged);

        // Watch all asset directories
        s_FileWatcher->AddWatch(FileSystem::AssetsPath());

        s_FileWatcher->Start(true);
    }

    void ResourceDB::Shutdown()
    {
        if (s_FileWatcher) {
            s_FileWatcher->Stop();
            s_FileWatcher.reset();
        }
    }

    void ResourceDB::Update()
    {
        ProcessReloadQueue();
    }

    const ResourceDB::ResourceInfo& ResourceDB::UuidToInfo(const UUID& uuid)
    {
        static ResourceInfo emptyInfo;
        auto it = s_UuidToInfo.find(uuid);
        return (it != s_UuidToInfo.end()) ? it->second : emptyInfo;
    }

    UUID ResourceDB::PathToUuid(const fs::path& assetPath)
    {
        // Check if path is already registered
        if (auto it = s_PathToUuid.find(assetPath); it != s_PathToUuid.end()) {
            return it->second;
        }

        // Check for meta file
        fs::path metaPath = assetPath.string() + ".meta";

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
        ResourceType type = FileSystem::ClassifyFileType(path);
        if (type == ResourceType::Unknown) {
            LH_CORE_WARN("Attempting to register unknown resource type: {0}", path.string());
            return;
        }

        s_UuidToInfo[uuid] = { path, type, false };
        s_PathToUuid[path] = uuid;
    }

    void ResourceDB::UnregisterAsset(const fs::path& path)
    {
        if (auto it = s_PathToUuid.find(path); it != s_PathToUuid.end()) {
            s_UuidToInfo.erase(it->second);
            s_PathToUuid.erase(it);
            LH_CORE_TRACE("Unregistered asset: {0}", path.string());
        }
    }

    void ResourceDB::UpdateAssetPath(const fs::path& oldPath, const fs::path& newPath)
    {
        // Check if the old path exists in our database
        auto pathIt = s_PathToUuid.find(oldPath);
        if (pathIt == s_PathToUuid.end()) {
            LH_CORE_WARN("Attempted to update non-registered path: {0}", oldPath.string());
            return;
        }

        // Check if the new path is already registered
        if (s_PathToUuid.find(newPath) != s_PathToUuid.end()) {
            LH_CORE_ERROR("New path already exists in resource database: {0}", newPath.string());
            return;
        }

        // Get the UUID for the old path
        const UUID uuid = pathIt->second;

        // Update the path in both maps
        s_PathToUuid.erase(oldPath);
        s_PathToUuid[newPath] = uuid;
		s_UuidToInfo[uuid].Path = newPath;

        LH_CORE_INFO("Updated resource path: {0} -> {1}", oldPath.string(), newPath.string());
    }

    std::vector<UUID> ResourceDB::GetAllDependencies(const UUID& uuid)
    {
        const auto& info = UuidToInfo(uuid);
        if (info.Path.empty()) {
            return {};
        }

        fs::path metaPath = info.Path.string() + ".meta";
        MetaFile meta(uuid);

        if (meta.Load(metaPath)) {
            return meta.GetDependencies();
        }

        return {};
    }

    void ResourceDB::SetDirty(UUID uuid)
    {
        if (auto it = s_UuidToInfo.find(uuid); it != s_UuidToInfo.end()) {
            it->second.Dirty = true;
        }
    }

    void ResourceDB::SaveDirty()
    {
        for (auto& [uuid, info] : s_UuidToInfo) {
            if (!info.Dirty) continue;

            switch (info.Type) {
                case ResourceType::Model:    ModelLibrary::Save(uuid);    break;
                case ResourceType::Material: MaterialLibrary::Save(uuid); break;
                default:
                    LH_CORE_ERROR("Unable to save resource of type {0}", static_cast<int>(info.Type));
                    break;
            }

            info.Dirty = false;
        }
    }

    bool ResourceDB::ProcessMetaFile(const fs::path& path)
    {
        // Skip meta files themselves
        if (path.extension() == ".meta") {
            return false;
        }

        // Get corresponding meta path
        fs::path metaPath = path.string() + ".meta";

        // Resource without .meta - create one if it's a known type
        if (!exists(metaPath)) {
            const auto type = FileSystem::ClassifyFileType(path);
            if (type == ResourceType::Unknown) {
                return false;
            }
            return MetaFile::Create(path, type);
        }

        // Load existing meta file
        MetaFile meta(UUID(0));
        if (meta.Load(metaPath)) {
            RegisterAsset(path, meta.GetUUID());
            return true;
        }

        return false;
    }

    bool ResourceDB::IsAssetPath(const fs::path& path)
    {
        return path.extension() != ".meta" &&
            FileSystem::ClassifyFileType(path) != ResourceType::Unknown;
    }

    void ResourceDB::CleanOrphanedMetaFiles(const fs::path& projectRoot)
    {
        for (const auto& entry : fs::recursive_directory_iterator(projectRoot)) {
            const fs::path& path = entry.path();

            if (path.extension() == ".meta") {
                fs::path assetPath = path;
                assetPath.replace_extension("");

                if (!exists(assetPath)) {
                    fs::remove(path);
                    LH_CORE_TRACE("Removed orphaned meta file: {0}", path.string());
                }
            }
        }
    }

    void ResourceDB::OnFileChanged(const fs::path& path, FileWatcher::FileStatus status)
    {
        // Skip directories and non-asset files
        if (fs::is_directory(path) || !IsAssetPath(path)) {
            return;
        }

        // Skip meta files but process their associated assets
        fs::path processedPath = path;
        if (path.extension() == ".meta") {
            processedPath.replace_extension("");
            if (!exists(processedPath)) return;
        }

        // Queue for main thread processing
        {
            std::lock_guard lock(s_QueueMutex);
            s_ReloadQueue.emplace_back(processedPath, status);
        }
    }

    void ResourceDB::ProcessReloadQueue()
    {
        std::vector<std::pair<fs::path, FileWatcher::FileStatus>> queue;
        {
            std::lock_guard lock(s_QueueMutex);
            queue.swap(s_ReloadQueue);
        }

        for (auto& [path, status] : queue) {
            switch (status) {
                case FileWatcher::FileStatus::Modified: HandleFileModified(path); break;
                case FileWatcher::FileStatus::Created:  HandleFileCreated(path);  break;
                case FileWatcher::FileStatus::Deleted:  HandleFileDeleted(path);  break;
            }
        }
    }

    void ResourceDB::HandleFileModified(const fs::path& path)
    {
        ResourceType type = FileSystem::ClassifyFileType(path);
        LH_CORE_INFO("Hot Reloading: {0}", path.string());

        UUID uuid = PathToUuid(path);

        switch (type) {
            case ResourceType::Shader:   ShaderLibrary::Reload(uuid);   break;
            case ResourceType::Model:    ModelLibrary::Reload(uuid);    break;
            case ResourceType::Material: MaterialLibrary::Reload(uuid); break;
            case ResourceType::Texture:  TextureCache::Reload(uuid);    break;
            default: break;
        }
    }

    void ResourceDB::HandleFileCreated(const fs::path& path)
    {
        if (ProcessMetaFile(path)) {
            ResourceType type = FileSystem::ClassifyFileType(path);
            LH_CORE_INFO("Loading new asset: {0}", path.string());

            switch (type) {
                case ResourceType::Model:    Resources::Load<Model>(path);    break;
                case ResourceType::Texture:  Resources::Load<Texture>(path);  break;
                case ResourceType::Material: Resources::Load<Material>(path); break;
                case ResourceType::Shader:   Resources::Load<Shader>(path);   break;
                default: break;
            }
        }
    }

    void ResourceDB::HandleFileDeleted(const fs::path& path)
    {
        UUID uuid = PathToUuid(path);
        UnregisterAsset(path);
        LH_CORE_WARN("Asset deleted: {0}", path.string());

        ResourceType type = FileSystem::ClassifyFileType(path);
        switch (type) {
            case ResourceType::Model:    ModelLibrary::Remove(uuid);    break;
            case ResourceType::Texture:  TextureCache::Remove(uuid);    break;
            case ResourceType::Material: MaterialLibrary::Remove(uuid); break;
            case ResourceType::Shader:   ShaderLibrary::Remove(uuid);   break;
            default: break;
        }
    }
}