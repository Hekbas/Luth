#pragma once

#include "luth/core/LuthTypes.h"
#include "DynamicRendering.h"
#include "luth/core/JobSystem.h"
#include "CommandAllocatorPool.h"
#include <functional>

namespace Luth
{
    // ===================================================================================
    // Render Pass Job
    // ===================================================================================
    // A job that records commands into a secondary command buffer.
    // It uses Dynamic Rendering to avoid VkRenderPass objects.
    
    struct RenderPassJob
    {
        RenderPassInfo PassInfo;
        std::function<void(VkCommandBuffer)> RecordFunction;
        
        // Output: The recorded command buffer
        VkCommandBuffer OutputCommandBuffer = VK_NULL_HANDLE;

        static void Execute(JobSystem::JobArgs args)
        {
            RenderPassJob* job = (RenderPassJob*)args.data;
            
            // 1. Acquire Command Buffer
            auto* ctx = JobSystem::GetCurrentJobContext();
            if (!ctx->CommandPool) return;
            
            if (!ctx->CurrentCommandAllocator)
            {
                ctx->CurrentCommandAllocator = ctx->CommandPool->Acquire();
            }
            
            CommandAllocator* allocator = (CommandAllocator*)ctx->CurrentCommandAllocator;
            VkCommandBuffer cmd = allocator->GetBuffer(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
            job->OutputCommandBuffer = cmd;

            // 2. Setup Inheritance for Dynamic Rendering
            std::vector<VkFormat> colorFormats;
            for(const auto& att : job->PassInfo.ColorAttachments)
            {
                colorFormats.push_back(att.Format);
            }

            VkCommandBufferInheritanceRenderingInfo renderingInheritanceInfo{};
            renderingInheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
            renderingInheritanceInfo.flags = 0;
            renderingInheritanceInfo.viewMask = 0;
            renderingInheritanceInfo.colorAttachmentCount = (u32)colorFormats.size();
            renderingInheritanceInfo.pColorAttachmentFormats = colorFormats.data();
            
            if (job->PassInfo.DepthAttachment)
            {
                renderingInheritanceInfo.depthAttachmentFormat = job->PassInfo.DepthAttachment->Format;
            }
            
            if (job->PassInfo.StencilAttachment)
            {
                renderingInheritanceInfo.stencilAttachmentFormat = job->PassInfo.StencilAttachment->Format;
            }
            
            // TODO: Rasterization Samples? Assume 1 for now or add to PassInfo.
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
        }
    };
}
