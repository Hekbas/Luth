#pragma once

#include "luth/renderer/RendererAPI.h"
#include "VulkanSwapchain.h"
#include "TimelineSemaphore.h"
#include "CommandAllocatorPool.h"
#include <vulkan/vulkan.h>
#include <array>

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
        
        // Accessor for Parallel Recording (Returns pool for CURRENT frame)
        CommandAllocatorPool& GetCommandAllocatorPool() { return *m_CommandAllocatorPools[m_CurrentFrame]; }

    private:
        void CreateSyncObjects();
        void CreateCommandBuffers();

        std::unique_ptr<VulkanSwapchain> m_Swapchain;
        
        // Frame Synchronization
        static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
        u32 m_CurrentFrame = 0;
        
        VkCommandPool m_CommandPool;
        std::vector<VkCommandBuffer> m_CommandBuffers;
        
        // Parallel Command Recording (Per-Frame Pools)
        std::array<std::unique_ptr<CommandAllocatorPool>, MAX_FRAMES_IN_FLIGHT> m_CommandAllocatorPools;
        
        std::vector<VkSemaphore> m_ImageAvailableSemaphores;
        std::vector<VkSemaphore> m_RenderFinishedSemaphores;
        
        // Timeline Semaphore for Frame Synchronization
        TimelineSemaphore m_FrameTimeline;
        u64 m_FrameValues[MAX_FRAMES_IN_FLIGHT] = {0};
        u64 m_CurrentFrameValue = 0;

        bool m_FramebufferResized = false;
    };
}
