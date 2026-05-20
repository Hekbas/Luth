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
    // Concrete RenderBackend implementation. Owns the swapchain, the per-frame ring of
    // CommandAllocatorPools (MAX_FRAMES_IN_FLIGHT), the timeline semaphore that drives the V6
    // FreeTag(N-2) heap reclaim, and the primary command buffer for each frame's submit.
    // Polled non-blocking by the Game(N) | Render(N-1) | GPU(N-2) loop in App::Run.
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

        // Command Pool Access for Parallel Recording. Graphics pool feeds passes that record secondary cmd buffers
        // from worker fibers (RenderGraph Phase 1). Compute pool exists for future fiber-recorded compute secondaries
        // (forward-plus cluster build, gpu particles); currently unused as today's compute passes record inline on the
        // primary in Phase 2.
        CommandAllocatorPool& GetCommandAllocatorPool(u32 frameIndex) { return *m_CommandAllocatorPools[frameIndex % MAX_FRAMES_IN_FLIGHT]; }
        CommandAllocatorPool& GetComputeCommandAllocatorPool(u32 frameIndex) { return *m_ComputeCommandAllocatorPools[frameIndex % MAX_FRAMES_IN_FLIGHT]; }

        // Per-queue primary cmd buffer access. gA = graphics work before first AsyncCompute pass; compute =
        // AsyncCompute passes; gB = graphics work after. See arch/multi-queue.md submit topology.
        VkCommandBuffer GetGraphicsAFrameCommandBuffer(u64 frameIndex) const { return m_PrimaryCommandBuffers[frameIndex % MAX_FRAMES_IN_FLIGHT]; }
        VkCommandBuffer GetComputeFrameCommandBuffer  (u64 frameIndex) const { return m_ComputePrimaryCommandBuffers[frameIndex % MAX_FRAMES_IN_FLIGHT]; }
        VkCommandBuffer GetGraphicsBFrameCommandBuffer(u64 frameIndex) const { return m_GraphicsBPrimaryCommandBuffers[frameIndex % MAX_FRAMES_IN_FLIGHT]; }

        // Non-blocking GPU completion check for frame pipelining
        bool IsFrameComplete(u64 frameIndex);

    private:
        void CreateSyncObjects();
        void DestroySyncObjects();
        void CreateFrameCommandBuffers();
        void CreateComputeFrameCommandBuffers();

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

        // Timeline semaphores driving CPU↔GPU sync. m_FrameTimeline tracks graphics submits (one per gA + one per gB
        // per view); m_ComputeTimeline tracks async-compute submits (one per view with AsyncCompute work). Per-submit
        // monotonic — multiple submits per frame mean multiple timeline values per frame, with the LAST value cached
        // in the per-frame ring below so AcquireImage knows which value gates GPU-N-2 retirement.
        TimelineSemaphore m_FrameTimeline;
        TimelineSemaphore m_ComputeTimeline;

        // Per-frame ring caches: the final m_FrameTimeline value at end of frame F (last view's gB signal) and the
        // final m_ComputeTimeline value (last view's compute submit, or 0 sentinel when no compute work that frame).
        // AcquireImage waits on these specific values when retiring frame N-2; 0 sentinel skips the compute wait.
        std::array<u64, MAX_FRAMES_IN_FLIGHT> m_LastGraphicsValuePerFrame{};
        std::array<u64, MAX_FRAMES_IN_FLIGHT> m_LastComputeValuePerFrame {};

        // Command Allocator Pools (Per-Frame) for Workers — parallel rings per queue family. CommandAllocatorPool is
        // already parameterized by queueFamilyIndex; instantiation is the only differentiator.
        std::array<std::unique_ptr<CommandAllocatorPool>, MAX_FRAMES_IN_FLIGHT> m_CommandAllocatorPools;
        std::array<std::unique_ptr<CommandAllocatorPool>, MAX_FRAMES_IN_FLIGHT> m_ComputeCommandAllocatorPools;

        // Primary Command Buffers for Submission (Owned by Backend). One ring per queue stream:
        //   m_PrimaryCommandBuffers           = graphics-A (pre-AsyncCompute work)
        //   m_GraphicsBPrimaryCommandBuffers  = graphics-B (post-AsyncCompute work)
        //   m_ComputePrimaryCommandBuffers    = async-compute work
        // gA + gB share m_PrimaryCommandPool (graphics family); compute uses m_ComputePrimaryCommandPool (compute family).
        VkCommandPool m_PrimaryCommandPool        = VK_NULL_HANDLE;
        VkCommandPool m_ComputePrimaryCommandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_PrimaryCommandBuffers;
        std::vector<VkCommandBuffer> m_GraphicsBPrimaryCommandBuffers;
        std::vector<VkCommandBuffer> m_ComputePrimaryCommandBuffers;

        u32 m_CurrentFrameIndex = 0;
    };
}
