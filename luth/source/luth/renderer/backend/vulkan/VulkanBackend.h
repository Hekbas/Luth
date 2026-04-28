#pragma once

#include "luth/renderer/RenderBackend.h"
#include "luth/core/FrameData.h"
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

        virtual bool AcquireImage(u64 frameIndex) override;
        virtual void SubmitFrame(u64 frameIndex, void* commandBuffer) override;
        virtual void* GetFrameCommandBuffer(u64 frameIndex) override;
        virtual void OnResize(u32 width, u32 height) override;

        // Accessors
        VkDevice GetDevice() const;
        VulkanSwapchain& GetSwapchain() { return *m_Swapchain; }

        // Command Pool Access for Parallel Recording
        CommandAllocatorPool& GetCommandAllocatorPool(u32 frameIndex) { return *m_CommandAllocatorPools[frameIndex % MAX_FRAMES_IN_FLIGHT]; }

        // Non-blocking GPU completion check for frame pipelining
        bool IsFrameComplete(u64 frameIndex);

    private:
        void CreateSyncObjects();
        void DestroySyncObjects();
        void CreateFrameCommandBuffers();

        std::unique_ptr<VulkanSwapchain> m_Swapchain;

        // Synchronization — per Vulkan swapchain semaphore reuse rules:
        // imageAvailable: ring of (imageCount + 1) — cycled by frame, picked before acquire
        // renderFinished: one per swapchain image — indexed by acquired image index
        //   (safe because acquiring image N means presentation released its semaphore)
        std::vector<VkSemaphore> m_ImageAvailableSemaphores;
        std::vector<VkSemaphore> m_RenderFinishedSemaphores;
        u32 m_ImageAvailableSemCount = 0;
        u32 m_RenderFinishedSemCount = 0;
        u32 m_NextAcquireSemIndex = 0;      // cycles through imageAvailable ring
        u32 m_CurrentAcquireSemIndex = 0;    // which imageAvailable was used this frame
        u32 m_AcquiredImageIndex = 0;        // swapchain image index from last acquire

        // Timeline Semaphore for CPU-GPU Sync
        TimelineSemaphore m_FrameTimeline;

        // Command Allocator Pools (Per-Frame) for Workers
        std::array<std::unique_ptr<CommandAllocatorPool>, MAX_FRAMES_IN_FLIGHT> m_CommandAllocatorPools;

        // Primary Command Buffers for Submission (Owned by Backend)
        VkCommandPool m_PrimaryCommandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_PrimaryCommandBuffers;

        u32 m_CurrentFrameIndex = 0;
    };
}
