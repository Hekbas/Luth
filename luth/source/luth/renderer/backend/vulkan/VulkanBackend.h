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
    // FreeTag(N-2) heap reclaim, and the command buffers recorded each frame (per-view gA/compute/gB).
    // Polled non-blocking by the Game(N) | Render(N-1) | GPU(N-2) loop in App::Run.
    class VulkanBackend : public RenderBackend
    {
    public:
        virtual void Init(void* windowHandle) override;
        virtual void Shutdown() override;

        virtual bool AcquireImage(u64 frameIndex) override;
        virtual void SubmitView(u64 frameIndex, u32 viewSlot, QueueRecorders recorders,
                                bool hasComputeWork, bool isLastView) override;
        virtual void OnResize(u32 width, u32 height) override;

        // Accessors
        VkDevice GetDevice() const;
        VulkanSwapchain& GetSwapchain() { return *m_Swapchain; }

        // Command Pool Access for Parallel Recording. Graphics pool feeds passes that record secondary cmd buffers
        // from worker fibers during the RenderGraph's parallel record phase. Compute pool exists for future
        // fiber-recorded compute secondaries (forward-plus cluster build, gpu particles); currently unused as today's
        // compute passes record inline on the primary during serial execution.
        CommandAllocatorPool& GetCommandAllocatorPool(u32 frameIndex) { return *m_CommandAllocatorPools[frameIndex % MAX_FRAMES_IN_FLIGHT]; }
        CommandAllocatorPool& GetComputeCommandAllocatorPool(u32 frameIndex) { return *m_ComputeCommandAllocatorPools[frameIndex % MAX_FRAMES_IN_FLIGHT]; }

        // Per-view x per-frame primary cmd buffer access. viewSlot in [0, MAX_VIEWS_PER_FRAME). gA = graphics work
        // before first AsyncCompute pass; compute = AsyncCompute passes; gB = graphics work after. Each view
        // owns its own triplet so cross-queue semaphores at submit boundaries sequence view K -> view K+1 for
        // shared resources (m_ShadowMap, IBL maps). See arch/multi-queue.md.
        VkCommandBuffer GetGraphicsAPrimary(u64 frameIndex, u32 viewSlot) const { return m_GAPrimaries     [frameIndex % MAX_FRAMES_IN_FLIGHT][viewSlot]; }
        VkCommandBuffer GetComputePrimary  (u64 frameIndex, u32 viewSlot) const { return m_ComputePrimaries[frameIndex % MAX_FRAMES_IN_FLIGHT][viewSlot]; }
        VkCommandBuffer GetGraphicsBPrimary(u64 frameIndex, u32 viewSlot) const { return m_GBPrimaries     [frameIndex % MAX_FRAMES_IN_FLIGHT][viewSlot]; }

        // Non-blocking GPU completion check: frame pipelining + the AcquireImage reclaim sweep
        bool IsFrameComplete(u64 frameIndex);

    private:
        void CreateSyncObjects();
        void DestroySyncObjects();
        void CreateFrameCommandBuffers();
        void CreateComputeFrameCommandBuffers();

        std::unique_ptr<VulkanSwapchain> m_Swapchain;

        // Synchronization, per Vulkan swapchain semaphore reuse rules:
        // imageAvailable: ring of (imageCount + 1), cycled by frame, picked before acquire
        // renderFinished: one per swapchain image, indexed by acquired image index
        //   (safe because acquiring image N means presentation released its semaphore)
        std::vector<VkSemaphore> m_ImageAvailableSemaphores;
        std::vector<VkSemaphore> m_RenderFinishedSemaphores;
        u32 m_ImageAvailableSemCount = 0;
        u32 m_RenderFinishedSemCount = 0;
        u32 m_NextAcquireSemIndex = 0;      // cycles through imageAvailable ring
        u32 m_CurrentAcquireSemIndex = 0;    // which imageAvailable was used this frame
        u32 m_AcquiredImageIndex = 0;        // swapchain image index from last acquire

        // Timeline semaphores driving CPU<->GPU sync. m_FrameTimeline tracks graphics submits (one per gA + one per gB
        // per view); m_ComputeTimeline tracks async-compute submits (one per view with AsyncCompute work). Per-submit
        // monotonic: multiple submits per frame mean multiple timeline values per frame, with the LAST value cached
        // in the per-frame ring below so AcquireImage knows which value gates GPU-N-2 retirement.
        TimelineSemaphore m_FrameTimeline;
        TimelineSemaphore m_ComputeTimeline;

        // Per-frame ring caches: the final m_FrameTimeline value at end of frame F (last view's gB signal) and the
        // final m_ComputeTimeline value (last view's compute submit, or 0 sentinel when no compute work that frame).
        // AcquireImage waits on these specific values when retiring frame N-2; 0 sentinel skips the compute wait.
        std::array<u64, MAX_FRAMES_IN_FLIGHT> m_LastGraphicsValuePerFrame{};
        std::array<u64, MAX_FRAMES_IN_FLIGHT> m_LastComputeValuePerFrame {};

        // High-water mark: highest consuming-frame label whose tags are reclaimed (direct IsFrameComplete
        // sweep in AcquireImage). Frees each completed frame's tags exactly once. see arch/memory.md
        u64 m_LastReclaimedLabel = 0;

        // Command Allocator Pools (Per-Frame) for Workers: parallel rings per queue family. CommandAllocatorPool is
        // already parameterized by queueFamilyIndex; instantiation is the only differentiator.
        std::array<std::unique_ptr<CommandAllocatorPool>, MAX_FRAMES_IN_FLIGHT> m_CommandAllocatorPools;
        std::array<std::unique_ptr<CommandAllocatorPool>, MAX_FRAMES_IN_FLIGHT> m_ComputeCommandAllocatorPools;

        // Primary Command Buffers for Submission. Per-view x per-frame rings: outer index = frameSlot, inner =
        // viewSlot. Each ring holds MAX_VIEWS_PER_FRAME x MAX_FRAMES_IN_FLIGHT cmd buffers (= 12 today). gA + gB
        // share m_PrimaryCommandPool (graphics family); compute uses m_ComputePrimaryCommandPool (compute family).
        // m_LastSubmittedGraphicsValue / m_LastSubmittedComputeValue accumulate as views submit within a frame;
        // each submit signals the next monotonic value; AcquireImage's GPU-N-2 reclaim reads the final values
        // cached in the per-frame ring at end of last view (m_LastGraphicsValuePerFrame / m_LastComputeValuePerFrame).
        VkCommandPool m_PrimaryCommandPool        = VK_NULL_HANDLE;
        VkCommandPool m_ComputePrimaryCommandPool = VK_NULL_HANDLE;
        std::array<std::array<VkCommandBuffer, MAX_VIEWS_PER_FRAME>, MAX_FRAMES_IN_FLIGHT> m_GAPrimaries{};
        std::array<std::array<VkCommandBuffer, MAX_VIEWS_PER_FRAME>, MAX_FRAMES_IN_FLIGHT> m_ComputePrimaries{};
        std::array<std::array<VkCommandBuffer, MAX_VIEWS_PER_FRAME>, MAX_FRAMES_IN_FLIGHT> m_GBPrimaries{};
        u64 m_LastSubmittedGraphicsValue   = 0;  // Monotonic across frames; advances per graphics submit (gA + gB).
        u64 m_LastSubmittedComputeValue    = 0;  // Monotonic across frames; advances per compute submit.
        u64 m_CurrentFrameLastComputeValue = 0;  // Per-frame accumulator: reset on firstView, captured into ring on isLastView.

        u32 m_CurrentFrameIndex = 0;
    };
}
