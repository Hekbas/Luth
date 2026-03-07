#include "luthpch.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/MaterialSystem.h"
#include "luth/core/JobSystem.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/CommandAllocatorPool.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"

namespace Luth
{
    std::unique_ptr<RenderBackend> Renderer::s_Backend = nullptr;
    FrameData* Renderer::s_FrameData = nullptr;

    void Renderer::Init(void* windowHandle)
    {
        s_Backend = RenderBackend::Create();
        s_Backend->Init(windowHandle);
        MaterialSystem::Init();
    }

    void Renderer::Shutdown()
    {
        MaterialSystem::Shutdown();
        if (s_Backend) {
            s_Backend->Shutdown();
            s_Backend.reset();
        }
    }

    void Renderer::SetFrameData(FrameData* frameData)
    {
        s_FrameData = frameData;
    }

    void Renderer::BeginFrame(u64 frameIndex)
    {
        // Acquire swapchain image (may block on GPU fence for this frame's slot)
        s_Backend->AcquireImage(frameIndex);
    }

    void Renderer::EndFrame()
    {
        // No-op — submission happens in ExecuteGraph
    }

    void Renderer::OnResize(u32 width, u32 height)
    {
        s_Backend->OnResize(width, height);
    }

    void Renderer::ExecuteGraph(RG::RenderGraph& graph, u64 frameIndex)
    {
        // Get the primary command buffer for this frame
        VkCommandBuffer primaryCmd = (VkCommandBuffer)s_Backend->GetFrameCommandBuffer(frameIndex);
        
        // Begin Primary
        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(primaryCmd, &beginInfo);
        
        // Execute Graph (Records barriers to primary, dispatches secondary buffers)
        std::vector<VkCommandBuffer> secondaryBuffers;
        graph.ExecuteParallel(primaryCmd, secondaryBuffers);
        
        // Present Barrier for Swapchain Image
        auto* vkBackend = static_cast<VulkanBackend*>(s_Backend.get());
        VkImage swapchainImage = vkBackend->GetSwapchain().GetImage(vkBackend->GetSwapchain().GetCurrentFrameIndex());
        
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = swapchainImage;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        vkCmdPipelineBarrier(primaryCmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        // End Primary
        vkEndCommandBuffer(primaryCmd);
        
        // Submit to GPU + Present
        s_Backend->SubmitFrame(frameIndex, primaryCmd);
    }
}
