#include "luthpch.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/importers/TextureImporter.h"
#include "luth/resources/importers/ModelImporter.h"
#include "luth/resources/importers/MaterialImporter.h"
#include "luth/renderer/Texture.h"
#include "luth/renderer/Model.h"
#include "luth/renderer/Material.h"
#include "luth/core/Time.h"

namespace Luth
{
    std::unordered_map<UUID, std::shared_ptr<Asset>, UUIDHash> AssetManager::s_Assets;
    std::unordered_set<UUID, UUIDHash> AssetManager::s_LoadingAssets;
    std::unordered_map<AssetType, std::unique_ptr<AssetImporter>> AssetManager::s_Importers;
    std::mutex AssetManager::s_AssetMutex;
    std::mutex AssetManager::s_UploadMutex;
    std::vector<AssetManager::PendingUpload> AssetManager::s_UploadQueue;

    static float s_GCTimer = 0.0f;
    static const float k_GCInterval = 2.0f; // Run GC every 2 seconds

    void AssetManager::Init()
    {
        s_Importers[AssetType::Texture] = std::make_unique<TextureImporter>();
        s_Importers[AssetType::Model] = std::make_unique<ModelImporter>();
        s_Importers[AssetType::Material] = std::make_unique<MaterialImporter>();
    }

    void AssetManager::Shutdown()
    {
        s_Assets.clear();
        s_LoadingAssets.clear();
        s_Importers.clear();
        s_UploadQueue.clear();
    }

    void AssetManager::LoadAsync(UUID handle)
    {
        std::lock_guard<std::mutex> lock(s_AssetMutex);
        
        // Check if already loaded or currently loading
        if (s_Assets.find(handle) != s_Assets.end()) return;
        if (s_LoadingAssets.find(handle) != s_LoadingAssets.end()) return;

        const auto& info = AssetDatabase::GetMetadata(handle);
        if (info.Path.empty())
        {
            LH_CORE_ERROR("AssetManager: UUID {0} not found in DB", handle.ToString());
            return;
        }

        // Mark as loading to prevent duplicate requests
        s_LoadingAssets.insert(handle);
        
        LoadRequest* req = new LoadRequest{ handle, info.Path, info.Type };
        
        // Dispatch to JobSystem
        JobSystem::Execute(LoadJob, req);
    }

    std::shared_ptr<Asset> AssetManager::LoadImmediate(UUID handle)
    {
        // Check cache first
        if (auto asset = GetAsset<Asset>(handle)) return asset;

        const auto& info = AssetDatabase::GetMetadata(handle);
        if (info.Path.empty()) return nullptr;

        // 1. Check/Create Artifact
        fs::path artifactPath = AssetDatabase::GetArtifactPath(handle);
        bool artifactReady = fs::exists(artifactPath);

        if (!artifactReady)
        {
            if (s_Importers.find(info.Type) != s_Importers.end())
            {
                artifactReady = s_Importers[info.Type]->Import(info.Path, artifactPath);
            }
        }

        if (!artifactReady) return nullptr;

        // 2. Load Data from Artifact
        std::unique_ptr<AssetData> data = nullptr;
        if (info.Type == AssetType::Texture) {
            auto texData = std::make_unique<TextureAssetData>();
            if (AssetSerializer::DeserializeTexture(artifactPath, *texData)) data = std::move(texData);
        }
        else if (info.Type == AssetType::Model) {
            auto modelData = std::make_unique<ModelAssetData>();
            if (AssetSerializer::DeserializeModel(artifactPath, *modelData)) data = std::move(modelData);
        }
        else if (info.Type == AssetType::Material)
        {
            auto matData = std::make_unique<MaterialAssetData>();
            if (AssetSerializer::DeserializeMaterial(artifactPath, *matData)) data = std::move(matData);
        }

        if (!data) return nullptr;

        // Create Asset (Main Thread)
        std::shared_ptr<Asset> newAsset = nullptr;
        if (info.Type == AssetType::Texture)
        {
            auto* texData = static_cast<TextureAssetData*>(data.get());
            newAsset = Texture::Create(texData->Width, texData->Height, texData->Format, texData->Pixels.data());
        }
        else if (info.Type == AssetType::Model)
        {
            auto* modelData = static_cast<ModelAssetData*>(data.get());
            newAsset = Model::Create(modelData->Meshes, modelData->Materials);
        }
        else if (info.Type == AssetType::Material)
        {
            auto* matData = static_cast<MaterialAssetData*>(data.get());
            auto material = std::make_shared<Material>();
            material->Deserialize(matData->JsonData);
            newAsset = material;
        }

        if (newAsset) { 
            newAsset->Handle = handle; 
            newAsset->LastAccessedTime = Time::GetTime();
            s_Assets[handle] = newAsset; 
        }
        return newAsset;
    }

    void AssetManager::Import(UUID handle)
    {
        const auto& info = AssetDatabase::GetMetadata(handle);
        if (info.Path.empty()) return;

        fs::path artifactPath = AssetDatabase::GetArtifactPath(handle);
        
        if (s_Importers.find(info.Type) != s_Importers.end())
        {
            LH_CORE_INFO("Importing Asset: {0}", info.Path.string());
            if (!s_Importers[info.Type]->Import(info.Path, artifactPath))
            {
                LH_CORE_ERROR("Failed to import asset: {0}", info.Path.string());
            }
        }
    }

    bool AssetManager::IsLoaded(UUID handle)
    {
        std::lock_guard<std::mutex> lock(s_AssetMutex);
        return s_Assets.find(handle) != s_Assets.end();
    }

    bool AssetManager::IsLoading(UUID handle)
    {
        std::lock_guard<std::mutex> lock(s_AssetMutex);
        return s_LoadingAssets.find(handle) != s_LoadingAssets.end();
    }

    void AssetManager::Trim()
    {
        std::lock_guard<std::mutex> lock(s_AssetMutex);
        f32 currentTime = Time::GetTime();
        const f32 timeout = 5.0f; // Keep unused assets for 5 seconds

        for (auto it = s_Assets.begin(); it != s_Assets.end(); )
        {
            // If use_count is 1, it means only s_Assets holds a reference
            if (it->second.use_count() == 1 && (currentTime - it->second->LastAccessedTime > timeout))
            {
                it = s_Assets.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void AssetManager::LoadJob(JobSystem::JobArgs args)
    {
        LH_PROFILE_FUNCTION();

        LoadRequest* req = (LoadRequest*)args.data;
        LH_PROFILE_TAG("Asset", req->Path.string().c_str());
        
        fs::path artifactPath = AssetDatabase::GetArtifactPath(req->Handle);
        bool artifactReady = fs::exists(artifactPath);

        // 1. Import if missing
        if (!artifactReady)
        {
            if (s_Importers.find(req->Type) != s_Importers.end())
            {
                auto& importer = s_Importers[req->Type];
                artifactReady = importer->Import(req->Path, artifactPath);
            }
            else
            {
                LH_CORE_ERROR("AssetManager: No importer for type {0}", (int)req->Type);
            }
        }

        // 2. Load from Artifact
        std::unique_ptr<AssetData> data = nullptr;
        if (artifactReady)
        {
            if (req->Type == AssetType::Texture) {
                auto texData = std::make_unique<TextureAssetData>();
                if (AssetSerializer::DeserializeTexture(artifactPath, *texData)) data = std::move(texData);
            }
            else if (req->Type == AssetType::Model) {
                auto modelData = std::make_unique<ModelAssetData>();
                if (AssetSerializer::DeserializeModel(artifactPath, *modelData)) data = std::move(modelData);
            }
            else if (req->Type == AssetType::Material) {
                auto matData = std::make_unique<MaterialAssetData>();
                if (AssetSerializer::DeserializeMaterial(artifactPath, *matData)) data = std::move(matData);
            }
        }

        // Push to upload queue regardless of success to clear the loading flag on main thread
        {
            std::lock_guard<std::mutex> lock(s_UploadMutex);
            s_UploadQueue.push_back({ req->Handle, std::move(data), req->Type });
        }

        delete req;
    }

    void AssetManager::Update()
    {
        LH_PROFILE_FUNCTION();

        // Automatic Garbage Collection
        s_GCTimer += Time::UnscaledDeltaTime();
        if (s_GCTimer >= k_GCInterval)
        {
            Trim();
            s_GCTimer = 0.0f;
        }

        std::lock_guard<std::mutex> lock(s_UploadMutex);
        if (s_UploadQueue.empty()) return;

        for (auto& upload : s_UploadQueue)
        {
            std::shared_ptr<Asset> newAsset = nullptr;

            if (upload.Data)
            {
                if (upload.Type == AssetType::Texture)
                {
                    auto* texData = static_cast<TextureAssetData*>(upload.Data.get());
                    newAsset = Texture::Create(texData->Width, texData->Height, texData->Format, texData->Pixels.data());
                }
                else if (upload.Type == AssetType::Model)
                {
                    auto* modelData = static_cast<ModelAssetData*>(upload.Data.get());
                    newAsset = Model::Create(modelData->Meshes, modelData->Materials);
                }
                else if (upload.Type == AssetType::Material)
                {
                    auto* matData = static_cast<MaterialAssetData*>(upload.Data.get());
                    auto material = std::make_shared<Material>();
                    material->Deserialize(matData->JsonData);
                    newAsset = material;
                }
            }

            {
                std::lock_guard<std::mutex> assetLock(s_AssetMutex);
                
                // Clear loading flag
                s_LoadingAssets.erase(upload.Handle);

                if (newAsset) {
                    newAsset->Handle = upload.Handle;
                    newAsset->LastAccessedTime = Time::GetTime();
                    s_Assets[upload.Handle] = newAsset;
                }
            }
        }
        s_UploadQueue.clear();
    }
}
