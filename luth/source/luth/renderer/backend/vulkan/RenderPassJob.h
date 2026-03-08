#pragma once

#include "luth/core/LuthTypes.h"
#include "DynamicRendering.h"
#include "luth/core/JobSystem.h"
#include "CommandAllocatorPool.h"
#include <functional>
#include <span>

namespace Luth
{
    // ===================================================================================
    // Render Pass Job
    // ===================================================================================
    // Records commands into a secondary command buffer on a worker thread.
    // Used by RenderGraph::Execute() — the executor collects the CommandBuffer
    // after WaitForCounter, so no TargetFrame push is needed.
    
    struct RenderPassJob
    {
        // Attachment metadata for inheritance info
        std::span<AttachmentInfo> ColorAttachments;
        AttachmentInfo DepthAttachment;
        bool HasDepth = false;
        
        std::function<void(VkCommandBuffer)> RecordFunction;
        
        // Output: The recorded command buffer (read by executor after WaitForCounter)
        VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;

        static void Execute(RenderPassJob* job)
        {
            // 1. Acquire Command Buffer from per-thread pool
            auto* ctx = JobSystem::GetCurrentJobContext();
            if (!ctx->CommandPool) return;
            
            CommandAllocator* allocator = ctx->CommandPool->Acquire();
            VkCommandBuffer cmd = allocator->GetBuffer(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
            job->CommandBuffer = cmd;

            // 2. Setup Inheritance for Dynamic Rendering
            std::vector<VkFormat> colorFormats;
            for (const auto& att : job->ColorAttachments)
            {
                colorFormats.push_back(att.Format);
            }

            VkCommandBufferInheritanceRenderingInfo renderingInheritanceInfo{};
            renderingInheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
            renderingInheritanceInfo.viewMask = 0;
            renderingInheritanceInfo.colorAttachmentCount = (u32)colorFormats.size();
            renderingInheritanceInfo.pColorAttachmentFormats = colorFormats.data();
            
            if (job->HasDepth)
            {
                renderingInheritanceInfo.depthAttachmentFormat = job->DepthAttachment.Format;
            }
            
            renderingInheritanceInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkCommandBufferInheritanceInfo inheritanceInfo{};
            inheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
            inheritanceInfo.pNext = &renderingInheritanceInfo;
            
            // 3. Begin Recording
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            beginInfo.pInheritanceInfo = &inheritanceInfo;

            vkBeginCommandBuffer(cmd, &beginInfo);

            // 4. User Recording
            if (job->RecordFunction)
            {
                job->RecordFunction(cmd);
            }

            // 5. End Recording
            vkEndCommandBuffer(cmd);
            
            // 6. Release Allocator back to pool
            ctx->CommandPool->Release(allocator);
        }
    };
}
