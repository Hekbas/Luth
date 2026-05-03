#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"

#include <imgui.h>

namespace Luth::UI
{
    // UUID-keyed asset thumbnail cache. Get() is cheap on hit; misses queue an
    // async bake (texture/mesh/material generators) and return 0 so the caller
    // draws an icon fallback. Bakes complete on worker fibers and post results
    // to a SpinLock-guarded queue; Drain() runs on main and binds the ImGui
    // descriptors so subsequent frames hit.
    //
    // invariant: asset-domain (UUID-keyed). widgets/TexturePreview.cpp's
    // s_TextureCache is runtime-domain (raw Texture* keyed) — distinct
    // abstractions that coexist by intent.
    class ThumbnailCache
    {
    public:
        static void Init();
        static void Shutdown();

        // Bind queued completions on main; wired in Editor::Render before gather.
        static void Drain();

        // Hit: ImTextureID (cast VkDescriptorSet). Miss / non-Vulkan / no project
        // / unsupported AssetType: 0. Safe per-frame.
        static ImTextureID Get(UUID asset, AssetType type);

        // Drop entry, fence-cleanup descriptor via VulkanContext::PushDeletion.
        // Main-thread only (called from AssetChangedSignal handler).
        static void Invalidate(UUID asset);

        // Drop everything. Used on project unload + size-change.
        static void Clear();
    };
}
