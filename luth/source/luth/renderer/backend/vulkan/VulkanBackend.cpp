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
        VulkanContext::Init(windowHandle);
        PipelineCache::Init();
        m_Swapchain = std::make_unique<VulkanSwapchain>(windowHandle);
        m_Swapchain->Init();
        CreateSyncObjects();
        CreateFrameCommandBuffers();

        // Init Command Allocator Pools
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            m_CommandAllocatorPools[i] = std::make_unique<CommandAllocatorPool>(VulkanContext::Get().GetGraphicsFamily());
            m_CommandAllocatorPools[i]->Init();
        }

        // GPU half of the Onion/Garlic split — depends on VulkanContext + VulkanAllocator being live.
        Memory::GPUTaggedPageAllocator::Get().Init();
    }

    void VulkanBackend::Shutdown()
    {
        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());

        // Heap pages are device-mapped — shut them down while the device is still alive.
        Memory::GPUTaggedPageAllocator::Get().Shutdown();

        DestroySyncObjects();
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            m_CommandAllocatorPools[i]->Shutdown();
        }

        vkDestroyCommandPool(VulkanContext::Get().GetDevice(), m_PrimaryCommandPool, nullptr);

        m_FrameTimeline.Shutdown();
        m_Swapchain.reset();
        PipelineCache::Shutdown();
        VulkanContext::Shutdown();
    }

    bool VulkanBackend::AcquireImage(u64 frameIndex)
    {
        m_CurrentFrameIndex = frameIndex % MAX_FRAMES_IN_FLIGHT;

        // Update Context Frame Index for Deletion Queue
        VulkanContext::Get().SetCurrentFrameIndex(m_CurrentFrameIndex);
        VulkanContext::Get().GetResourceCache().NewFrame(); // Tick Cache

        // Wait for GPU to finish with this frame's resources from previous cycle
        if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
        {
            u64 waitValue = frameIndex - MAX_FRAMES_IN_FLIGHT + 1;
            m_FrameTimeline.Wait(waitValue);

            // V6 driver: GPU has retired frame N-2; reclaim its tagged pages on
            // both halves of the Onion/Garlic split.
            // See arch/fiber-system.md V6 + arch/memory.md.
            const u32 finishedTag = static_cast<u32>(waitValue);
            Memory::TaggedPageAllocator::Get().FreeTag(finishedTag);
            Memory::GPUTaggedPageAllocator::Get().FreeTag(finishedTag);
        }

        // Flush deletions AFTER we know the GPU is done with this frame's resources
        VulkanContext::Get().FlushDeletionQueue();

        // Reset Command Allocator Pool for THIS frame
        m_CommandAllocatorPools[m_CurrentFrameIndex]->ResetAll();

        // Reset Primary Command Buffer for THIS frame
        vkResetCommandBuffer(m_PrimaryCommandBuffers[m_CurrentFrameIndex], 0);

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

    void* VulkanBackend::GetFrameCommandBuffer(u64 frameIndex)
    {
        u32 index = frameIndex % MAX_FRAMES_IN_FLIGHT;
        return m_PrimaryCommandBuffers[index];
    }

    void VulkanBackend::SubmitFrame(u64 frameIndex, void* commandBuffer)
    {
        if (m_AcquiredImageIndex == UINT32_MAX) return;

        VkCommandBuffer cmd = (VkCommandBuffer)commandBuffer;

        VkSemaphoreSubmitInfo waitSem{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        waitSem.semaphore = m_ImageAvailableSemaphores[m_CurrentAcquireSemIndex];
        waitSem.value     = 0;
        waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        // renderFinished is per-image (safe: acquiring image N means presentation released its sem); timeline tracks frame.
        VkSemaphoreSubmitInfo signalSems[2]{};
        signalSems[0].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalSems[0].semaphore = m_RenderFinishedSemaphores[m_AcquiredImageIndex];
        signalSems[0].value     = 0;
        signalSems[0].stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        signalSems[1].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalSems[1].semaphore = m_FrameTimeline.GetHandle();
        signalSems[1].value     = frameIndex + 1;
        signalSems[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = cmd;

        VkSubmitInfo2 submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submitInfo.waitSemaphoreInfoCount   = 1;
        submitInfo.pWaitSemaphoreInfos      = &waitSem;
        submitInfo.commandBufferInfoCount   = 1;
        submitInfo.pCommandBufferInfos      = &cmdInfo;
        submitInfo.signalSemaphoreInfoCount = 2;
        submitInfo.pSignalSemaphoreInfos    = signalSems;

        if (!VulkanContext::Get().Submit2(submitInfo, VK_NULL_HANDLE))
        {
            LH_CORE_ERROR("Failed to submit frame!");
        }

        m_Swapchain->Present(m_RenderFinishedSemaphores[m_AcquiredImageIndex]);
    }

    void VulkanBackend::OnResize(u32 width, u32 height)
    {
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
        m_NextAcquireSemIndex = 0;

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
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = VulkanContext::Get().GetGraphicsFamily();

        if (vkCreateCommandPool(VulkanContext::Get().GetDevice(), &poolInfo, nullptr, &m_PrimaryCommandPool) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create primary command pool!");
        }

        m_PrimaryCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_PrimaryCommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)m_PrimaryCommandBuffers.size();

        if (vkAllocateCommandBuffers(VulkanContext::Get().GetDevice(), &allocInfo, m_PrimaryCommandBuffers.data()) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to allocate primary command buffers!");
        }
    }
    bool VulkanBackend::IsFrameComplete(u64 frameIndex)
    {
        // Non-blocking check: has GPU finished this frame?
        // Timeline value for frame F is F+1 (set in SubmitFrame)
        u64 requiredValue = frameIndex + 1;
        return m_FrameTimeline.GetValue() >= requiredValue;
    }
}
