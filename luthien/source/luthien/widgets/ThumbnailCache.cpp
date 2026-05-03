#include "lepch.h"
#include "luthien/widgets/ThumbnailCache.h"
#include "luthien/widgets/ThumbnailGenerator.h"

#include "luth/events/EventBus.h"
#include "luth/jobs/SpinLock.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/resources/FileSystem.h"

#include "luthien/events/EditorSignals.h"

#include <backends/imgui_impl_vulkan.h>

#include <atomic>
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

        constexpr u32 kMaxBakesInFlight = 8;

        SpinLock                                   s_MapLock;
        std::unordered_map<UUID, Entry, UUIDHash>  s_Entries;

        SpinLock                                   s_QueueLock;
        std::vector<TextureCompletion>             s_TexCompletions;

        std::atomic<u32>   s_BakesInFlight{ 0 };
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
            SpinLockGuard g(s_MapLock);
            auto it = s_Entries.find(asset);
            if (it == s_Entries.end()) {
                s_BakesInFlight.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
            it->second.state = BakeState::Failed;
            ++it->second.retryCount;
            s_BakesInFlight.fetch_sub(1, std::memory_order_relaxed);
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
                ThumbnailCache::Invalidate(sig.GetAsset());
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
        std::vector<TextureCompletion> local;
        {
            SpinLockGuard g(s_QueueLock);
            local.swap(s_TexCompletions);
        }
        if (local.empty()) return;
        if (!VulkanActive()) {
            // Backend gone mid-bake — drop completions silently; map mutation
            // would race teardown anyway.
            s_BakesInFlight.fetch_sub(static_cast<u32>(local.size()), std::memory_order_relaxed);
            return;
        }

        for (auto& c : local) {
            // Texture creation + descriptor allocation outside the map lock —
            // ImGui_ImplVulkan_AddTexture is microsecond-scale, well over V1's
            // <100-cycle budget for SpinLock-held work.
            std::shared_ptr<Texture> tex = Texture::Create(
                c.width, c.height, TextureFormat::RGBA8, c.pixels.data());
            if (!tex) {
                s_BakesInFlight.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }
            auto vkTex = std::static_pointer_cast<VKTexture>(tex);
            VkDescriptorSet newSet = ImGui_ImplVulkan_AddTexture(
                vkTex->GetSampler(),
                vkTex->GetImageView(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

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
            s_BakesInFlight.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    ImTextureID ThumbnailCache::Get(UUID asset, AssetType type)
    {
        if (!asset.IsValid())             return 0;
        if (!VulkanActive())              return 0;
        if (!FileSystem::HasProject())    return 0;
        if (!IsSupportedType(type))       return 0;

        bool needsBake = false;
        {
            SpinLockGuard g(s_MapLock);
            auto it = s_Entries.find(asset);
            if (it != s_Entries.end()) {
                if (it->second.state == BakeState::Ready)
                    return (ImTextureID)it->second.imguiSet;
                return 0;
            }
            // Insert Pending sentinel atomically with the bake decision so
            // back-to-back Get() calls in the same frame don't dispatch twice.
            if (s_BakesInFlight.load(std::memory_order_relaxed) >= kMaxBakesInFlight)
                return 0;
            Entry e;
            e.type  = type;
            e.state = BakeState::Pending;
            s_Entries.emplace(asset, std::move(e));
            needsBake = true;
        }

        if (needsBake) {
            s_BakesInFlight.fetch_add(1, std::memory_order_relaxed);
            ThumbnailGenerator::Dispatch(asset, type);
        }
        return 0;
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
    }

    void ThumbnailCache::Clear()
    {
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
