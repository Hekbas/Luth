#include "lepch.h"
#include "luthien/widgets/ThumbnailCache.h"

#include "luth/events/EventBus.h"
#include "luth/jobs/SpinLock.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/resources/FileSystem.h"

#include "luthien/events/EditorSignals.h"

#include <backends/imgui_impl_vulkan.h>

#include <unordered_map>
#include <vector>

namespace Luth::UI
{
    namespace
    {
        enum class BakeState : u8 { Idle, Pending, InFlight, Ready, Failed };

        struct Entry
        {
            VkDescriptorSet   imguiSet   = VK_NULL_HANDLE;
            AssetType         type       = AssetType::None;
            BakeState         state      = BakeState::Idle;
            u8                retryCount = 0;
            std::vector<UUID> deps;        // material → sampled-texture cascade
        };

        // Placeholder until typed payloads land alongside generators. Drain
        // swaps the queue out under s_QueueLock and walks the local copy
        // unlocked — V1 micro-critical mirror of MainThreadPump::Drain.
        struct CompletionMsg
        {
            UUID      asset;
            AssetType type = AssetType::None;
        };

        SpinLock                                   s_MapLock;
        std::unordered_map<UUID, Entry, UUIDHash>  s_Entries;

        SpinLock                                   s_QueueLock;
        std::vector<CompletionMsg>                 s_Completions;

        SubscriptionHandle s_AssetSub;
        bool               s_Initialized = false;

        bool VulkanActive()
        {
            return Renderer::GetBackend()
                && Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan;
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
        // Hook only — typed-payload processing lands with each generator.
        std::vector<CompletionMsg> local;
        {
            SpinLockGuard g(s_QueueLock);
            local.swap(s_Completions);
        }
        (void)local;
    }

    ImTextureID ThumbnailCache::Get(UUID asset, AssetType type)
    {
        if (!asset.IsValid())             return 0;
        if (!VulkanActive())              return 0;
        if (!FileSystem::HasProject())    return 0;
        if (type != AssetType::Texture
         && type != AssetType::Model
         && type != AssetType::Material)  return 0;

        SpinLockGuard g(s_MapLock);
        auto it = s_Entries.find(asset);
        if (it == s_Entries.end()) return 0;
        if (it->second.state != BakeState::Ready) return 0;
        return (ImTextureID)it->second.imguiSet;
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
