#include "luthpch.h"
#include "VKRendererAPI.h"
#include "VulkanContext.h"
#include "luth/core/Log.h"
#include "luth/window/Window.h" // For getting native window handle
#include "luth/renderer/rendergraph/RenderGraph.h"

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
            vkDestroyFence(device, m_InFlightFences[i], nullptr);
        }

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

        // Wait for previous frame
        vkWaitForFences(device, 1, &m_InFlightFences[m_CurrentFrame], VK_TRUE, UINT64_MAX);

        // Flush deletions AFTER we know the GPU is done with this frame's resources
        VulkanContext::Get().FlushDeletionQueue();

        // Acquire image
        u32 imageIndex = m_Swapchain->AcquireNextImage(m_ImageAvailableSemaphores[m_CurrentFrame]);
        
        if (imageIndex == -1) {
            // If acquire failed, we can't proceed. 
            // We might need to recreate swapchain if it's out of date, but usually OnResize handles it.
            // Just return false to skip this frame.
            // Consider adding a small sleep here to avoid spinning if we are minimized or in a bad state.
            return false; 
        }

        vkResetFences(device, 1, &m_InFlightFences[m_CurrentFrame]);

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
        // The RenderGraph leaves it in COLOR_ATTACHMENT_OPTIMAL (from ImGuiPass)
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

        // Check for submit failure to avoid desyncing semaphores
        if (!VulkanContext::Get().Submit(submitInfo, m_InFlightFences[m_CurrentFrame]))
        {
            // If submit failed, do not present. Wait for idle to reset state.
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
        m_InFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        VkDevice device = VulkanContext::Get().GetDevice();

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_ImageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_RenderFinishedSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(device, &fenceInfo, nullptr, &m_InFlightFences[i]) != VK_SUCCESS) {
                LH_CORE_CRITICAL("Failed to create synchronization objects!");
            }
        }
    }
}
