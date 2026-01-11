#include "luthpch.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/importers/TextureImporter.h"
#include "luth/resources/importers/ModelImporter.h"
#include "luth/resources/importers/MaterialImporter.h"
#include "luth/renderer/Texture.h"
#include "luth/renderer/Model.h"
#include "luth/renderer/Material.h"

namespace Luth
{
    std::unordered_map<UUID, std::shared_ptr<Asset>, UUIDHash> AssetManager::s_Assets;
    std::unordered_set<UUID, UUIDHash> AssetManager::s_LoadingAssets;
    std::unordered_map<AssetType, std::unique_ptr<AssetImporter>> AssetManager::s_Importers;
    std::mutex AssetManager::s_AssetMutex;
    std::mutex AssetManager::s_UploadMutex;
    std::vector<AssetManager::PendingUpload> AssetManager::s_UploadQueue;

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

        // Load Data
        std::unique_ptr<AssetData> data = nullptr;
        if (s_Importers.find(info.Type) != s_Importers.end())
        {
            s_Importers[info.Type]->Import(info.Path, data);
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

        if (newAsset) { newAsset->Handle = handle; s_Assets[handle] = newAsset; }
        return newAsset;
    }

    bool AssetManager::IsLoaded(UUID handle)
    {
        std::lock_guard<std::mutex> lock(s_AssetMutex);
        return s_Assets.find(handle) != s_Assets.end();
    }

    void AssetManager::LoadJob(JobSystem::JobArgs args)
    {
        LH_PROFILE_FUNCTION();

        LoadRequest* req = (LoadRequest*)args.data;
        LH_PROFILE_TAG("Asset", req->Path.string().c_str());
        
        std::unique_ptr<AssetData> data = nullptr;
        bool success = false;

        if (s_Importers.find(req->Type) != s_Importers.end())
        {
            auto& importer = s_Importers[req->Type];
            success = importer->Import(req->Path, data);
        }
        else
        {
            LH_CORE_ERROR("AssetManager: No importer for type {0}", (int)req->Type);
        }

        if (!success)
        {
            LH_CORE_ERROR("AssetManager: Failed to import {0}", req->Path.string());
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
                    s_Assets[upload.Handle] = newAsset;
                }
            }
        }
        s_UploadQueue.clear();
    }
}
