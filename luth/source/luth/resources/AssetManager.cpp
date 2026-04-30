#include "luthpch.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/importers/TextureImporter.h"
#include "luth/resources/importers/ModelImporter.h"
#include "luth/resources/importers/MaterialImporter.h"
#include "luth/resources/importers/ShaderImporter.h"
#include "luth/resources/importers/AnimationClipImporter.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/AnimationClip.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/shader/Shader.h"
#include "luth/renderer/backend/vulkan/UploadContext.h"
#include "luth/core/time/Time.h"

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
        s_Importers[AssetType::Shader] = std::make_unique<ShaderImporter>();
        s_Importers[AssetType::Animation] = std::make_unique<AnimationClipImporter>();
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
        JobSystem::Execute(LoadJob, req, nullptr, "AssetLoad");
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
        auto data = DeserializeArtifact(info.Type, artifactPath);
        if (!data) return nullptr;

        // 3. Create Asset (Main Thread)
        auto newAsset = FinalizeAsset(info.Type, data.get(), info.Path);

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

    bool AssetManager::HasImporter(AssetType type)
    {
        return s_Importers.find(type) != s_Importers.end();
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

    void AssetManager::Evict(UUID handle)
    {
        std::lock_guard<std::mutex> lock(s_AssetMutex);
        s_Assets.erase(handle);
        s_LoadingAssets.erase(handle);
    }

    u32 AssetManager::Trim(bool force)
    {
        std::lock_guard<std::mutex> lock(s_AssetMutex);
        f32 currentTime = Time::GetTime();
        const f32 timeout = 5.0f;
        u32 evicted = 0;

        for (auto it = s_Assets.begin(); it != s_Assets.end(); )
        {
            if (it->second.use_count() == 1 &&
                (force || (currentTime - it->second->LastAccessedTime > timeout)))
            {
                it = s_Assets.erase(it);
                ++evicted;
            }
            else
            {
                ++it;
            }
        }
        return evicted;
    }

    void AssetManager::ImportDirty()
    {
        const auto& dirtyAssets = AssetDatabase::GetDirtyAssets();
        if (dirtyAssets.empty()) return;

        std::vector<UUID> assetsToImport = dirtyAssets;
        LH_CORE_INFO("Importing {} assets...", assetsToImport.size());

        JobSystem::Counter importCounter(0);
        JobSystem::Dispatch((u32)assetsToImport.size(), 1, [](JobSystem::JobArgs args) {
            LH_PROFILE_SCOPE("AssetImport");
            std::vector<UUID>* assets = (std::vector<UUID>*)args.data;
            AssetManager::Import((*assets)[args.jobIndex]);
        }, &assetsToImport, &importCounter, "AssetImport");
        JobSystem::WaitForCounter(&importCounter);
    }

    // ================================================================
    // Shared helpers
    // ================================================================

    std::unique_ptr<AssetData> AssetManager::DeserializeArtifact(AssetType type, const fs::path& artifactPath)
    {
        if (type == AssetType::Texture) {
            auto d = std::make_unique<TextureAssetData>();
            if (AssetSerializer::DeserializeTexture(artifactPath, *d)) return d;
        }
        else if (type == AssetType::Model) {
            auto d = std::make_unique<ModelAssetData>();
            if (AssetSerializer::DeserializeModel(artifactPath, *d)) return d;
        }
        else if (type == AssetType::Material) {
            auto d = std::make_unique<MaterialAssetData>();
            if (AssetSerializer::DeserializeMaterial(artifactPath, *d)) return d;
        }
        else if (type == AssetType::Shader) {
            auto d = std::make_unique<ShaderAssetData>();
            if (AssetSerializer::DeserializeShader(artifactPath, *d)) return d;
        }
        else if (type == AssetType::Animation) {
            auto d = std::make_unique<AnimationAssetData>();
            if (AssetSerializer::DeserializeAnimation(artifactPath, *d)) return d;
        }
        return nullptr;
    }

    std::shared_ptr<Asset> AssetManager::FinalizeAsset(AssetType type, AssetData* data, const fs::path& sourcePath)
    {
        if (type == AssetType::Texture) {
            auto* d = static_cast<TextureAssetData*>(data);
            return Texture::Create(d->Width, d->Height, d->Format, d->Pixels.data(), d->Settings);
        }
        else if (type == AssetType::Model) {
            auto* d = static_cast<ModelAssetData*>(data);
            if (d->IsSkinned)
                return Model::Create(d->Meshes, d->Materials, d->SkeletonData, d->AnimationClipUUIDs, true);
            else
                return Model::Create(d->Meshes, d->Materials);
        }
        else if (type == AssetType::Material) {
            auto* d = static_cast<MaterialAssetData*>(data);
            auto material = std::make_shared<Material>();
            material->Deserialize(d->JsonData);
            return material;
        }
        else if (type == AssetType::Shader) {
            auto* d = static_cast<ShaderAssetData*>(data);
            return Shader::Create(d->Stage, d->SpirV, sourcePath);
        }
        else if (type == AssetType::Animation) {
            auto* d = static_cast<AnimationAssetData*>(data);
            return std::make_shared<AnimationClip>(d->Clip);
        }
        return nullptr;
    }

    // ================================================================
    // Async loading
    // ================================================================

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
                // Font and Scene types are handled directly — not an error
                if (req->Type != AssetType::Font && req->Type != AssetType::Scene)
                    LH_CORE_ERROR("AssetManager: No importer for type {0}", (int)req->Type);
            }
        }

        // 2. Load from Artifact
        std::unique_ptr<AssetData> data = nullptr;
        if (artifactReady)
            data = DeserializeArtifact(req->Type, artifactPath);

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

        // Drain pending-bind queue every frame, regardless of whether new assets finalized this
        // tick — texture upload fences typically retire 1-2 frames after the ctor pushed the
        // entry, and idle frames must still tick the pump.
        UploadContext::Get().DrainPendingBinds();

        std::lock_guard<std::mutex> lock(s_UploadMutex);
        if (s_UploadQueue.empty()) return;

        for (auto& upload : s_UploadQueue)
        {
            std::shared_ptr<Asset> newAsset = nullptr;

            if (upload.Data)
            {
                newAsset = FinalizeAsset(upload.Type, upload.Data.get(), {});

                // Material-specific: load texture dependencies
                if (upload.Type == AssetType::Material && newAsset)
                {
                    auto* material = static_cast<Material*>(newAsset.get());
                    for (const auto& map : material->GetTextures())
                    {
                        if (map.Uuid.IsValid())
                            LoadAsync(map.Uuid);
                    }
                }

                // Model-specific: trigger async load for the model's animation clips
                // so AnimationSystem can sample them on the next frame.
                if (upload.Type == AssetType::Model && newAsset)
                {
                    auto* model = static_cast<Model*>(newAsset.get());
                    for (const auto& clipUUID : model->GetAnimationClipUUIDs())
                    {
                        if (clipUUID.IsValid())
                            LoadAsync(clipUUID);
                    }
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
