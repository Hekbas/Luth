#include "luthpch.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/importers/TextureImporter.h"
#include "luth/resources/importers/ModelImporter.h"
#include "luth/resources/importers/MaterialImporter.h"
#include "luth/resources/importers/PhysicsMaterialImporter.h"
#include "luth/resources/importers/ShaderImporter.h"
#include "luth/resources/importers/AnimationClipImporter.h"
#include "luth/physics/PhysicsMaterial.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/AnimationClip.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/shader/Shader.h"
#include "luth/renderer/backend/vulkan/UploadContext.h"
#include "luth/core/time/Time.h"

#include <emmintrin.h> // _mm_pause (main-thread wait spin)

namespace Luth
{
    std::unordered_map<UUID, std::shared_ptr<Asset>, UUIDHash> AssetManager::s_Assets;
    std::unordered_set<UUID, UUIDHash> AssetManager::s_LoadingAssets;
    std::unordered_set<UUID, UUIDHash> AssetManager::s_ImportingAssets;
    std::unordered_map<AssetType, std::unique_ptr<AssetImporter>> AssetManager::s_Importers;
    std::mutex AssetManager::s_AssetMutex;
    std::mutex AssetManager::s_UploadMutex;
    std::mutex AssetManager::s_ImportMutex;
    std::vector<AssetManager::PendingUpload> AssetManager::s_UploadQueue;

    static float s_GCTimer = 0.0f;
    static const float k_GCInterval = 2.0f; // seconds

    void AssetManager::Init()
    {
        s_Importers[AssetType::Texture] = std::make_unique<TextureImporter>();
        s_Importers[AssetType::Model] = std::make_unique<ModelImporter>();
        s_Importers[AssetType::Material] = std::make_unique<MaterialImporter>();
        s_Importers[AssetType::Shader] = std::make_unique<ShaderImporter>();
        s_Importers[AssetType::Animation] = std::make_unique<AnimationClipImporter>();
        s_Importers[AssetType::PhysicsMaterial] = std::make_unique<PhysicsMaterialImporter>();
    }

    void AssetManager::Shutdown()
    {
        s_Assets.clear();
        s_LoadingAssets.clear();
        s_ImportingAssets.clear();
        s_Importers.clear();
        s_UploadQueue.clear();
    }

    void AssetManager::LoadAsync(UUID handle)
    {
        LH_PROFILE_FUNCTION();
        std::lock_guard<std::mutex> lock(s_AssetMutex);

        if (s_Assets.find(handle) != s_Assets.end()) return;
        if (s_LoadingAssets.find(handle) != s_LoadingAssets.end()) return;

        const auto& info = AssetDatabase::GetMetadata(handle);
        if (info.Path.empty())
        {
            LH_LOG(Assets, error, "AssetManager: UUID {0} not found in DB", handle.ToString());
            return;
        }

        // Mark as loading to prevent duplicate requests
        s_LoadingAssets.insert(handle);
        
        LoadRequest* req = new LoadRequest{ handle, info.Path, info.Type };

        JobSystem::Execute(LoadJob, req, nullptr, "AssetLoad");
    }

    std::shared_ptr<Asset> AssetManager::LoadImmediate(UUID handle)
    {
        LH_PROFILE_FUNCTION();

        if (auto asset = GetAsset<Asset>(handle)) return asset;

        const auto& info = AssetDatabase::GetMetadata(handle);
        if (info.Path.empty()) return nullptr;

        // Ensure the artifact exists (self-import if missing), serialized against any in-flight import
        // of the same UUID so a background ImportDirty and this blocking load never write it twice.
        fs::path artifactPath = AssetDatabase::GetArtifactPath(handle);
        EnsureImported(handle, /*forceReimport*/ false, /*blockIfBusy*/ true);
        if (!fs::exists(artifactPath)) return nullptr;

        // A present-but-incompatible artifact (an older schema after a format-version bump) fails to
        // deserialize; force one regeneration so a schema bump self-heals instead of failing every load.
        auto data = DeserializeArtifact(info.Type, artifactPath);
        if (!data)
        {
            LH_LOG(Assets, warn, "AssetManager: artifact incompatible (schema bump?) -- reimporting {0}", info.Path.string());
            EnsureImported(handle, /*forceReimport*/ true, /*blockIfBusy*/ true);
            data = DeserializeArtifact(info.Type, artifactPath);
        }
        if (!data) return nullptr;

        // Create the asset (main thread).
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
        LH_PROFILE_FUNCTION();
        // Forced, blocking reimport (editor "Apply", IngestFile). Routed through the guard so it
        // serializes against any concurrent background import of the same UUID.
        EnsureImported(handle, /*forceReimport*/ true, /*blockIfBusy*/ true);
    }

    // ---- Import serialization ----

    bool AssetManager::TryBeginImport(UUID handle)
    {
        std::lock_guard<std::mutex> lock(s_ImportMutex);
        return s_ImportingAssets.insert(handle).second; // true if we newly claimed it
    }

    void AssetManager::EndImport(UUID handle)
    {
        std::lock_guard<std::mutex> lock(s_ImportMutex);
        s_ImportingAssets.erase(handle);
    }

    bool AssetManager::IsImporting(UUID handle)
    {
        std::lock_guard<std::mutex> lock(s_ImportMutex);
        return s_ImportingAssets.find(handle) != s_ImportingAssets.end();
    }

    void AssetManager::WaitWhileImporting(UUID handle)
    {
        // V1: never poll while holding s_ImportMutex. Worker fibers yield so the owning import fiber can
        // run; the main thread (V2-isolated, no JobContext, YieldFiber is a no-op there) busy-spins.
        const bool onWorker = (JobSystem::GetCurrentJobContext() != nullptr);
        while (IsImporting(handle))
        {
            if (onWorker) JobSystem::YieldFiber();
            else          _mm_pause();
        }
    }

    void AssetManager::EnsureImported(UUID handle, bool forceReimport, bool blockIfBusy)
    {
        // Acquire the per-UUID import token, or wait for the in-flight owner. Invariant: holding the
        // token means we are the sole importer AND it is actively running, so a waiter blocks on one
        // asset (this handle), never on the whole dispatch queue.
        while (!TryBeginImport(handle))
        {
            if (!blockIfBusy) return;                    // fire-and-forget: the owner is running it
            WaitWhileImporting(handle);
            if (!forceReimport && fs::exists(AssetDatabase::GetArtifactPath(handle)))
                return;                                  // owner produced it; nothing left to do
            // else: owner failed to produce it, or we must force -> loop to claim the token ourselves
        }

        // RAII release: the token must clear on every exit, including an importer throw (caught below).
        struct ReleaseGuard { UUID h; ~ReleaseGuard() { EndImport(h); } } releaseGuard{ handle };

        const auto& info = AssetDatabase::GetMetadata(handle);
        if (info.Path.empty()) return;

        fs::path artifactPath = AssetDatabase::GetArtifactPath(handle);
        if (!forceReimport && fs::exists(artifactPath)) return; // a racing reader already produced it

        auto it = s_Importers.find(info.Type);
        if (it == s_Importers.end()) return;             // Font/Scene etc. have no importer

        // Catch importer exceptions here: a throw must not unwind across the fiber's asm context-switch
        // boundary (FiberEntryPoint has no catch), which would terminate the process.
        try
        {
            if (!it->second->Import(info.Path, artifactPath))
                LH_LOG(Assets, error, "Failed to import asset: {0}", info.Path.string());
        }
        catch (const std::exception& e)
        {
            LH_LOG(Assets, error, "Importer threw for {0}: {1}", info.Path.string(), e.what());
        }
        catch (...)
        {
            LH_LOG(Assets, error, "Importer threw (unknown) for {0}", info.Path.string());
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
        LH_PROFILE_FUNCTION();
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
        LH_PROFILE_FUNCTION();

        const auto& dirtyAssets = AssetDatabase::GetDirtyAssets();
        if (dirtyAssets.empty()) return;

        // Fire-and-forget: one job per dirty asset, no WaitForCounter -- the main thread never blocks on
        // the import storm (cold Bistro = hundreds of texture bakes). The token is claimed inside the job
        // when it starts (not here), so a main-thread reader for a still-queued asset wins TryBeginImport
        // and imports that one inline instead of stalling on the whole queue. IsImporting skips assets
        // already actively importing (LoadProject dispatches, then the next frame sees the same
        // still-uncleared dirty set before ClearDirtyAssets runs).
        u32 dispatched = 0;
        for (UUID handle : dirtyAssets)
        {
            if (IsImporting(handle)) continue;
            ImportRequest* req = new ImportRequest{ handle };
            JobSystem::Execute(ImportJob, req, nullptr, "AssetImport", JobSystem::Priority::Low);
            ++dispatched;
        }
        if (dispatched > 0)
            LH_LOG(Assets, info, "Importing {} assets (async)...", dispatched);
    }

    void AssetManager::ImportJob(JobSystem::JobArgs args)
    {
        LH_PROFILE_FUNCTION();
        ImportRequest* req = (ImportRequest*)args.data;
        // Dirty => artifact absent, so no force; non-blocking, so if another entry point already owns
        // this import we bail rather than wait (it is being produced).
        EnsureImported(req->Handle, /*forceReimport*/ false, /*blockIfBusy*/ false);
        delete req;
    }

    // ---- Shared helpers ----

    std::unique_ptr<AssetData> AssetManager::DeserializeArtifact(AssetType type, const fs::path& artifactPath)
    {
        LH_PROFILE_FUNCTION();
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
        else if (type == AssetType::PhysicsMaterial) {
            auto d = std::make_unique<PhysicsMaterialAssetData>();
            if (AssetSerializer::DeserializePhysicsMaterial(artifactPath, *d)) return d;
        }
        return nullptr;
    }

    std::shared_ptr<Asset> AssetManager::FinalizeAsset(AssetType type, AssetData* data, const fs::path& sourcePath)
    {
        LH_PROFILE_FUNCTION();
        if (type == AssetType::Texture) {
            auto* d = static_cast<TextureAssetData*>(data);
            // Pre-baked BCn uploads the stored mip chain directly; uncompressed keeps the blit-mip path.
            if (GetTextureFormatInfo(d->Format).compressed)
                return Texture::Create(d->Width, d->Height, d->Format, d->Pixels.data(),
                                       (u64)d->Pixels.size(), d->MipLevels, d->Settings);
            return Texture::Create(d->Width, d->Height, d->Format, d->Pixels.data(), d->Settings);
        }
        else if (type == AssetType::Model) {
            auto* d = static_cast<ModelAssetData*>(data);
            if (d->IsSkinned)
                return Model::Create(d->Meshes, d->Materials, d->SkeletonData, d->AnimationClipUUIDs, true);
            auto model = Model::Create(d->Meshes, d->Materials);
            model->SetSceneGraph(std::move(d->Nodes), std::move(d->Cameras), std::move(d->Lights));
            return model;
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
        else if (type == AssetType::PhysicsMaterial) {
            auto* d = static_cast<PhysicsMaterialAssetData*>(data);
            auto mat = std::make_shared<PhysicsMaterial>();
            mat->Deserialize(d->JsonData);
            return mat;
        }
        return nullptr;
    }

    // ---- Async loading ----

    void AssetManager::LoadJob(JobSystem::JobArgs args)
    {
        LH_PROFILE_FUNCTION();

        LoadRequest* req = (LoadRequest*)args.data;
        LH_PROFILE_TAG("Asset", req->Path.string().c_str());

        // Ensure the artifact exists (self-import if missing), serialized against any in-flight import
        // of the same UUID. Worker fiber -> waits by yielding, never becomes a second writer.
        fs::path artifactPath = AssetDatabase::GetArtifactPath(req->Handle);
        EnsureImported(req->Handle, /*forceReimport*/ false, /*blockIfBusy*/ true);

        // Load from the artifact.
        std::unique_ptr<AssetData> data = nullptr;
        if (fs::exists(artifactPath))
            data = DeserializeArtifact(req->Type, artifactPath);

        // Schema-bump self-heal (mirrors LoadImmediate): a present-but-incompatible artifact fails to
        // deserialize; force one reimport so async/material-referenced assets migrate instead of vanishing.
        if (!data && fs::exists(artifactPath))
        {
            LH_LOG(Assets, warn, "AssetManager: artifact incompatible (schema bump?) -- reimporting {0}", req->Path.string());
            EnsureImported(req->Handle, /*forceReimport*/ true, /*blockIfBusy*/ true);
            data = DeserializeArtifact(req->Type, artifactPath);
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

        // Idle frames must still tick; upload fences retire 1-2 frames after the ctor pushed.
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

                // Model-specific: async-load the model's animation clips so AnimationSystem can sample them next frame.
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
