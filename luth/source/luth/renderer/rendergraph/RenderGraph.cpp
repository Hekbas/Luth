#include "luthpch.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/core/Log.h"
#include "luth/core/Profiler.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VKRendererAPI.h"
#include "luth/renderer/Renderer.h"
#include <vma/vk_mem_alloc.h>

namespace Luth::RG
{
    // ===================================================================================
    // RenderPassBuilder Implementation
    // ===================================================================================

    ResourceHandle RenderPassBuilder::Read(ResourceHandle resource)
    {
        m_Graph.RegisterRead(m_PassIndex, resource, ResourceState::ShaderResource);
        return resource;
    }

    ResourceHandle RenderPassBuilder::ReadTransfer(ResourceHandle resource)
    {
        m_Graph.RegisterRead(m_PassIndex, resource, ResourceState::TransferSrc);
        return resource;
    }

    ResourceHandle RenderPassBuilder::Write(ResourceHandle resource)
    {
        return m_Graph.RegisterWrite(m_PassIndex, resource, ResourceState::ColorAttachment);
    }

    ResourceHandle RenderPassBuilder::WriteTransfer(ResourceHandle resource)
    {
        return m_Graph.RegisterWrite(m_PassIndex, resource, ResourceState::TransferDst);
    }

    ResourceHandle RenderPassBuilder::CreateTexture(const TextureDesc& desc)
    {
        return m_Graph.RegisterResource(desc);
    }

    // ===================================================================================
    // RenderGraph Implementation
    // ===================================================================================

    RenderGraph::RenderGraph(LinearAllocator& allocator)
        : m_Allocator(allocator)
    {
        m_Passes.reserve(64);
        m_Resources.reserve(256);
    }

    ResourceHandle RenderGraph::RegisterResource(const TextureDesc& desc)
    {
        u32 index = (u32)m_Resources.size() + 1; // 1-based index
        m_Resources.push_back({ desc, 0, true, ResourceState::Undefined, ResourceState::Undefined });
        return { index, 0 };
    }

    ResourceHandle RenderGraph::ImportResource(const TextureDesc& desc, void* image, void* view, ResourceState initialState)
    {
        u32 index = (u32)m_Resources.size() + 1;
        // isTransient = false because we don't own it
        ResourceNode node{ desc, 0, false, initialState, initialState }; // Current state starts at initial
        node.image = (VkImage)image;
        node.view = (VkImageView)view;
        node.external = true;
        m_Resources.push_back(node);
        return { index, 0 };
    }

    void RenderGraph::RegisterRead(u32 passIndex, ResourceHandle handle, ResourceState state)
    {
        LH_CORE_ASSERT(handle.IsValid(), "Invalid resource handle read!");
        LH_CORE_ASSERT(handle.index <= m_Resources.size(), "Resource index out of bounds!");
        m_Passes[passIndex].reads.push_back(handle);
        m_Passes[passIndex].readStates.push_back(state);
    }

    ResourceHandle RenderGraph::RegisterWrite(u32 passIndex, ResourceHandle handle, ResourceState state)
    {
        LH_CORE_ASSERT(handle.IsValid(), "Invalid resource handle write!");
        
        ResourceNode& node = m_Resources[handle.index - 1];
        node.version++;
        
        ResourceHandle newHandle = { handle.index, node.version };
        m_Passes[passIndex].writes.push_back(newHandle);
        m_Passes[passIndex].writeStates.push_back(state);
        
        return newHandle;
    }

    void RenderGraph::Compile()
    {
        LH_PROFILE_FUNCTION();

        // Reset resource states for simulation
        for (auto& res : m_Resources)
        {
            res.currentState = res.initialState;
            // If transient, initial state is Undefined (newly allocated)
            if (res.isTransient)
            {
                res.currentState = ResourceState::Undefined;
            }
        }

        // Iterate passes to inject barriers
        for (auto& pass : m_Passes)
        {
            // 1. Process Reads
            for (size_t i = 0; i < pass.reads.size(); ++i)
            {
                ResourceHandle handle = pass.reads[i];
                ResourceState targetState = pass.readStates[i];
                ResourceNode& res = m_Resources[handle.index - 1];
                
                if (res.currentState != targetState)
                {
                    Barrier barrier;
                    barrier.resource = handle;
                    barrier.before = res.currentState;
                    barrier.after = targetState;
                    
                    pass.preBarriers.push_back(barrier);
                    res.currentState = targetState;
                }
            }

            // 2. Process Writes
            for (size_t i = 0; i < pass.writes.size(); ++i)
            {
                ResourceHandle handle = pass.writes[i];
                ResourceState targetState = pass.writeStates[i];
                ResourceNode& res = m_Resources[handle.index - 1];
                
                // Override target state for Depth (if not transfer)
                if (targetState == ResourceState::ColorAttachment)
                {
                    if (res.desc.format == TextureFormat::D32_Float || 
                        res.desc.format == TextureFormat::D24_Unorm_S8_Uint)
                    {
                        targetState = ResourceState::DepthStencilAttachment;
                    }
                }

                if (res.currentState != targetState)
                {
                    Barrier barrier;
                    barrier.resource = handle;
                    barrier.before = res.currentState;
                    barrier.after = targetState;
                    
                    pass.preBarriers.push_back(barrier);
                    res.currentState = targetState;
                }
            }
        }
    }

    // Helper to map states to Vulkan flags
    static std::pair<VkPipelineStageFlags2, VkAccessFlags2> GetStateInfo(ResourceState state) {
        switch (state) {
            case ResourceState::Undefined: return { VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
            case ResourceState::ColorAttachment: return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
            case ResourceState::DepthStencilAttachment: return { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
            case ResourceState::TransferDst: return { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
            case ResourceState::TransferSrc: return { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT };
            case ResourceState::ShaderResource: return { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT };
            case ResourceState::Present: return { VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0 };
            default: return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
        }
    }

    static VkImageLayout GetLayout(ResourceState state) {
        switch (state) {
            case ResourceState::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
            case ResourceState::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            case ResourceState::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            case ResourceState::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            case ResourceState::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            case ResourceState::ShaderResource: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            case ResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            default: return VK_IMAGE_LAYOUT_UNDEFINED;
        }
    }

    void RenderGraph::Execute(VkCommandBuffer cmd)
    {
        LH_PROFILE_FUNCTION();

        AllocatePhysicalResources();

        RenderPassContext ctx; 
        ctx.commandBuffer = cmd;
        ctx.GetResource = [&](ResourceHandle h) -> void* {
            if (h.index == 0 || h.index > m_Resources.size()) return nullptr;
            return &m_Resources[h.index - 1];
        };
        
        for (const auto& pass : m_Passes)
        {
            LH_PROFILE_SCOPE_DYNAMIC(pass.name);

            // 1. Execute Barriers
            std::vector<VkImageMemoryBarrier2> barriers;
            for (const auto& b : pass.preBarriers)
            {
                ResourceNode& res = m_Resources[b.resource.index - 1];
                
                VkImageMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.image = res.image;
                
                if (res.desc.format == TextureFormat::D32_Float)
                    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                else
                    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;
                
                auto [srcStage, srcAccess] = GetStateInfo(b.before);
                auto [dstStage, dstAccess] = GetStateInfo(b.after);

                barrier.srcStageMask = srcStage;
                barrier.srcAccessMask = srcAccess;
                barrier.dstStageMask = dstStage;
                barrier.dstAccessMask = dstAccess;
                barrier.oldLayout = GetLayout(b.before);
                barrier.newLayout = GetLayout(b.after);

                barriers.push_back(barrier);
            }

            if (!barriers.empty())
            {
                VkDependencyInfo depInfo{};
                depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                depInfo.imageMemoryBarrierCount = (u32)barriers.size();
                depInfo.pImageMemoryBarriers = barriers.data();
                vkCmdPipelineBarrier2(cmd, &depInfo);
            }

            // 2. Execute Pass
            pass.execute(ctx);
        }

        CleanupPhysicalResources();
    }

    // ===================================================================================
    // Parallel Execution
    // ===================================================================================

    struct PassJobData
    {
        const RenderGraph::PassNode* pass;
        RenderGraph::ResourceNode* resources; // Pointer to array
        u32 resourceCount;
        VkCommandBuffer cmd;
    };

    struct ParallelJobArgs
    {
        PassJobData data;
        VkCommandBuffer* outCmd;
    };

    void RenderGraph::ExecuteParallel(VkCommandBuffer primaryCmd, std::vector<VkCommandBuffer>& outCommandBuffers)
    {
        LH_PROFILE_FUNCTION();

        AllocatePhysicalResources();
        
        auto* renderer = dynamic_cast<VKRendererAPI*>(Renderer::GetRendererAPI());
        LH_CORE_ASSERT(renderer, "RenderGraph::ExecuteParallel requires VKRendererAPI!");
        
        JobSystem::Counter jobCounter;
        std::vector<ParallelJobArgs> parallelArgs(m_Passes.size());
        outCommandBuffers.resize(m_Passes.size());

        // 1. Dispatch Jobs
        for (size_t i = 0; i < m_Passes.size(); ++i)
        {
            parallelArgs[i].data.pass = &m_Passes[i];
            parallelArgs[i].data.resources = m_Resources.data();
            parallelArgs[i].data.resourceCount = (u32)m_Resources.size();
            parallelArgs[i].outCmd = &outCommandBuffers[i];
            
            JobSystem::Execute([](JobSystem::JobArgs args)
            {
                auto* ctx = JobSystem::GetCurrentJobContext();
                LH_CORE_ASSERT(ctx && ctx->CommandPool, "JobContext missing CommandPool!");
                
                CommandAllocator* allocator = ctx->CommandPool->Acquire();
                VkCommandBuffer cmd = allocator->GetBuffer();
                
                VkCommandBufferInheritanceInfo inheritanceInfo{};
                inheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
                
                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                beginInfo.pInheritanceInfo = &inheritanceInfo;

                if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
                {
                    LH_CORE_ERROR("Failed to begin secondary command buffer!");
                    ctx->CommandPool->Release(allocator);
                    return;
                }
                
                ParallelJobArgs* jobArgs = (ParallelJobArgs*)args.data;
                jobArgs->data.cmd = cmd;
                
                RenderPassContext passCtx;
                passCtx.commandBuffer = cmd;
                passCtx.GetResource = [jobArgs](ResourceHandle h) -> void* {
                    if (h.index == 0 || h.index > jobArgs->data.resourceCount) return nullptr;
                    return &jobArgs->data.resources[h.index - 1];
                };
                
                jobArgs->data.pass->execute(passCtx);
                
                vkEndCommandBuffer(cmd);
                *jobArgs->outCmd = cmd;
                
                ctx->CommandPool->Release(allocator);
            }, &parallelArgs[i], &jobCounter);
        }

        // 2. Wait for all jobs
        JobSystem::WaitForCounter(&jobCounter);

        // 3. Record Barriers and Execute Secondary Buffers
        for (size_t i = 0; i < m_Passes.size(); ++i)
        {
            const auto& pass = m_Passes[i];
            
            // Barriers
            std::vector<VkImageMemoryBarrier2> barriers;
            for (const auto& b : pass.preBarriers)
            {
                ResourceNode& res = m_Resources[b.resource.index - 1];
                
                VkImageMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.image = res.image;
                
                if (res.desc.format == TextureFormat::D32_Float)
                    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                else
                    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;
                
                auto [srcStage, srcAccess] = GetStateInfo(b.before);
                auto [dstStage, dstAccess] = GetStateInfo(b.after);

                barrier.srcStageMask = srcStage;
                barrier.srcAccessMask = srcAccess;
                barrier.dstStageMask = dstStage;
                barrier.dstAccessMask = dstAccess;
                barrier.oldLayout = GetLayout(b.before);
                barrier.newLayout = GetLayout(b.after);

                barriers.push_back(barrier);
            }

            if (!barriers.empty())
            {
                VkDependencyInfo depInfo{};
                depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                depInfo.imageMemoryBarrierCount = (u32)barriers.size();
                depInfo.pImageMemoryBarriers = barriers.data();
                vkCmdPipelineBarrier2(primaryCmd, &depInfo);
            }
            
            // Execute Secondary Buffer
            if (outCommandBuffers[i] != VK_NULL_HANDLE)
            {
                vkCmdExecuteCommands(primaryCmd, 1, &outCommandBuffers[i]);
            }
        }

        CleanupPhysicalResources();
    }

    void RenderGraph::AllocatePhysicalResources()
    {
        for (auto& res : m_Resources)
        {
            if (!res.isTransient || res.image != VK_NULL_HANDLE) continue;

            // Use Cache
            PooledResource pooled = VulkanContext::Get().GetResourceCache().GetTexture(res.desc);
            res.image = pooled.image;
            res.view = pooled.view;
            res.allocation = (VmaAllocation_T*)pooled.allocation;
        }
    }

    void RenderGraph::CleanupPhysicalResources()
    {
        for (auto& res : m_Resources)
        {
            if (res.isTransient && res.image != VK_NULL_HANDLE)
            {
                // Return to Cache
                PooledResource pooled;
                pooled.image = res.image;
                pooled.view = res.view;
                pooled.allocation = (VmaAllocation)res.allocation;
                pooled.desc = res.desc;
                
                VulkanContext::Get().GetResourceCache().ReturnTexture(pooled);

                res.image = VK_NULL_HANDLE;
                res.view = VK_NULL_HANDLE;
                res.allocation = nullptr;
            }
        }
    }
}
