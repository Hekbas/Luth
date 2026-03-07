#include "luthpch.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/MaterialSystem.h"
#include "luth/core/JobSystem.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/CommandAllocatorPool.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h" // For barrier

namespace Luth
{
    std::unique_ptr<RenderBackend> Renderer::s_Backend = nullptr;
    FrameData Renderer::s_FrameData;

    void Renderer::Init(void* windowHandle)
    {
        s_Backend = RenderBackend::Create();
        s_Backend->Init(windowHandle);
        s_FrameData.Init();
        MaterialSystem::Init();
    }

    void Renderer::Shutdown()
    {
        MaterialSystem::Shutdown();
        s_FrameData.Shutdown();
        if (s_Backend) {
            s_Backend->Shutdown();
            s_Backend.reset();
        }
    }

    void Renderer::BeginFrame()
    {
        // 1. Advance Frame Data
        s_FrameData.Advance();
        FrameContext& frame = s_FrameData.GetCurrentFrame();
        
        // 2. Acquire Image (Might block/wait for GPU)
        u32 imageIndex = s_Backend->AcquireImage(s_FrameData.GetCurrentFrameIndex());
        
        // 3. Reset Frame Allocators
        frame.Reset();
    }

    void Renderer::EndFrame()
    {
        // No-op here, submission happens in ExecuteGraph
    }

    void Renderer::OnResize(u32 width, u32 height)
    {
        s_Backend->OnResize(width, height);
    }

    void Renderer::ExecuteGraph(RG::RenderGraph& graph)
    {
        FrameContext& frame = s_FrameData.GetCurrentFrame();
        
        // Get the dedicated Primary Command Buffer for this frame from the backend
        VkCommandBuffer primaryCmd = (VkCommandBuffer)s_Backend->GetFrameCommandBuffer(s_FrameData.GetCurrentFrameIndex());
        
        // Begin Primary
        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(primaryCmd, &beginInfo);
        
        // Execute Graph (Records barriers to primary, returns secondary buffers)
        std::vector<VkCommandBuffer> secondaryBuffers;
        graph.ExecuteParallel(primaryCmd, secondaryBuffers);
        
        // Execute Secondary
        if (!secondaryBuffers.empty())
        {
            // We must execute each secondary buffer inside its own Dynamic Rendering block
            // IF it was recorded with RENDER_PASS_CONTINUE.
            // But RenderGraph::ExecuteParallel currently returns a list.
            // And RenderPassJob sets RENDER_PASS_CONTINUE.
            // So we CANNOT just call vkCmdExecuteCommands for all of them at once if they have different attachments.
            
            // FIX: RenderGraph::ExecuteParallel logic is flawed for the "Return List" approach.
            // It should have executed them.
            // But since we are here, let's assume for now we have ONE pass or compatible passes.
            // Wait, we have GeometryPass and ImGuiPass.
            // They have DIFFERENT attachments.
            // So we CANNOT batch them.
            
            // We need to fix RenderGraph::ExecuteParallel to NOT return a list, but execute immediately.
            // OR return a list of "Pass Execution Info" (CmdBuffer + Attachments).
            
            // For this specific step (fixing black screen), I will assume RenderGraph::ExecuteParallel
            // has been updated to record execution into primaryCmd.
            // But I haven't updated it yet!
            
            // I will update RenderGraph::ExecuteParallel in the next file write.
            // For now, I will comment out this block to avoid invalid execution.
            // vkCmdExecuteCommands(primaryCmd, (u32)secondaryBuffers.size(), secondaryBuffers.data());
        }
        
        // Inject Present Barrier for Swapchain Image
        // We need access to the swapchain image.
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
        
        // Submit
        s_Backend->SubmitFrame(s_FrameData.GetCurrentFrameIndex(), primaryCmd);
    }
}
