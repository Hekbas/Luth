#pragma once

#include "luth/renderer/RendererAPI.h"
#include "VulkanSwapchain.h"
#include "TimelineSemaphore.h"
#include <vulkan/vulkan.h>

namespace Luth
{
    class VKRendererAPI : public RendererAPI
    {
    public:
        virtual void Init(void* windowHandle) override;
        virtual void Shutdown() override;

        virtual bool BeginFrame() override;
        virtual void EndFrame() override;
        virtual void ExecuteGraph(RG::RenderGraph& graph) override;
        virtual void OnResize(u32 width, u32 height) override;

        // Vulkan Specific Accessors
        VulkanSwapchain& GetSwapchain() { return *m_Swapchain; }
        VkCommandBuffer GetCurrentCommandBuffer() { return m_CommandBuffers[m_CurrentFrame]; }
        u32 GetCurrentFrameIndex() const { return m_CurrentFrame; }

    private:
        void CreateSyncObjects();
        void CreateCommandBuffers();

        std::unique_ptr<VulkanSwapchain> m_Swapchain;
        
        // Frame Synchronization
        static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
        u32 m_CurrentFrame = 0;
        
        VkCommandPool m_CommandPool;
        std::vector<VkCommandBuffer> m_CommandBuffers;
        
        std::vector<VkSemaphore> m_ImageAvailableSemaphores;
        std::vector<VkSemaphore> m_RenderFinishedSemaphores;
        
        // Timeline Semaphore for Frame Synchronization
        // Replaces VkFence m_InFlightFences
        TimelineSemaphore m_FrameTimeline;
        u64 m_FrameValues[MAX_FRAMES_IN_FLIGHT] = {0};
        u64 m_CurrentFrameValue = 0;

        bool m_FramebufferResized = false;
    };
}
