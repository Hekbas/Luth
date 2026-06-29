#include "lepch.h"
#include "luthien/widgets/ThumbnailCache.h"
#include "luthien/widgets/ThumbnailGenerator.h"

#include "luth/core/diagnostics/Log.h"
#include "luth/events/EventBus.h"
#include "luth/jobs/SpinLock.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"

#include "luthien/Editor.h"
#include "luthien/EditorSettings.h"
#include "luthien/events/EditorSignals.h"

#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Luth::UI
{
    namespace
    {
        enum class BakeState : u8 { Idle, Pending, InFlight, Ready, Failed };

        struct Entry
        {
            VkDescriptorSet              imguiSet   = VK_NULL_HANDLE;
            std::shared_ptr<Texture>     tex;            // keeps the small backing texture alive
            AssetType                    type       = AssetType::None;
            BakeState                    state      = BakeState::Idle;
            u8                           retryCount = 0;
            std::vector<UUID>            deps;          // material → sampled-texture cascade
        };

        // Texture-completion record posted by ThumbnailGenerator's worker.
        // Pixel buffer is moved into the message so the worker doesn't outlive
        // its allocation; Drain reads it on main and constructs a small VKTexture.
        struct TextureCompletion
        {
            UUID            asset;
            std::vector<u8> pixels;     // RGBA8, width * height * 4
            u32             width  = 0;
            u32             height = 0;
        };

        constexpr u32 kMaxDrainPerFrame              = 8;    // bounds per-frame upload pressure
        constexpr u32 kMaxTextureDispatchPerFrame    = 5;    // CPU bakes — run on workers, cheap
        constexpr u32 kMaxBakeDispatchPerFrame       = 1;    // GPU bakes — block main, ~10–30 ms each

        // Deferred dispatch — Get / ScanDiskCache append; Drain pumps up to
        // kMaxDispatchPerFrame per frame. Spreads cold-start CPU work over
        // multiple frames to bound stutter on first folder entry.
        struct DispatchRequest
        {
            UUID      asset;
            AssetType type;
            bool      fromDisk;   // true → DispatchLoadFromDisk, false → Dispatch
        };

        namespace fs = std::filesystem;

        fs::path ThumbnailDir()
        {
            return FileSystem::ProjectPath() / ".luth" / "thumbnails";
        }

        fs::path ThumbnailFilePath(UUID asset)
        {
            return ThumbnailDir() / (asset.ToString() + ".png");
        }

        SpinLock                                   s_MapLock;
        std::unordered_map<UUID, Entry, UUIDHash>  s_Entries;

        SpinLock                                   s_QueueLock;
        std::vector<TextureCompletion>             s_TexCompletions;

        SpinLock                                   s_DispatchLock;
        std::vector<DispatchRequest>               s_PendingDispatches;

        SubscriptionHandle s_AssetSub;
        bool               s_Initialized = false;

        bool VulkanActive()
        {
            return Renderer::GetBackend()
                && Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan;
        }

        bool IsSupportedType(AssetType t)
        {
            return t == AssetType::Texture
                || t == AssetType::Model
                || t == AssetType::Material;
        }

        // Mark Pending entry as Failed so future Gets see a terminal state and
        // don't keep re-dispatching the same broken bake. invariant: callers
        // must NOT hold s_MapLock — this acquires it.
        void MarkFailed(UUID asset)
        {
            SpinLockGuard g(s_MapLock);
            auto it = s_Entries.find(asset);
            if (it == s_Entries.end()) return;
            if (it->second.state != BakeState::Ready) {
                it->second.state = BakeState::Failed;
                ++it->second.retryCount;
            }
        }
    }

    // Wire-internal — invoked from ThumbnailGenerator's worker fiber. Not part
    // of the public ThumbnailCache surface; intentionally not in the header.
    namespace ThumbnailCacheInternal
    {
        void PushTextureCompletion(UUID asset, std::vector<u8> pixels, u32 width, u32 height)
        {
            TextureCompletion msg;
            msg.asset  = asset;
            msg.pixels = std::move(pixels);
            msg.width  = width;
            msg.height = height;
            SpinLockGuard g(s_QueueLock);
            s_TexCompletions.emplace_back(std::move(msg));
        }

        void NotifyBakeFailed(UUID asset)
        {
            MarkFailed(asset);
        }

        void SetEntryDeps(UUID asset, std::vector<UUID> deps)
        {
            SpinLockGuard g(s_MapLock);
            auto it = s_Entries.find(asset);
            if (it != s_Entries.end()) it->second.deps = std::move(deps);
        }
    }

    void ThumbnailCache::Init()
    {
        if (s_Initialized) return;
        s_Initialized = true;

        // The signal currently fires Modified for every dirty UUID (Editor::Init
        // forwards the AssetDatabase change callback); Imported/Deleted will get
        // distinct treatment if the engine ever differentiates. For thumbnails,
        // any change → invalidate + re-bake the latest.
        s_AssetSub = EventBus::Subscribe<AssetChangedSignal>(BusType::MainThread,
            [](Event& e) {
                auto& sig = static_cast<AssetChangedSignal&>(e);
                const UUID changed = sig.GetAsset();
                ThumbnailCache::Invalidate(changed);

                // Cascade: invalidate material thumbnails whose deps contain
                // the changed UUID. invariant: walk under s_MapLock to collect
                // dependents, invalidate outside so PushDeletion + disk remove
                // run unblocked. Edge-frequency (asset save / file-watch) —
                // V1 micro-critical budget doesn't apply on this path.
                std::vector<UUID> dependents;
                {
                    SpinLockGuard g(s_MapLock);
                    for (auto& [uuid, entry] : s_Entries) {
                        if (entry.type != AssetType::Material) continue;
                        for (const UUID& dep : entry.deps) {
                            if (dep == changed) { dependents.push_back(uuid); break; }
                        }
                    }
                }
                for (const UUID& u : dependents) ThumbnailCache::Invalidate(u);
            });
    }

    void ThumbnailCache::Shutdown()
    {
        if (!s_Initialized) return;
        EventBus::Unsubscribe(BusType::MainThread, s_AssetSub);
        s_AssetSub = {};

        // invariant: at editor shutdown the ImGui descriptor pool is about to
        // be destroyed (Editor::Shutdown vkDestroyDescriptorPool), taking every
        // descriptor with it. Skip PushDeletion — its fenced lambdas wouldn't
        // get a chance to run before the pool is gone.
        {
            SpinLockGuard g(s_MapLock);
            s_Entries.clear();
        }
        s_Initialized = false;
    }

    void ThumbnailCache::Drain()
    {
        // Pump deferred-dispatch queue. FIFO — ProjectPanel iterates the grid
        // top-to-bottom, so push order matches reading order; taking from the
        // front yields top-down thumbnail population. Per-type budgets keep
        // mesh/material bakes (synchronous on main, ~10–30 ms each) from
        // dominating frame time even when many are queued.
        {
            std::vector<DispatchRequest> batch;
            {
                SpinLockGuard g(s_DispatchLock);
                u32 textureLeft = kMaxTextureDispatchPerFrame;
                u32 bakeLeft    = kMaxBakeDispatchPerFrame;
                auto it = s_PendingDispatches.begin();
                while (it != s_PendingDispatches.end() && (textureLeft > 0 || bakeLeft > 0)) {
                    bool take = false;
                    if (it->fromDisk && textureLeft > 0) {
                        take = true; --textureLeft;
                    } else if (it->type == AssetType::Texture && textureLeft > 0) {
                        take = true; --textureLeft;
                    } else if ((it->type == AssetType::Model || it->type == AssetType::Material) && bakeLeft > 0) {
                        take = true; --bakeLeft;
                    }
                    if (take) { batch.push_back(*it); it = s_PendingDispatches.erase(it); }
                    else      { ++it; }
                }
            }
            for (auto& r : batch) {
                if (r.fromDisk) ThumbnailGenerator::DispatchLoadFromDisk(r.asset, r.type);
                else            ThumbnailGenerator::Dispatch(r.asset, r.type);
            }
        }

        std::vector<TextureCompletion> local;
        {
            SpinLockGuard g(s_QueueLock);
            local.swap(s_TexCompletions);
        }
        if (local.empty()) return;
        if (!VulkanActive()) {
            // Backend gone mid-bake — drop completions silently; map mutation
            // would race teardown anyway.
            return;
        }

        // Cap per-Drain work so a 500-asset cold-start can't drop the frame.
        // Excess completions are pushed back for the next Drain.
        const size_t toProcess = std::min<size_t>(local.size(), kMaxDrainPerFrame);
        if (toProcess < local.size()) {
            SpinLockGuard g(s_QueueLock);
            for (size_t i = toProcess; i < local.size(); ++i)
                s_TexCompletions.emplace_back(std::move(local[i]));
        }

        for (size_t idx = 0; idx < toProcess; ++idx) {
            auto& c = local[idx];
            try {
                // Texture creation + descriptor allocation outside the map lock —
                // ImGui_ImplVulkan_AddTexture is microsecond-scale, well over V1's
                // <100-cycle budget for SpinLock-held work.
                // Mipmaps + trilinear so the slider's small sizes (16-32 px) don't
                // shimmer when sampling the 128 px base level. ClampToEdge avoids
                // edge-bleed at preview boundaries.
                TextureSettings tsettings;
                tsettings.GenerateMipmaps = true;
                tsettings.WrapMode  = TextureWrapMode::ClampToEdge;
                tsettings.MinFilter = TextureFilterMode::LinearMipmapLinear;
                tsettings.MagFilter = TextureFilterMode::Linear;
                std::shared_ptr<Texture> tex = Texture::Create(
                    c.width, c.height, TextureFormat::RGBA8, c.pixels.data(), tsettings);
                if (!tex) { MarkFailed(c.asset); continue; }

                auto vkTex = std::static_pointer_cast<VKTexture>(tex);
                VkDescriptorSet newSet = ImGui_ImplVulkan_AddTexture(
                    vkTex->GetSampler(),
                    vkTex->GetImageView(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

                if (newSet == VK_NULL_HANDLE) {
                    // ImGui descriptor pool exhausted (size=2000). Mark Failed so
                    // we don't busy-loop dispatching against an exhausted pool;
                    // a future polish epic will own a dedicated thumbnail pool.
                    LH_LOG(Editor, warn, "Thumbnail: ImGui descriptor pool exhausted for {}", c.asset.ToString());
                    MarkFailed(c.asset);
                    continue;
                }

                VkDescriptorSet oldSet = VK_NULL_HANDLE;
                bool installed = false;
                {
                    SpinLockGuard g(s_MapLock);
                    auto it = s_Entries.find(c.asset);
                    if (it != s_Entries.end()) {
                        oldSet = it->second.imguiSet;
                        it->second.imguiSet = newSet;
                        it->second.tex      = tex;
                        it->second.state    = BakeState::Ready;
                        installed = true;
                    }
                }

                auto& ctx = VulkanContext::Get();
                if (!installed) {
                    // Entry was Invalidated mid-bake — drop the just-created descriptor
                    // and the backing texture (Ref releases on scope exit).
                    ctx.PushDeletion([newSet]() { ImGui_ImplVulkan_RemoveTexture(newSet); });
                } else if (oldSet != VK_NULL_HANDLE) {
                    ctx.PushDeletion([oldSet]() { ImGui_ImplVulkan_RemoveTexture(oldSet); });
                }
            } catch (const std::exception& e) {
                LH_LOG(Editor, error, "Thumbnail: Drain threw for {}: {}", c.asset.ToString(), e.what());
                MarkFailed(c.asset);
            } catch (...) {
                LH_LOG(Editor, error, "Thumbnail: Drain threw non-std exception for {}", c.asset.ToString());
                MarkFailed(c.asset);
            }
        }
    }

    ImTextureID ThumbnailCache::Get(UUID asset, AssetType type)
    {
        if (!asset.IsValid())                       return 0;
        if (!VulkanActive())                        return 0;
        if (!FileSystem::HasProject())              return 0;
        if (!IsSupportedType(type))                 return 0;
        if (!Editor::GetSettings().thumbnailsEnabled) return 0;

        // Pending sentinel acts as the de-dupe — back-to-back Gets for the same
        // UUID see Pending and skip re-dispatch. Natural backpressure (IOThread
        // serialization, kMaxDrainPerFrame upload cap) bounds concurrent work
        // without a separate counter that could leak and stall the cache.
        bool needsBake = false;
        {
            SpinLockGuard g(s_MapLock);
            auto it = s_Entries.find(asset);
            if (it != s_Entries.end()) {
                if (it->second.state == BakeState::Ready)
                    return (ImTextureID)it->second.imguiSet;
                return 0;   // Pending / InFlight / Failed → icon fallback
            }
            Entry e;
            e.type  = type;
            e.state = BakeState::Pending;
            s_Entries.emplace(asset, std::move(e));
            needsBake = true;
        }

        if (needsBake) {
            SpinLockGuard g(s_DispatchLock);
            s_PendingDispatches.push_back({asset, type, /*fromDisk=*/false});
        }
        return 0;
    }

    ImVec2 ThumbnailCache::GetThumbnailSize(UUID asset)
    {
        if (!asset.IsValid()) return ImVec2(0, 0);
        SpinLockGuard g(s_MapLock);
        auto it = s_Entries.find(asset);
        if (it == s_Entries.end() || !it->second.tex) return ImVec2(0, 0);
        return ImVec2(static_cast<float>(it->second.tex->GetWidth()),
                      static_cast<float>(it->second.tex->GetHeight()));
    }

    void ThumbnailCache::Invalidate(UUID asset)
    {
        if (!asset.IsValid()) return;

        VkDescriptorSet pendingFree = VK_NULL_HANDLE;
        {
            SpinLockGuard g(s_MapLock);
            auto it = s_Entries.find(asset);
            if (it == s_Entries.end()) return;
            pendingFree = it->second.imguiSet;
            s_Entries.erase(it);
        }

        if (pendingFree != VK_NULL_HANDLE && VulkanActive()) {
            VulkanContext::Get().PushDeletion([pendingFree]() {
                ImGui_ImplVulkan_RemoveTexture(pendingFree);
            });
        }

        // Drop the persisted PNG too so the next Get re-bakes from source rather
        // than re-loading a stale snapshot. Sync remove on main is fine — small
        // file, no I/O queue contention.
        if (FileSystem::HasProject()) {
            std::error_code ec;
            fs::remove(ThumbnailFilePath(asset), ec);
        }
    }

    void ThumbnailCache::ScanDiskCache()
    {
        if (!FileSystem::HasProject()) return;

        const fs::path dir = ThumbnailDir();
        std::error_code ec;
        if (!fs::exists(dir, ec)) return;

        // Two-pass: collect entries, drop orphans / over-cap, then dispatch loads.
        struct DiskEntry { fs::path path; UUID uuid; AssetType type; fs::file_time_type mtime; };
        std::vector<DiskEntry> entries;
        entries.reserve(64);

        for (const auto& dirent : fs::directory_iterator(dir, ec)) {
            if (!dirent.is_regular_file()) continue;
            if (dirent.path().extension() != ".png") continue;

            const std::string stem = dirent.path().stem().string();
            UUID uuid = UUID::FromString(stem);
            if (!uuid.IsValid()) {
                fs::remove(dirent.path(), ec);
                continue;
            }
            if (!AssetDatabase::Exists(uuid)) {
                fs::remove(dirent.path(), ec);   // orphan — asset gone
                continue;
            }
            const auto& meta = AssetDatabase::GetMetadata(uuid);
            if (!IsSupportedType(meta.Type)) {
                fs::remove(dirent.path(), ec);
                continue;
            }
            entries.push_back({ dirent.path(), uuid, meta.Type, dirent.last_write_time() });
        }

        // GC over-cap by oldest mtime first.
        const u32 maxEntries = Editor::GetSettings().thumbnailMaxDiskEntries;
        if (maxEntries > 0 && entries.size() > maxEntries) {
            std::sort(entries.begin(), entries.end(),
                [](const DiskEntry& a, const DiskEntry& b) { return a.mtime < b.mtime; });
            const size_t toDrop = entries.size() - maxEntries;
            for (size_t i = 0; i < toDrop; ++i)
                fs::remove(entries[i].path, ec);
            entries.erase(entries.begin(), entries.begin() + toDrop);
        }

        // Insert Pending sentinels + dispatch async loads. The Pending state
        // acts as both de-dupe (Get sees it, skips re-dispatch) and as the
        // implicit in-flight ledger.
        for (const auto& e : entries) {
            bool inserted = false;
            {
                SpinLockGuard g(s_MapLock);
                if (s_Entries.find(e.uuid) == s_Entries.end()) {
                    Entry mapEntry;
                    mapEntry.type  = e.type;
                    mapEntry.state = BakeState::Pending;
                    s_Entries.emplace(e.uuid, std::move(mapEntry));
                    inserted = true;
                }
            }
            if (!inserted) continue;
            SpinLockGuard gd(s_DispatchLock);
            s_PendingDispatches.push_back({e.uuid, e.type, /*fromDisk=*/true});
        }

        if (!entries.empty())
            LH_LOG(Editor, info, "Thumbnail: queued {} entries from disk cache", entries.size());
    }

    void ThumbnailCache::Clear()
    {
        // Discard deferred dispatches — UUIDs belong to the outgoing project.
        {
            SpinLockGuard g(s_DispatchLock);
            s_PendingDispatches.clear();
        }
        std::vector<VkDescriptorSet> toFree;
        {
            SpinLockGuard g(s_MapLock);
            toFree.reserve(s_Entries.size());
            for (auto& [uuid, entry] : s_Entries)
                if (entry.imguiSet != VK_NULL_HANDLE)
                    toFree.push_back(entry.imguiSet);
            s_Entries.clear();
        }
        if (!VulkanActive()) return;
        auto& ctx = VulkanContext::Get();
        for (VkDescriptorSet set : toFree) {
            ctx.PushDeletion([set]() {
                ImGui_ImplVulkan_RemoveTexture(set);
            });
        }
    }
}
