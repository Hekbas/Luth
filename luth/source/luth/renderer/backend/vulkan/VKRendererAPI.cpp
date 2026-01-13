#include "luthpch.h"
#include "VKRendererAPI.h"
#include "VulkanContext.h"
#include "VulkanWaitJob.h"
#include "luth/core/Log.h"
#include "luth/window/Window.h" // For getting native window handle
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/core/JobSystem.h"

namespace Luth
{
    void VKRendererAPI::Init(void* windowHandle)
    {
        LH_CORE_INFO("Initializing Vulkan Renderer API");
        
        // 1. Init Context (Instance, Device, VMA)
        VulkanContext::Init(windowHandle);

        // 2. Init Swapchain
        m_Swapchain = std::make_unique<VulkanSwapchain>(windowHandle);
        m_Swapchain->Init();

        // 3. Init Command Pool & Sync Objects
        CreateCommandBuffers();
        CreateSyncObjects();
    }

    void VKRendererAPI::Shutdown()
    {
        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());

        VkDevice device = VulkanContext::Get().GetDevice();

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(device, m_ImageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(device, m_RenderFinishedSemaphores[i], nullptr);
        }
        
        m_FrameTimeline.Shutdown();

        vkDestroyCommandPool(device, m_CommandPool, nullptr);
        m_Swapchain.reset();
        VulkanContext::Shutdown();
    }
    
    bool VKRendererAPI::BeginFrame()
    {
        LH_PROFILE_FUNCTION();

        VkDevice device = VulkanContext::Get().GetDevice();
        
        // Update Context Frame Index and Flush Deletions for this frame
        VulkanContext::Get().SetCurrentFrameIndex(m_CurrentFrame);
        VulkanContext::Get().GetResourceCache().NewFrame(); // Tick Cache

        // Wait for previous frame using Poller Job (Non-blocking)
        // We need to wait until the GPU has finished with the resources of this frame index
        // from the PREVIOUS cycle.
        u64 waitValue = m_FrameValues[m_CurrentFrame];
        if (waitValue > 0)
        {
            // If we are on the main thread (which we should be for BeginFrame),
            // we can use a fiber-aware wait.
            // We spawn a job that waits for the timeline semaphore, and we wait for that job.
            
            JobSystem::Counter waitCounter;
            VulkanWaitJob::Dispatch(m_FrameTimeline, waitValue, &waitCounter);
            JobSystem::WaitForCounter(&waitCounter, 0);
        }

        // Flush deletions AFTER we know the GPU is done with this frame's resources
        VulkanContext::Get().FlushDeletionQueue();

        // Acquire image
        u32 imageIndex = m_Swapchain->AcquireNextImage(m_ImageAvailableSemaphores[m_CurrentFrame]);
        
        if (imageIndex == -1) {
            return false; 
        }

        // Reset Command Buffer
        vkResetCommandBuffer(m_CommandBuffers[m_CurrentFrame], 0);

        // Begin Recording
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_CommandBuffers[m_CurrentFrame], &beginInfo);
        
        return true;
    }

    void VKRendererAPI::EndFrame()
    {
        LH_PROFILE_FUNCTION();

        VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

        // Transition Swapchain Image to Present Layout
        VkImage swapchainImage = m_Swapchain->GetImage(m_Swapchain->GetCurrentFrameIndex());
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = swapchainImage;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);

        // Advance Timeline Value
        m_CurrentFrameValue++;
        m_FrameValues[m_CurrentFrame] = m_CurrentFrameValue;

        // Submit
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = { m_ImageAvailableSemaphores[m_CurrentFrame] };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;

        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkSemaphore signalSemaphores[] = { m_RenderFinishedSemaphores[m_CurrentFrame] };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        // Timeline Semaphore Info
        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.signalSemaphoreValueCount = 1;
        u64 signalValue = m_CurrentFrameValue;
        timelineInfo.pSignalSemaphoreValues = &signalValue;
        
        // We need to signal the timeline semaphore too
        // But wait, we can't mix binary and timeline semaphores easily in the same array in standard submit?
        // Actually we can chain pNext.
        // But we need to attach the timeline semaphore to the signal list.
        // Let's add the timeline semaphore to the signal list.
        
        std::vector<VkSemaphore> allSignalSemaphores = { m_RenderFinishedSemaphores[m_CurrentFrame], m_FrameTimeline.GetHandle() };
        std::vector<u64> allSignalValues = { 0, m_CurrentFrameValue }; // 0 is ignored for binary semaphores
        
        submitInfo.signalSemaphoreCount = 2;
        submitInfo.pSignalSemaphores = allSignalSemaphores.data();
        
        timelineInfo.signalSemaphoreValueCount = 2;
        timelineInfo.pSignalSemaphoreValues = allSignalValues.data();
        
        submitInfo.pNext = &timelineInfo;

        // We don't use fences anymore!
        if (!VulkanContext::Get().Submit(submitInfo, VK_NULL_HANDLE))
        {
            vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
            return;
        }

        // Present
        m_Swapchain->Present(m_RenderFinishedSemaphores[m_CurrentFrame]);

        m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    void VKRendererAPI::ExecuteGraph(RG::RenderGraph& graph)
    {
        graph.Execute(m_CommandBuffers[m_CurrentFrame]);
    }

    void VKRendererAPI::OnResize(u32 width, u32 height)
    {
        LH_PROFILE_FUNCTION();

        if (width == 0 || height == 0) return;
        m_Swapchain->Recreate(width, height);
    }

    void VKRendererAPI::CreateCommandBuffers()
    {
        auto& ctx = VulkanContext::Get();

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = ctx.GetGraphicsFamily();

        if (vkCreateCommandPool(ctx.GetDevice(), &poolInfo, nullptr, &m_CommandPool) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create command pool!");
        }

        m_CommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_CommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)m_CommandBuffers.size();

        if (vkAllocateCommandBuffers(ctx.GetDevice(), &allocInfo, m_CommandBuffers.data()) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to allocate command buffers!");
        }
    }

    void VKRendererAPI::CreateSyncObjects()
    {
        m_ImageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        m_RenderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        
        // Initialize Timeline Semaphore
        m_FrameTimeline.Init(0);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkDevice device = VulkanContext::Get().GetDevice();

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_ImageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_RenderFinishedSemaphores[i]) != VK_SUCCESS) {
                LH_CORE_CRITICAL("Failed to create synchronization objects!");
            }
        }
    }
}
