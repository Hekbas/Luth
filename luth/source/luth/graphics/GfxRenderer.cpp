#include "luthpch.h"
#include "luth/graphics/GfxRenderer.h"
#include "luth/core/Log.h"
#include "luth/core/Profiler.h"
#include "luth/renderer/vulkan/VKCommon.h" // For VK_CHECK_RESULT

namespace Luth::Gfx
{
    std::unique_ptr<GfxSwapchain> GfxRenderer::s_Swapchain;
    std::unique_ptr<VKRenderGraphExecutor> GfxRenderer::s_GraphExecutor;
    VkCommandPool GfxRenderer::s_CommandPool;
    std::vector<VkCommandBuffer> GfxRenderer::s_CommandBuffers;
    std::vector<GfxRenderer::FrameData> GfxRenderer::s_Frames;
    u32 GfxRenderer::s_CurrentFrameIndex = 0;
    u32 GfxRenderer::s_CurrentImageIndex = 0;

    void GfxRenderer::Init(void* windowHandle, u32 width, u32 height)
    {
        GfxContext::Init(windowHandle);
        s_Swapchain = std::make_unique<GfxSwapchain>(width, height);
        
        auto& ctx = GfxContext::Get();

        // Command Pool
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = ctx.GetGraphicsQueue().familyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK_RESULT(vkCreateCommandPool(ctx.GetDevice(), &poolInfo, nullptr, &s_CommandPool), "Failed to create command pool");

        CreateCommandBuffers();
        CreateSyncObjects();

        // Reuse the executor logic we wrote earlier (it depends on VKResourceManager)
        // We might need to move VKRenderGraphExecutor to Luth::Gfx namespace later
        s_GraphExecutor = std::make_unique<VKRenderGraphExecutor>(ctx.GetDevice(), ctx.GetPhysicalDevice());
    }

    void GfxRenderer::Shutdown()
    {
        vkDeviceWaitIdle(GfxContext::Get().GetDevice());

        s_GraphExecutor.reset();
        s_Swapchain.reset();

        auto device = GfxContext::Get().GetDevice();
        for (auto& frame : s_Frames)
        {
            vkDestroySemaphore(device, frame.imageAvailable, nullptr);
            vkDestroySemaphore(device, frame.renderFinished, nullptr);
            vkDestroyFence(device, frame.inFlightFence, nullptr);
        }

        vkDestroyCommandPool(device, s_CommandPool, nullptr);
        GfxContext::Shutdown();
    }

    void GfxRenderer::Resize(u32 width, u32 height)
    {
        vkDeviceWaitIdle(GfxContext::Get().GetDevice());
        s_Swapchain->Resize(width, height);
    }

    void GfxRenderer::CreateCommandBuffers()
    {
        s_CommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = s_CommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (u32)s_CommandBuffers.size();

        VK_CHECK_RESULT(vkAllocateCommandBuffers(GfxContext::Get().GetDevice(), &allocInfo, s_CommandBuffers.data()), "Failed to alloc cmd buffers");
    }

    void GfxRenderer::CreateSyncObjects()
    {
        s_Frames.resize(MAX_FRAMES_IN_FLIGHT);
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        auto device = GfxContext::Get().GetDevice();
        for (auto& frame : s_Frames)
        {
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.imageAvailable);
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.renderFinished);
            vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlightFence);
        }
    }

    void GfxRenderer::BeginFrame()
    {
        auto& frame = s_Frames[s_CurrentFrameIndex];
        auto device = GfxContext::Get().GetDevice();

        vkWaitForFences(device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);

        if (!s_Swapchain->AcquireNextImage(frame.imageAvailable, s_CurrentImageIndex))
        {
            // Resize needed
            // For now, just wait? Or handle resize loop.
            // Assuming App handles resize event and calls Resize()
        }
        
        vkResetFences(device, 1, &frame.inFlightFence);
        vkResetCommandBuffer(s_CommandBuffers[s_CurrentFrameIndex], 0);
    }

    void GfxRenderer::ExecuteGraph(RG::RenderGraph& graph)
    {
        LH_PROFILE_FUNCTION();

        VkCommandBuffer cmd = s_CommandBuffers[s_CurrentFrameIndex];
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Import Backbuffer
        RG::TextureDesc backbufferDesc;
        backbufferDesc.name = "Backbuffer";
        backbufferDesc.width = s_Swapchain->GetExtent().width;
        backbufferDesc.height = s_Swapchain->GetExtent().height;
        backbufferDesc.format = RG::TextureFormat::RGBA8_Unorm; // TODO: Map from s_Swapchain->GetFormat()

        VKImageResource backbufferRes;
        backbufferRes.image = s_Swapchain->GetImage(s_CurrentImageIndex);
        backbufferRes.view = s_Swapchain->GetImageView(s_CurrentImageIndex);
        backbufferRes.format = s_Swapchain->GetFormat();
        backbufferRes.extent = { backbufferDesc.width, backbufferDesc.height, 1 };
        backbufferRes.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED; // Acquired image is undefined/presentable

        graph.ImportResource(backbufferDesc, &backbufferRes, RG::ResourceState::Undefined);

        // Execute
        s_GraphExecutor->Execute(graph, cmd);

        // Transition to Present
        {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; // Assuming last pass was Clear/Copy
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = backbufferRes.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = 0;

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &barrier
            );
        }

        vkEndCommandBuffer(cmd);
    }

    void GfxRenderer::EndFrame()
    {
        auto& frame = s_Frames[s_CurrentFrameIndex];
        VkCommandBuffer cmd = s_CommandBuffers[s_CurrentFrameIndex];

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        
        VkSemaphore waitSemaphores[] = { frame.imageAvailable };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkSemaphore signalSemaphores[] = { frame.renderFinished };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        VK_CHECK_RESULT(vkQueueSubmit(GfxContext::Get().GetGraphicsQueue().handle, 1, &submitInfo, frame.inFlightFence), "Queue Submit Failed");

        s_Swapchain->Present(frame.renderFinished, s_CurrentImageIndex);

        s_CurrentFrameIndex = (s_CurrentFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
    }
}
