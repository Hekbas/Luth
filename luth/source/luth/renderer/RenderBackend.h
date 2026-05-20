#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/QueueRecorders.h"
#include <memory>

namespace Luth
{
    // Abstract GPU API surface owned by Renderer. The concrete VulkanBackend handles swapchain
    // acquire, per-view × per-frame primary command buffers across three queue streams (gA / compute / gB), and
    // the per-view 3-submit topology that sequences cross-queue work. AcquireImage can legitimately fail on resize
    // or a suboptimal swapchain — the caller yields the fiber and retries the same frameIndex on the next tick.
    class RenderBackend
    {
    public:
        enum class API
        {
            None = 0,
            Vulkan
        };

        virtual ~RenderBackend() = default;

        virtual void Init(void* windowHandle) = 0;
        virtual void Shutdown() = 0;

        // false = skip this frame (caller yields + retries with the same frameIndex).
        virtual bool AcquireImage(u64 frameIndex) = 0;

        // Submit one view's triplet of primary command buffers using the per-view 3-submit topology
        // (gA → compute → gB; compute submit skipped if hasComputeWork is false). The first view's gA waits on
        // imageAvailable; subsequent views' gA waits on the previous view's gB signal at EARLY_FRAGMENT_TESTS.
        // The last view's gB signals renderFinished + caches the per-frame final timeline values consumed by
        // AcquireImage's GPU-N-2 reclaim predicate. No-op if AcquireImage skipped.
        virtual void SubmitView(u64 frameIndex, u32 viewSlot, QueueRecorders recorders,
                                bool hasComputeWork, bool isLastView) = 0;

        virtual void OnResize(u32 width, u32 height) = 0;

        static API GetAPI() { return s_API; }
        static std::unique_ptr<RenderBackend> Create();

    protected:
        static API s_API;
    };
}
