#pragma once

#include "luth/renderer/RenderBackend.h"
#include "VulkanSwapchain.h"
#include "TimelineSemaphore.h"
#include "CommandAllocatorPool.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <array>

namespace Luth
{
    class VulkanBackend : public RenderBackend
    {
    public:
        virtual void Init(void* windowHandle) override;
        virtual void Shutdown() override;

        virtual u32 AcquireImage(u64 frameIndex) override;
        virtual void SubmitFrame(u64 frameIndex, void* commandBuffer) override;
        virtual void* GetFrameCommandBuffer(u64 frameIndex) override;
        virtual void OnResize(u32 width, u32 height) override;

        // Accessors
        VkDevice GetDevice() const;
        VulkanSwapchain& GetSwapchain() { return *m_Swapchain; }
        
        // Command Pool Access for Parallel Recording
        CommandAllocatorPool& GetCommandAllocatorPool(u32 frameIndex) { return *m_CommandAllocatorPools[frameIndex % MAX_FRAMES_IN_FLIGHT]; }

    private:
        void CreateSyncObjects();
        void CreateFrameCommandBuffers();

        std::unique_ptr<VulkanSwapchain> m_Swapchain;
        
        // Synchronization
        static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
        std::vector<VkSemaphore> m_ImageAvailableSemaphores;
        std::vector<VkSemaphore> m_RenderFinishedSemaphores;
        
        // Timeline Semaphore for CPU-GPU Sync
        TimelineSemaphore m_FrameTimeline;
        
        // Command Allocator Pools (Per-Frame) for Workers
        std::array<std::unique_ptr<CommandAllocatorPool>, MAX_FRAMES_IN_FLIGHT> m_CommandAllocatorPools;
        
        // Primary Command Buffers for Submission (Owned by Backend)
        VkCommandPool m_PrimaryCommandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_PrimaryCommandBuffers;
        
        u32 m_CurrentFrameIndex = 0; // 0 or 1
    };
}
