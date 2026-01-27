#pragma once

#include "luth/core/LuthTypes.h"
#include "DynamicRendering.h"
#include "luth/core/JobSystem.h"
#include "CommandAllocatorPool.h"
#include "luth/core/FrameData.h" // For FrameContext
#include <functional>
#include <span>

namespace Luth
{
    // ===================================================================================
    // Render Pass Job
    // ===================================================================================
    // A job that records commands into a secondary command buffer.
    // It uses Dynamic Rendering to avoid VkRenderPass objects.
    
    struct RenderPassJob
    {
        // Flattened Pass Info for easier access
        std::span<AttachmentInfo> ColorAttachments;
        AttachmentInfo DepthAttachment;
        bool HasDepth = false;
        
        std::function<void(VkCommandBuffer)> RecordFunction;
        
        // Output: The recorded command buffer
        VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
        
        // Pointer to the FrameContext to push the result to
        FrameContext* TargetFrame = nullptr;

        static void Execute(RenderPassJob* job)
        {
            // 1. Acquire Command Buffer
            auto* ctx = JobSystem::GetCurrentJobContext();
            if (!ctx->CommandPool) return;
            
            if (!ctx->CurrentCommandAllocator)
            {
                ctx->CurrentCommandAllocator = ctx->CommandPool->Acquire();
            }
            
            CommandAllocator* allocator = (CommandAllocator*)ctx->CurrentCommandAllocator;
            VkCommandBuffer cmd = allocator->GetBuffer(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
            job->CommandBuffer = cmd;

            // 2. Setup Inheritance for Dynamic Rendering
            std::vector<VkFormat> colorFormats;
            for(const auto& att : job->ColorAttachments)
            {
                colorFormats.push_back(att.Format);
            }

            VkCommandBufferInheritanceRenderingInfo renderingInheritanceInfo{};
            renderingInheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
            renderingInheritanceInfo.flags = 0;
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
            
            // 6. Push to FrameContext (Thread-Safe)
            if (job->TargetFrame)
            {
                std::lock_guard<std::mutex> lock(job->TargetFrame->CommandBufferMutex);
                job->TargetFrame->CommandBuffers.push_back(cmd);
            }
        }
    };
}
