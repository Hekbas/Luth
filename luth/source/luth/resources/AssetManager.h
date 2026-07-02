#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"
#include "luth/resources/AssetImporter.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/time/Time.h"

#include <unordered_map>
#include <queue>
#include <mutex>
#include <unordered_set>
#include <memory>
#include <filesystem>

namespace Luth
{
    // UUID-keyed cache of loaded asset shared_ptrs. Async loads dispatch onto worker fibers; the
    // resulting GPU uploads land in a queue that the main thread drains each frame, where Vulkan
    // handles and bindless-slot binds actually happen. Trim() evicts unreferenced assets, but
    // Scene::HoldAsset keeps live entries pinned across the cycle.
    class AssetManager
    {
    public:
        static void Init();
        static void Shutdown();
        
        // Main Thread: Process upload queue (CPU -> GPU)
        static void Update(); 

        // Thread-safe retrieval. Returns nullptr or placeholder if not loaded.
        template<typename T>
        static std::shared_ptr<T> GetAsset(UUID handle)
        {
            std::lock_guard<std::mutex> lock(s_AssetMutex);
            auto it = s_Assets.find(handle);
            if (it != s_Assets.end()) {
                it->second->LastAccessedTime = Time::GetTime();
                return std::static_pointer_cast<T>(it->second);
            }
            return nullptr;
        }

        // Triggers background loading via JobSystem
        static void LoadAsync(UUID handle);

        // Blocking load (Main Thread). Used by Editor for immediate instantiation.
        static std::shared_ptr<Asset> LoadImmediate(UUID handle);

        // Forces regeneration of the artifact from source (Blocking)
        static void Import(UUID handle);

        // Check status
        static bool IsLoaded(UUID handle);
        static bool IsLoading(UUID handle);

        // Returns true if an importer is registered for this asset type
        static bool HasImporter(AssetType type);

        // Import all dirty assets from AssetDatabase using the job system (blocking).
        static void ImportDirty();

        // Removes a specific asset from the cache (forces reload on next access)
        static void Evict(UUID handle);

        // Unloads assets only referenced by the AssetManager. Returns the count evicted; force=true skips the 5s timeout.
        static u32 Trim(bool force = false);

    private:
        struct LoadRequest {
            UUID Handle;
            std::filesystem::path Path;
            AssetType Type;
        };

        struct PendingUpload {
            UUID Handle;
            std::unique_ptr<AssetData> Data;
            AssetType Type;
        };

        static void LoadJob(JobSystem::JobArgs args);

        // Shared helpers: DeserializeArtifact is thread-safe; FinalizeAsset must run on the main thread
        // (creates GPU resources).
        static std::unique_ptr<AssetData> DeserializeArtifact(AssetType type, const std::filesystem::path& artifactPath);
        static std::shared_ptr<Asset> FinalizeAsset(AssetType type, AssetData* data, const std::filesystem::path& sourcePath);

        static std::unordered_map<UUID, std::shared_ptr<Asset>, UUIDHash> s_Assets;
        static std::unordered_set<UUID, UUIDHash> s_LoadingAssets;
        static std::unordered_map<AssetType, std::unique_ptr<AssetImporter>> s_Importers;
        
        static std::mutex s_AssetMutex;
        static std::mutex s_UploadMutex;
        static std::vector<PendingUpload> s_UploadQueue;
    };
}