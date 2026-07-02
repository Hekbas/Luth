#include "luthpch.h"
#include "VulkanBackend.h"
#include "VulkanContext.h"
#include "PipelineCache.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/TaggedPageAllocator.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

namespace Luth
{
    void VulkanBackend::Init(void* windowHandle)
    {
        LH_PROFILE_FUNCTION();

        VulkanContext::Init(windowHandle);
        PipelineCache::Init();
        m_Swapchain = std::make_unique<VulkanSwapchain>(windowHandle);
        m_Swapchain->Init();
        CreateSyncObjects();
        CreateFrameCommandBuffers();
        CreateComputeFrameCommandBuffers();

        // Command allocator pools: one ring per queue family. Compute pool feeds future fiber-recorded compute
        // secondaries; today's compute passes record inline so the pool sits idle. CommandAllocatorPool's ctor is
        // already parameterized by queueFamilyIndex; single-family GPUs alias compute family to graphics family
        // and the second pool becomes a duplicate over the same family (no Vulkan rule against this).
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            m_CommandAllocatorPools[i] = std::make_unique<CommandAllocatorPool>(VulkanContext::Get().GetGraphicsFamily());
            m_CommandAllocatorPools[i]->Init();
            m_ComputeCommandAllocatorPools[i] = std::make_unique<CommandAllocatorPool>(VulkanContext::Get().GetComputeFamily());
            m_ComputeCommandAllocatorPools[i]->Init();
        }

        // GPU half of the Onion/Garlic split; depends on VulkanContext + VulkanAllocator being live.
        Memory::GPUTaggedPageAllocator::Get().Init();
    }

    void VulkanBackend::Shutdown()
    {
        LH_PROFILE_FUNCTION();

        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());

        // Heap pages are device-mapped; shut them down while the device is still alive.
        Memory::GPUTaggedPageAllocator::Get().Shutdown();

        DestroySyncObjects();
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            m_CommandAllocatorPools[i]->Shutdown();
            m_ComputeCommandAllocatorPools[i]->Shutdown();
        }

        vkDestroyCommandPool(VulkanContext::Get().GetDevice(), m_PrimaryCommandPool, nullptr);
        vkDestroyCommandPool(VulkanContext::Get().GetDevice(), m_ComputePrimaryCommandPool, nullptr);

        m_ComputeTimeline.Shutdown();
        m_FrameTimeline.Shutdown();
        m_Swapchain.reset();
        PipelineCache::Shutdown();
        VulkanContext::Shutdown();
    }

    bool VulkanBackend::AcquireImage(u64 frameIndex)
    {
        LH_PROFILE_FUNCTION();

        m_CurrentFrameIndex = frameIndex % MAX_FRAMES_IN_FLIGHT;

        // Update Context Frame Index for Deletion Queue
        VulkanContext::Get().SetCurrentFrameIndex(m_CurrentFrameIndex);
        VulkanContext::Get().GetResourceCache().NewFrame(); // Tick Cache

        // Wait for GPU to finish with this frame's resources from previous cycle.
        // Per-view 3-submit means m_FrameTimeline is signaled twice per view (gA + gB) and m_ComputeTimeline once
        // per view-with-compute; both no longer equal frameIndex+1. The per-frame ring caches the LAST value
        // of each timeline at end of the previous frame N-2; AcquireImage waits on exactly those.
        if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
        {
            // +1: per-frame UAB descriptor slots are read at renderFrameIndex%N, one frame ahead of the
            // cmd-buffer slot (gameFrameIndex%N) the cmd-buffer reset gates. Wait the slot's prior DESCRIPTOR
            // reader (frame N-3), not just its cmd-buffer prior user (N-4), so a game-stage slot rewrite can't
            // race an older in-flight reader under GPU-behind load (skinned-pose ghost). Monotone, so it still
            // covers cmd-buffer reset. see arch/multi-queue.md
            const u32 retiringSlot = (u32)((frameIndex - MAX_FRAMES_IN_FLIGHT + 1) % MAX_FRAMES_IN_FLIGHT);
            const u64 gfxWait     = m_LastGraphicsValuePerFrame[retiringSlot];
            const u64 computeWait = m_LastComputeValuePerFrame [retiringSlot];

            m_FrameTimeline.Wait(gfxWait);
            if (computeWait > 0) m_ComputeTimeline.Wait(computeWait);

            // Direct ND reclaim (HasFrameCompleted): the submit labeled L consumed all data tagged L-1, so
            // free tag (label-1) for each consuming frame `label` that is GPU-complete. Bound at frameIndex-1
            // (frame `frameIndex` hasn't submitted; stale cache slot). The block-wait above guarantees label
            // (frameIndex - MAX_FRAMES_IN_FLIGHT + 1) is complete, so this never frees less than the legacy
            // FreeTag(frameIndex-4); a faster GPU retires newer labels too. see arch/memory.md
            for (u64 label = m_LastReclaimedLabel + 1; label + 1 <= frameIndex; ++label)
            {
                if (!IsFrameComplete(label)) break;  // labels complete in order; stop at the first that isn't
                const u32 freeTag = static_cast<u32>(label - 1);
                Memory::TaggedPageAllocator   ::Get().FreeTag(freeTag);
                Memory::GPUTaggedPageAllocator::Get().FreeTag(freeTag);
                m_LastReclaimedLabel = label;
            }
        }

        // Flush deletions AFTER the GPU is done with this frame's resources
        VulkanContext::Get().FlushDeletionQueue();

        // Reset Command Allocator Pools for THIS frame, both queue families.
        m_CommandAllocatorPools[m_CurrentFrameIndex]->ResetAll();
        m_ComputeCommandAllocatorPools[m_CurrentFrameIndex]->ResetAll();

        // Reset every per-view primary cmd buffer for THIS frame across all three queue streams. Views that don't
        // render this frame leave their cmd buffers untouched (still get reset for cleanliness); compute and gB
        // primaries are reset every frame even when no pass routes there; recording empty is valid Vulkan.
        for (u32 v = 0; v < MAX_VIEWS_PER_FRAME; ++v)
        {
            vkResetCommandBuffer(m_GAPrimaries     [m_CurrentFrameIndex][v], 0);
            vkResetCommandBuffer(m_ComputePrimaries[m_CurrentFrameIndex][v], 0);
            vkResetCommandBuffer(m_GBPrimaries     [m_CurrentFrameIndex][v], 0);
        }

        // Update JobSystem Context
        JobSystem::SetGlobalCommandPool(m_CommandAllocatorPools[m_CurrentFrameIndex].get());

        // Pick next imageAvailable semaphore from the ring (we don't know the image index yet)
        m_CurrentAcquireSemIndex = m_NextAcquireSemIndex;
        m_NextAcquireSemIndex = (m_NextAcquireSemIndex + 1) % m_ImageAvailableSemCount;

        u32 imageIndex = m_Swapchain->AcquireNextImage(m_ImageAvailableSemaphores[m_CurrentAcquireSemIndex]);
        m_AcquiredImageIndex = imageIndex;

        // Same frameIndex retries next iteration (App skips before Advance), so SubmitFrame still signals frameIndex+1.
        if (imageIndex == UINT32_MAX) return false;
        return true;
    }

    void VulkanBackend::SubmitView(u64 frameIndex, u32 viewSlot, QueueRecorders recorders,
                                   bool hasComputeWork, bool isLastView)
    {
        LH_PROFILE_FUNCTION();

        if (m_AcquiredImageIndex == UINT32_MAX) return;
        LH_CORE_ASSERT(viewSlot < MAX_VIEWS_PER_FRAME, "viewSlot exceeds MAX_VIEWS_PER_FRAME");

        const bool firstView = (viewSlot == 0);
        if (firstView) m_CurrentFrameLastComputeValue = 0;

        // ---- graphics-A submit ----
        // Subsequent views wait the previous view's gB at EARLY_FRAGMENT_TESTS (inter-view depth ordering). First view
        // waits the PREVIOUS frame's compute at ALL_COMMANDS: a persistent resource (shadow map) written here by ShadowPass
        // must not race the previous frame's async-compute still reading it. imageAvailable is waited on the last gB now.
        // see arch/multi-queue.md
        VkSemaphoreSubmitInfo gaWait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        u32 gaWaitCount = 0;
        if (!firstView)
        {
            gaWait.semaphore = m_FrameTimeline.GetHandle();
            gaWait.value     = m_LastSubmittedGraphicsValue;
            gaWait.stageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
            gaWaitCount = 1;
        }
        else if (frameIndex > 0)
        {
            const u32 prevSlot    = (u32)((frameIndex - 1) % MAX_FRAMES_IN_FLIGHT);
            const u64 prevCompute = m_LastComputeValuePerFrame[prevSlot];
            if (prevCompute > 0)
            {
                gaWait.semaphore = m_ComputeTimeline.GetHandle();
                gaWait.value     = prevCompute;
                gaWait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                gaWaitCount = 1;
            }
        }

        const u64 gaSignalValue = ++m_LastSubmittedGraphicsValue;
        VkSemaphoreSubmitInfo gaSignal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        gaSignal.semaphore = m_FrameTimeline.GetHandle();
        gaSignal.value     = gaSignalValue;
        gaSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo gaCmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        gaCmdInfo.commandBuffer = recorders.gA;

        VkSubmitInfo2 gaInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        gaInfo.waitSemaphoreInfoCount   = gaWaitCount;
        gaInfo.pWaitSemaphoreInfos      = &gaWait;
        gaInfo.commandBufferInfoCount   = 1;
        gaInfo.pCommandBufferInfos      = &gaCmdInfo;
        gaInfo.signalSemaphoreInfoCount = 1;
        gaInfo.pSignalSemaphoreInfos    = &gaSignal;
        if (!VulkanContext::Get().SubmitGraphics2(gaInfo, VK_NULL_HANDLE))
            LH_LOG(Renderer, error, "VulkanBackend::SubmitView - graphics-A submit failed (frame {}, view {}).", frameIndex, viewSlot);

        // ---- async-compute submit ----
        // Only fires when the view's RG routed any pass to AsyncCompute. Compute waits the gA value at ALL_COMMANDS
        // (not COMPUTE_SHADER): gates the reader's TOP_OF_PIPE-src cross-queue layout transition of gA outputs. see arch/multi-queue.md
        u64 computeSignalValue = 0;
        if (hasComputeWork)
        {
            // First view also waits the PREVIOUS frame's last compute: frame K's in-place BLAS refit must
            // not overlap frame K-1's traceRays still reading that BLAS on compute. Later views chain via
            // gA -> prev gB -> prev compute. see arch/multi-queue.md
            VkSemaphoreSubmitInfo cWaits[2]{};
            u32 cWaitCount = 0;
            cWaits[cWaitCount].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            cWaits[cWaitCount].semaphore = m_FrameTimeline.GetHandle();
            cWaits[cWaitCount].value     = gaSignalValue;
            cWaits[cWaitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ++cWaitCount;

            if (firstView && frameIndex > 0)
            {
                const u32 prevSlot   = (u32)((frameIndex - 1) % MAX_FRAMES_IN_FLIGHT);
                const u64 prevValue  = m_LastComputeValuePerFrame[prevSlot];
                if (prevValue > 0)
                {
                    cWaits[cWaitCount].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                    cWaits[cWaitCount].semaphore = m_ComputeTimeline.GetHandle();
                    cWaits[cWaitCount].value     = prevValue;
                    cWaits[cWaitCount].stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                                 | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                    ++cWaitCount;
                }
            }

            computeSignalValue = ++m_LastSubmittedComputeValue;
            VkSemaphoreSubmitInfo cSignal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
            cSignal.semaphore = m_ComputeTimeline.GetHandle();
            cSignal.value     = computeSignalValue;
            cSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

            VkCommandBufferSubmitInfo cCmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
            cCmdInfo.commandBuffer = recorders.compute;

            VkSubmitInfo2 cInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
            cInfo.waitSemaphoreInfoCount   = cWaitCount;
            cInfo.pWaitSemaphoreInfos      = cWaits;
            cInfo.commandBufferInfoCount   = 1;
            cInfo.pCommandBufferInfos      = &cCmdInfo;
            cInfo.signalSemaphoreInfoCount = 1;
            cInfo.pSignalSemaphoreInfos    = &cSignal;
            if (!VulkanContext::Get().SubmitCompute2(cInfo, VK_NULL_HANDLE))
                LH_LOG(Renderer, error, "VulkanBackend::SubmitView - compute submit failed (frame {}, view {}).", frameIndex, viewSlot);
            m_CurrentFrameLastComputeValue = computeSignalValue;
        }

        // ---- graphics-B submit ----
        // hasComputeWork: wait the compute signal at ALL_COMMANDS; gates gB's TOP_OF_PIPE-src cross-queue layout
        // transition of compute outputs (a narrower FRAGMENT_SHADER wait would not). see arch/multi-queue.md
        // !hasComputeWork: wait the just-signaled gA value at ALL_GRAPHICS (same primary's prior submit, intra-queue).
        VkSemaphoreSubmitInfo gbWaits[2]{};
        gbWaits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        if (hasComputeWork)
        {
            gbWaits[0].semaphore = m_ComputeTimeline.GetHandle();
            gbWaits[0].value     = computeSignalValue;
            gbWaits[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        else
        {
            gbWaits[0].semaphore = m_FrameTimeline.GetHandle();
            gbWaits[0].value     = gaSignalValue;
            gbWaits[0].stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        }
        u32 gbWaitCount = 1;

        // Last view runs ImGuiPass (only swapchain writer): acquire wait at ALL_COMMANDS, since the UNDEFINED->COLOR
        // transition is at TOP_OF_PIPE (COLOR_OUTPUT wouldn't gate it).
        if (isLastView)
        {
            gbWaits[1].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            gbWaits[1].semaphore = m_ImageAvailableSemaphores[m_CurrentAcquireSemIndex];
            gbWaits[1].value     = 0;
            gbWaits[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            gbWaitCount = 2;
        }

        const u64 gbSignalValue = ++m_LastSubmittedGraphicsValue;
        VkSemaphoreSubmitInfo gbSignals[2]{};
        gbSignals[0].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        gbSignals[0].semaphore = m_FrameTimeline.GetHandle();
        gbSignals[0].value     = gbSignalValue;
        gbSignals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        // Last view of the frame signals renderFinished too (the binary sem the swapchain Present waits on).
        u32 gbSignalCount = 1;
        if (isLastView)
        {
            gbSignals[1].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            gbSignals[1].semaphore = m_RenderFinishedSemaphores[m_AcquiredImageIndex];
            gbSignals[1].value     = 0;
            gbSignals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
            gbSignalCount = 2;
        }

        VkCommandBufferSubmitInfo gbCmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        gbCmdInfo.commandBuffer = recorders.gB;

        VkSubmitInfo2 gbInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        gbInfo.waitSemaphoreInfoCount   = gbWaitCount;
        gbInfo.pWaitSemaphoreInfos      = gbWaits;
        gbInfo.commandBufferInfoCount   = 1;
        gbInfo.pCommandBufferInfos      = &gbCmdInfo;
        gbInfo.signalSemaphoreInfoCount = gbSignalCount;
        gbInfo.pSignalSemaphoreInfos    = gbSignals;
        if (!VulkanContext::Get().SubmitGraphics2(gbInfo, VK_NULL_HANDLE))
            LH_LOG(Renderer, error, "VulkanBackend::SubmitView - graphics-B submit failed (frame {}, view {}).", frameIndex, viewSlot);

        // Cache per-frame final timeline values + present on the last view. AcquireImage reads these caches when
        // gating GPU-N-2 page reclaim (skips compute wait when the per-frame value is 0).
        if (isLastView)
        {
            const u32 frameSlot = (u32)(frameIndex % MAX_FRAMES_IN_FLIGHT);
            m_LastGraphicsValuePerFrame[frameSlot] = gbSignalValue;
            m_LastComputeValuePerFrame [frameSlot] = m_CurrentFrameLastComputeValue;  // 0 sentinel = no compute that frame
            m_Swapchain->Present(m_RenderFinishedSemaphores[m_AcquiredImageIndex]);
        }
    }

    void VulkanBackend::OnResize(u32 width, u32 height)
    {
        LH_PROFILE_FUNCTION();

        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
        m_Swapchain->Recreate(width, height);

        // Recreate semaphores if swapchain image count changed
        u32 newImageCount = m_Swapchain->GetImageCount();
        if (newImageCount != m_RenderFinishedSemCount)
        {
            DestroySyncObjects();
            CreateSyncObjects();
        }
    }

    VkDevice VulkanBackend::GetDevice() const
    {
        return VulkanContext::Get().GetDevice();
    }

    void VulkanBackend::CreateSyncObjects()
    {
        LH_PROFILE_FUNCTION();

        u32 imageCount = m_Swapchain->GetImageCount();

        // imageAvailable: ring of (imageCount + 1) semaphores.
        // At most imageCount can be held by the presentation engine (one per presented image),
        // so imageCount + 1 guarantees at least one is always free for the next acquire.
        m_ImageAvailableSemCount = imageCount + 1;
        m_ImageAvailableSemaphores.resize(m_ImageAvailableSemCount);

        // renderFinished: one per swapchain image, indexed by acquired image index.
        // When vkAcquireNextImageKHR returns image N, the presentation engine has released
        // image N's renderFinished semaphore, so it's safe to signal it again.
        m_RenderFinishedSemCount = imageCount;
        m_RenderFinishedSemaphores.resize(m_RenderFinishedSemCount);

        m_FrameTimeline.Init(0);
        m_ComputeTimeline.Init(0);
        m_NextAcquireSemIndex = 0;

        // Timelines just reset to 0 (resize rebuild); clear the per-frame value caches + submit counters so
        // AcquireImage doesn't block-wait or reclaim against a stale pre-resize value. see arch/multi-queue.md
        m_LastGraphicsValuePerFrame.fill(0);
        m_LastComputeValuePerFrame.fill(0);
        m_LastSubmittedGraphicsValue   = 0;
        m_LastSubmittedComputeValue    = 0;
        m_CurrentFrameLastComputeValue = 0;

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        for (u32 i = 0; i < m_ImageAvailableSemCount; i++) {
            vkCreateSemaphore(VulkanContext::Get().GetDevice(), &semaphoreInfo, nullptr, &m_ImageAvailableSemaphores[i]);
        }
        for (u32 i = 0; i < m_RenderFinishedSemCount; i++) {
            vkCreateSemaphore(VulkanContext::Get().GetDevice(), &semaphoreInfo, nullptr, &m_RenderFinishedSemaphores[i]);
        }
    }

    void VulkanBackend::DestroySyncObjects()
    {
        LH_PROFILE_FUNCTION();

        VkDevice device = VulkanContext::Get().GetDevice();
        for (u32 i = 0; i < m_ImageAvailableSemCount; i++)
            vkDestroySemaphore(device, m_ImageAvailableSemaphores[i], nullptr);
        for (u32 i = 0; i < m_RenderFinishedSemCount; i++)
            vkDestroySemaphore(device, m_RenderFinishedSemaphores[i], nullptr);
        m_ImageAvailableSemaphores.clear();
        m_RenderFinishedSemaphores.clear();
        m_ImageAvailableSemCount = 0;
        m_RenderFinishedSemCount = 0;
    }

    void VulkanBackend::CreateFrameCommandBuffers()
    {
        LH_PROFILE_FUNCTION();

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = VulkanContext::Get().GetGraphicsFamily();

        if (vkCreateCommandPool(VulkanContext::Get().GetDevice(), &poolInfo, nullptr, &m_PrimaryCommandPool) != VK_SUCCESS) {
            LH_LOG(Renderer, critical, "Failed to create primary command pool!");
        }

        // Allocate gA + gB per-view rings from the graphics pool. Total = MAX_VIEWS_PER_FRAME x MAX_FRAMES_IN_FLIGHT
        // primaries per ring (= 12 at current constants). Single allocate call per ring, then strided assignment
        // into the 2D arrays; Vulkan returns the buffers in alloc-order, mapped view-major within each frame.
        constexpr u32 perRing = MAX_VIEWS_PER_FRAME * MAX_FRAMES_IN_FLIGHT;
        std::array<VkCommandBuffer, perRing> gaFlat{};
        std::array<VkCommandBuffer, perRing> gbFlat{};

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool        = m_PrimaryCommandPool;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = perRing;
        if (vkAllocateCommandBuffers(VulkanContext::Get().GetDevice(), &allocInfo, gaFlat.data()) != VK_SUCCESS)
            LH_LOG(Renderer, critical, "Failed to allocate graphics-A per-view primary command buffers!");
        if (vkAllocateCommandBuffers(VulkanContext::Get().GetDevice(), &allocInfo, gbFlat.data()) != VK_SUCCESS)
            LH_LOG(Renderer, critical, "Failed to allocate graphics-B per-view primary command buffers!");

        for (u32 f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f)
        for (u32 v = 0; v < MAX_VIEWS_PER_FRAME;  ++v)
        {
            m_GAPrimaries[f][v] = gaFlat[f * MAX_VIEWS_PER_FRAME + v];
            m_GBPrimaries[f][v] = gbFlat[f * MAX_VIEWS_PER_FRAME + v];
        }
    }

    void VulkanBackend::CreateComputeFrameCommandBuffers()
    {
        LH_PROFILE_FUNCTION();

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = VulkanContext::Get().GetComputeFamily();

        if (vkCreateCommandPool(VulkanContext::Get().GetDevice(), &poolInfo, nullptr, &m_ComputePrimaryCommandPool) != VK_SUCCESS) {
            LH_LOG(Renderer, critical, "Failed to create compute primary command pool!");
        }

        constexpr u32 perRing = MAX_VIEWS_PER_FRAME * MAX_FRAMES_IN_FLIGHT;
        std::array<VkCommandBuffer, perRing> flat{};
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool        = m_ComputePrimaryCommandPool;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = perRing;
        if (vkAllocateCommandBuffers(VulkanContext::Get().GetDevice(), &allocInfo, flat.data()) != VK_SUCCESS)
            LH_LOG(Renderer, critical, "Failed to allocate compute per-view primary command buffers!");

        for (u32 f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f)
        for (u32 v = 0; v < MAX_VIEWS_PER_FRAME;  ++v)
            m_ComputePrimaries[f][v] = flat[f * MAX_VIEWS_PER_FRAME + v];
    }

    bool VulkanBackend::IsFrameComplete(u64 frameIndex)
    {
        // Non-blocking GPU completion check: did the last gB signal of frame F land yet? Per-frame ring is set in
        // SubmitView when isLastView fires. Value 0 means SubmitView never ran for that frame (still in flight).
        const u32 frameSlot = (u32)(frameIndex % MAX_FRAMES_IN_FLIGHT);
        const u64 expected  = m_LastGraphicsValuePerFrame[frameSlot];
        if (expected == 0) return false;
        if (m_FrameTimeline.GetValue() < expected) return false;
        const u64 expectedCompute = m_LastComputeValuePerFrame[frameSlot];
        if (expectedCompute > 0 && m_ComputeTimeline.GetValue() < expectedCompute) return false;
        return true;
    }
}
