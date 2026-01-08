#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/graphics/GfxContext.h"
#include "luth/graphics/GfxSwapchain.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/vulkan/VKRenderGraphExecutor.h" // Reuse for now, will refactor later

#include <memory>
#include <vector>

namespace Luth::Gfx
{
    class GfxRenderer
    {
    public:
        static void Init(void* windowHandle, u32 width, u32 height);
        static void Shutdown();
        static void Resize(u32 width, u32 height);

        static void BeginFrame();
        static void EndFrame();

        static void ExecuteGraph(RG::RenderGraph& graph);

        static u32 GetCurrentFrameIndex() { return s_CurrentFrameIndex; }

    private:
        static void CreateCommandBuffers();
        static void CreateSyncObjects();

        static std::unique_ptr<GfxSwapchain> s_Swapchain;
        static std::unique_ptr<VKRenderGraphExecutor> s_GraphExecutor; // Reuse old executor for now

        static VkCommandPool s_CommandPool;
        static std::vector<VkCommandBuffer> s_CommandBuffers;

        // Sync
        static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
        struct FrameData
        {
            VkSemaphore imageAvailable;
            VkSemaphore renderFinished;
            VkFence inFlightFence;
        };
        static std::vector<FrameData> s_Frames;
        static u32 s_CurrentFrameIndex;
        static u32 s_CurrentImageIndex;
    };
}
