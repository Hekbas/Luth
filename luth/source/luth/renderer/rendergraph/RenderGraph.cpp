#include "luthpch.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/core/Log.h"
#include "luth/core/Profiler.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VKRendererAPI.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/RenderPassJob.h"
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

    ResourceHandle RenderPassBuilder::Write(ResourceHandle resource, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue)
    {
        ResourceHandle newHandle = m_Graph.RegisterWrite(m_PassIndex, resource, ResourceState::ColorAttachment);
        m_Graph.RegisterColorAttachment(m_PassIndex, newHandle, loadOp, storeOp, clearValue);
        return newHandle;
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
        ResourceNode node{ desc, 0, false, initialState, initialState };
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

    void RenderGraph::RegisterColorAttachment(u32 passIndex, ResourceHandle handle, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue)
    {
        PassAttachment att;
        att.handle = handle;
        att.loadOp = loadOp;
        att.storeOp = storeOp;
        att.clearValue = clearValue;
        m_Passes[passIndex].colorAttachments.push_back(att);
    }

    void RenderGraph::RegisterDepthAttachment(u32 passIndex, ResourceHandle handle, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue)
    {
        PassAttachment att;
        att.handle = handle;
        att.loadOp = loadOp;
        att.storeOp = storeOp;
        att.clearValue = clearValue;
        m_Passes[passIndex].depthAttachment = att;
        m_Passes[passIndex].hasDepth = true;
    }

    void RenderGraph::Compile()
    {
        LH_PROFILE_FUNCTION();

        for (auto& res : m_Resources)
        {
            res.currentState = res.initialState;
            if (res.isTransient)
            {
                res.currentState = ResourceState::Undefined;
            }
        }

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
        // Legacy execution (not updated for new attachment logic)
        // Use ExecuteParallel instead.
    }

    // ===================================================================================
    // Parallel Execution
    // ===================================================================================

    void RenderGraph::ExecuteParallel(VkCommandBuffer primaryCmd, std::vector<VkCommandBuffer>& outCommandBuffers)
    {
        LH_PROFILE_FUNCTION();

        AllocatePhysicalResources();
        
        auto* renderer = dynamic_cast<VKRendererAPI*>(Renderer::GetRendererAPI());
        LH_CORE_ASSERT(renderer, "RenderGraph::ExecuteParallel requires VKRendererAPI!");
        
        outCommandBuffers.resize(m_Passes.size());
        std::fill(outCommandBuffers.begin(), outCommandBuffers.end(), VK_NULL_HANDLE);

        JobSystem::Counter jobCounter;
        
        // Allocate jobs in a stable vector to avoid pointer invalidation
        std::vector<RenderPassJob> jobs(m_Passes.size());
        
        // Storage for AttachmentInfo vectors to keep them alive during job execution
        // We use a vector of vectors, indexed by pass index
        std::vector<std::vector<AttachmentInfo>> passAttachments(m_Passes.size());

        for (size_t i = 0; i < m_Passes.size(); ++i)
        {
            const auto& pass = m_Passes[i];
            RenderPassJob& job = jobs[i];
            auto& attachments = passAttachments[i];
            
            // 1. Build RenderPassInfo
            for(const auto& att : pass.colorAttachments)
            {
                ResourceNode& res = m_Resources[att.handle.index - 1];
                AttachmentInfo info{};
                info.ImageView = res.view;
                
                if (res.desc.format == TextureFormat::RGBA8_Unorm) info.Format = VK_FORMAT_R8G8B8A8_UNORM;
                // TODO: Add full format mapping
                
                info.LoadOp = att.loadOp;
                info.StoreOp = att.storeOp;
                info.ClearValue = att.clearValue;
                info.Layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                
                attachments.push_back(info);
            }
            
            job.ColorAttachments = { attachments.data(), attachments.size() };
            
            if (pass.hasDepth)
            {
                ResourceNode& res = m_Resources[pass.depthAttachment.handle.index - 1];
                AttachmentInfo info{};
                info.ImageView = res.view;
                info.Format = VK_FORMAT_D32_SFLOAT; // TODO: Map format
                info.LoadOp = pass.depthAttachment.loadOp;
                info.StoreOp = pass.depthAttachment.storeOp;
                info.ClearValue = pass.depthAttachment.clearValue;
                info.Layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                
                // We need to store this somewhere stable too if we want to point to it
                // But RenderPassJob has a separate DepthAttachment field (not a pointer to vector)
                // Wait, RenderPassJob struct definition is needed to confirm.
                // Assuming it stores AttachmentInfo by value or pointer.
                // If by value, we are good.
                job.DepthAttachment = info;
                job.HasDepth = true;
            }

            // 2. Inject Barriers (Serial for now, into Primary Cmd)
            for (const auto& barrier : pass.preBarriers)
            {
                ResourceNode& res = m_Resources[barrier.resource.index - 1];
                auto [srcStage, srcAccess] = GetStateInfo(barrier.before);
                auto [dstStage, dstAccess] = GetStateInfo(barrier.after);

                VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                b.srcStageMask = srcStage;
                b.srcAccessMask = srcAccess;
                b.dstStageMask = dstStage;
                b.dstAccessMask = dstAccess;
                b.oldLayout = GetLayout(barrier.before);
                b.newLayout = GetLayout(barrier.after);
                b.image = res.image;
                b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                
                if (res.desc.format == TextureFormat::D32_Float)
                    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

                VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &b;

                vkCmdPipelineBarrier2(primaryCmd, &dep);
            }

            // 3. Setup Job Lambda
            job.RecordFunction = [this, &pass](VkCommandBuffer cmd) {
                RenderPassContext ctx;
                ctx.commandBuffer = cmd;
                ctx.GetResource = [this](ResourceHandle h) -> void* {
                    if (h.index == 0 || h.index > m_Resources.size()) return nullptr;
                    // Return the ResourceNode itself, or the image/view?
                    // The context expects void* which the user casts to ResourceNode*
                    return &m_Resources[h.index - 1];
                };
                pass.execute(ctx);
            };
            
            // 4. Kick Job
            // We pass the job pointer. The job struct must remain valid until Wait.
            // 'jobs' vector is stable until end of function.
            JobSystem::Execute([](JobSystem::JobArgs args) {
                RenderPassJob* j = (RenderPassJob*)args.data;
                j->Execute(j); // Execute the recording logic
            }, &job, &jobCounter);
        }

        // Wait for all recording to finish
        JobSystem::WaitForCounter(&jobCounter);

        // Collect Command Buffers
        for (size_t i = 0; i < jobs.size(); ++i)
        {
            if (jobs[i].CommandBuffer != VK_NULL_HANDLE)
            {
                outCommandBuffers[i] = jobs[i].CommandBuffer;
            }
        }

        CleanupPhysicalResources();
    }

    void RenderGraph::AllocatePhysicalResources()
    {
        for (auto& res : m_Resources)
        {
            if (!res.isTransient || res.image != VK_NULL_HANDLE) continue;

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
