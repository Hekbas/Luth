#include "luthpch.h"
#include "VulkanBackend.h"
#include "VulkanContext.h"
#include "luth/core/Log.h"
#include "luth/core/JobSystem.h"

namespace Luth
{
    void VulkanBackend::Init(void* windowHandle)
    {
        VulkanContext::Init(windowHandle);
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
    }

    void VulkanBackend::Shutdown()
    {
        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
        
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(VulkanContext::Get().GetDevice(), m_ImageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(VulkanContext::Get().GetDevice(), m_RenderFinishedSemaphores[i], nullptr);
            m_CommandAllocatorPools[i]->Shutdown();
        }
        
        vkDestroyCommandPool(VulkanContext::Get().GetDevice(), m_PrimaryCommandPool, nullptr);
        
        m_FrameTimeline.Shutdown();
        m_Swapchain.reset();
        VulkanContext::Shutdown();
    }

    u32 VulkanBackend::AcquireImage(u64 frameIndex)
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
        }
        
        // Flush deletions AFTER we know the GPU is done with this frame's resources
        VulkanContext::Get().FlushDeletionQueue();
        
        // Reset Command Allocator Pool for THIS frame
        m_CommandAllocatorPools[m_CurrentFrameIndex]->ResetAll();
        
        // Reset Primary Command Buffer for THIS frame
        vkResetCommandBuffer(m_PrimaryCommandBuffers[m_CurrentFrameIndex], 0);
        
        // Update JobSystem Context
        JobSystem::SetGlobalCommandPool(m_CommandAllocatorPools[m_CurrentFrameIndex].get());

        return m_Swapchain->AcquireNextImage(m_ImageAvailableSemaphores[m_CurrentFrameIndex]);
    }

    void* VulkanBackend::GetFrameCommandBuffer(u64 frameIndex)
    {
        u32 index = frameIndex % MAX_FRAMES_IN_FLIGHT;
        return m_PrimaryCommandBuffers[index];
    }

    void VulkanBackend::SubmitFrame(u64 frameIndex, void* commandBuffer)
    {
        VkCommandBuffer cmd = (VkCommandBuffer)commandBuffer;
        
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = { m_ImageAvailableSemaphores[m_CurrentFrameIndex] };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;

        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkSemaphore signalSemaphores[] = { m_RenderFinishedSemaphores[m_CurrentFrameIndex] };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        // Timeline Signal
        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.signalSemaphoreValueCount = 1;
        u64 signalValue = frameIndex + 1;
        timelineInfo.pSignalSemaphoreValues = &signalValue;
        
        // Correctly setup signal semaphores: RenderFinished (Binary) AND FrameTimeline (Timeline)
        std::vector<VkSemaphore> allSignalSemaphores = { m_RenderFinishedSemaphores[m_CurrentFrameIndex], m_FrameTimeline.GetHandle() };
        std::vector<u64> allSignalValues = { 0, signalValue }; // 0 is ignored for binary semaphores
        
        submitInfo.signalSemaphoreCount = 2;
        submitInfo.pSignalSemaphores = allSignalSemaphores.data();
        timelineInfo.signalSemaphoreValueCount = 2;
        timelineInfo.pSignalSemaphoreValues = allSignalValues.data();
        submitInfo.pNext = &timelineInfo;

        if (!VulkanContext::Get().Submit(submitInfo, VK_NULL_HANDLE))
        {
            LH_CORE_ERROR("Failed to submit frame!");
        }
        
        m_Swapchain->Present(m_RenderFinishedSemaphores[m_CurrentFrameIndex]);
    }

    void VulkanBackend::OnResize(u32 width, u32 height)
    {
        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
        m_Swapchain->Recreate(width, height);
    }

    VkDevice VulkanBackend::GetDevice() const
    {
        return VulkanContext::Get().GetDevice();
    }

    void VulkanBackend::CreateSyncObjects()
    {
        m_ImageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        m_RenderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        m_FrameTimeline.Init(0);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkCreateSemaphore(VulkanContext::Get().GetDevice(), &semaphoreInfo, nullptr, &m_ImageAvailableSemaphores[i]);
            vkCreateSemaphore(VulkanContext::Get().GetDevice(), &semaphoreInfo, nullptr, &m_RenderFinishedSemaphores[i]);
        }
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
}
