#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"
#include "luth/resources/AssetImporter.h"
#include "luth/core/JobSystem.h"

#include <unordered_map>
#include <queue>
#include <mutex>
#include <memory>
#include <filesystem>

namespace Luth
{
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
            if (it != s_Assets.end())
                return std::static_pointer_cast<T>(it->second);
            return nullptr;
        }

        // Triggers background loading via JobSystem
        static void LoadAsync(UUID handle);

        // Check status
        static bool IsLoaded(UUID handle);

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

        static std::unordered_map<UUID, std::shared_ptr<Asset>, UUIDHash> s_Assets;
        static std::unordered_map<AssetType, std::unique_ptr<AssetImporter>> s_Importers;
        
        static std::mutex s_AssetMutex;
        static std::mutex s_UploadMutex;
        static std::vector<PendingUpload> s_UploadQueue;
    };
}