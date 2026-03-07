#include "luthpch.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/core/Log.h"
#include "luth/core/Profiler.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/RenderPassJob.h"
#include <vma/vk_mem_alloc.h>

namespace Luth::RG
{
    // ... (Builder implementation remains same) ...
    ResourceHandle RenderPassBuilder::Read(ResourceHandle resource) { m_Graph.RegisterRead(m_PassIndex, resource, ResourceState::ShaderResource); return resource; }
    ResourceHandle RenderPassBuilder::ReadTransfer(ResourceHandle resource) { m_Graph.RegisterRead(m_PassIndex, resource, ResourceState::TransferSrc); return resource; }
    ResourceHandle RenderPassBuilder::Write(ResourceHandle resource, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue) { ResourceHandle newHandle = m_Graph.RegisterWrite(m_PassIndex, resource, ResourceState::ColorAttachment); m_Graph.RegisterColorAttachment(m_PassIndex, newHandle, loadOp, storeOp, clearValue); return newHandle; }
    ResourceHandle RenderPassBuilder::WriteDepth(ResourceHandle resource, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue) { ResourceHandle newHandle = m_Graph.RegisterWrite(m_PassIndex, resource, ResourceState::DepthStencilAttachment); m_Graph.RegisterDepthAttachment(m_PassIndex, newHandle, loadOp, storeOp, clearValue); return newHandle; }
    ResourceHandle RenderPassBuilder::WriteTransfer(ResourceHandle resource) { return m_Graph.RegisterWrite(m_PassIndex, resource, ResourceState::TransferDst); }
    ResourceHandle RenderPassBuilder::CreateTexture(const TextureDesc& desc) { return m_Graph.RegisterResource(desc); }

    // ... (RenderGraph constructor/register methods remain same) ...
    RenderGraph::RenderGraph(LinearAllocator& allocator) : m_Allocator(allocator) { m_Passes.reserve(64); m_Resources.reserve(256); }
    ResourceHandle RenderGraph::RegisterResource(const TextureDesc& desc) { u32 index = (u32)m_Resources.size() + 1; m_Resources.push_back({ desc, 0, true, ResourceState::Undefined, ResourceState::Undefined }); return { index, 0 }; }
    ResourceHandle RenderGraph::ImportResource(const TextureDesc& desc, void* image, void* view, ResourceState initialState) { u32 index = (u32)m_Resources.size() + 1; ResourceNode node{ desc, 0, false, initialState, initialState }; node.image = (VkImage)image; node.view = (VkImageView)view; node.external = true; m_Resources.push_back(node); return { index, 0 }; }
    void RenderGraph::RegisterRead(u32 passIndex, ResourceHandle handle, ResourceState state) { m_Passes[passIndex].reads.push_back(handle); m_Passes[passIndex].readStates.push_back(state); }
    ResourceHandle RenderGraph::RegisterWrite(u32 passIndex, ResourceHandle handle, ResourceState state) { ResourceNode& node = m_Resources[handle.index - 1]; node.version++; ResourceHandle newHandle = { handle.index, node.version }; m_Passes[passIndex].writes.push_back(newHandle); m_Passes[passIndex].writeStates.push_back(state); return newHandle; }
    void RenderGraph::RegisterColorAttachment(u32 passIndex, ResourceHandle handle, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue) { PassAttachment att; att.handle = handle; att.loadOp = loadOp; att.storeOp = storeOp; att.clearValue = clearValue; m_Passes[passIndex].colorAttachments.push_back(att); }
    void RenderGraph::RegisterDepthAttachment(u32 passIndex, ResourceHandle handle, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue) { PassAttachment att; att.handle = handle; att.loadOp = loadOp; att.storeOp = storeOp; att.clearValue = clearValue; m_Passes[passIndex].depthAttachment = att; m_Passes[passIndex].hasDepth = true; }

    void RenderGraph::Compile()
    {
        LH_PROFILE_FUNCTION();
        for (auto& res : m_Resources) { res.currentState = res.initialState; if (res.isTransient) res.currentState = ResourceState::Undefined; }
        for (auto& pass : m_Passes) {
            for (size_t i = 0; i < pass.reads.size(); ++i) {
                ResourceHandle handle = pass.reads[i]; ResourceState targetState = pass.readStates[i]; ResourceNode& res = m_Resources[handle.index - 1];
                if (res.currentState != targetState) { pass.preBarriers.push_back({ handle, res.currentState, targetState }); res.currentState = targetState; }
            }
            for (size_t i = 0; i < pass.writes.size(); ++i) {
                ResourceHandle handle = pass.writes[i]; ResourceState targetState = pass.writeStates[i]; ResourceNode& res = m_Resources[handle.index - 1];
                // Auto-detect depth removed, rely on WriteDepth
                if (res.currentState != targetState) { pass.preBarriers.push_back({ handle, res.currentState, targetState }); res.currentState = targetState; }
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

    void RenderGraph::Execute(VkCommandBuffer cmd) {}

    void RenderGraph::ExecuteParallel(VkCommandBuffer primaryCmd, std::vector<VkCommandBuffer>& outCommandBuffers)
    {
        LH_PROFILE_FUNCTION();
        AllocatePhysicalResources();
        
        JobSystem::Counter jobCounter;
        std::vector<RenderPassJob> jobs(m_Passes.size());
        std::vector<std::vector<AttachmentInfo>> passAttachments(m_Passes.size());

        for (size_t i = 0; i < m_Passes.size(); ++i)
        {
            const auto& pass = m_Passes[i];
            RenderPassJob& job = jobs[i];
            auto& attachments = passAttachments[i];
            
            // 1. Build RenderPassInfo
            for(const auto& att : pass.colorAttachments) {
                ResourceNode& res = m_Resources[att.handle.index - 1];
                AttachmentInfo info{};
                info.ImageView = res.view;
                
                // Map TextureFormat to VkFormat
                if (res.desc.format == TextureFormat::RGBA8_Unorm) info.Format = VK_FORMAT_R8G8B8A8_UNORM;
                else if (res.desc.format == TextureFormat::BGRA8_Unorm) info.Format = VK_FORMAT_B8G8R8A8_UNORM;
                else if (res.desc.format == TextureFormat::D32_Float) info.Format = VK_FORMAT_D32_SFLOAT;
                else if (res.desc.format == TextureFormat::D24_Unorm_S8_Uint) info.Format = VK_FORMAT_D24_UNORM_S8_UINT;
                
                info.LoadOp = att.loadOp; info.StoreOp = att.storeOp; info.ClearValue = att.clearValue;
                info.Layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachments.push_back(info);
            }
            job.ColorAttachments = { attachments.data(), attachments.size() };
            
            if (pass.hasDepth) {
                ResourceNode& res = m_Resources[pass.depthAttachment.handle.index - 1];
                AttachmentInfo info{};
                info.ImageView = res.view;
                info.Format = VK_FORMAT_D32_SFLOAT;
                info.LoadOp = pass.depthAttachment.loadOp; info.StoreOp = pass.depthAttachment.storeOp; info.ClearValue = pass.depthAttachment.clearValue;
                info.Layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                job.DepthAttachment = info;
                job.HasDepth = true;
            }

            // 2. Inject Barriers (Serial)
            for (const auto& barrier : pass.preBarriers) {
                ResourceNode& res = m_Resources[barrier.resource.index - 1];
                auto [srcStage, srcAccess] = GetStateInfo(barrier.before);
                auto [dstStage, dstAccess] = GetStateInfo(barrier.after);
                VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                b.srcStageMask = srcStage; b.srcAccessMask = srcAccess; b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
                b.oldLayout = GetLayout(barrier.before); b.newLayout = GetLayout(barrier.after);
                b.image = res.image; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                if (res.desc.format == TextureFormat::D32_Float) b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
                vkCmdPipelineBarrier2(primaryCmd, &dep);
            }

            // 3. Setup Job
            job.RecordFunction = [this, &pass](VkCommandBuffer cmd) {
                RenderPassContext ctx; ctx.commandBuffer = cmd;
                ctx.GetResource = [this](ResourceHandle h) -> void* { return (h.index > 0 && h.index <= m_Resources.size()) ? &m_Resources[h.index - 1] : nullptr; };
                pass.execute(ctx);
            };
            
            // 4. Kick Job
            JobSystem::Execute([](JobSystem::JobArgs args) {
                RenderPassJob* j = (RenderPassJob*)args.data;
                RenderPassJob::Execute(j);
            }, &job, &jobCounter);
        }

        JobSystem::WaitForCounter(&jobCounter);

        // 5. Execute Secondary Buffers into Primary (WITH Dynamic Rendering Scope)
        for (size_t i = 0; i < jobs.size(); ++i)
        {
            if (jobs[i].CommandBuffer != VK_NULL_HANDLE)
            {
                RenderPassInfo info;
                info.ColorAttachments = jobs[i].ColorAttachments;
                if (jobs[i].HasDepth) info.DepthAttachment = &jobs[i].DepthAttachment;
                
                // Set the flag for secondary buffer execution
                info.Flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
                
                if (!info.ColorAttachments.empty()) {
                    ResourceHandle h = m_Passes[i].colorAttachments[0].handle;
                    info.RenderArea = { {0, 0}, {m_Resources[h.index-1].desc.width, m_Resources[h.index-1].desc.height} };
                } else if (jobs[i].HasDepth) {
                     ResourceHandle h = m_Passes[i].depthAttachment.handle;
                     info.RenderArea = { {0, 0}, {m_Resources[h.index-1].desc.width, m_Resources[h.index-1].desc.height} };
                }

                DynamicRendering::BeginRendering(primaryCmd, info);
                vkCmdExecuteCommands(primaryCmd, 1, &jobs[i].CommandBuffer);
                DynamicRendering::EndRendering(primaryCmd);
            }
        }

        CleanupPhysicalResources();
    }

    void RenderGraph::AllocatePhysicalResources()
    {
        for (auto& res : m_Resources) {
            if (!res.isTransient || res.image != VK_NULL_HANDLE) continue;
            PooledResource pooled = VulkanContext::Get().GetResourceCache().GetTexture(res.desc);
            res.image = pooled.image; res.view = pooled.view; res.allocation = (VmaAllocation_T*)pooled.allocation;
        }
    }

    void RenderGraph::CleanupPhysicalResources()
    {
        for (auto& res : m_Resources) {
            if (res.isTransient && res.image != VK_NULL_HANDLE) {
                PooledResource pooled; pooled.image = res.image; pooled.view = res.view; pooled.allocation = (VmaAllocation)res.allocation; pooled.desc = res.desc;
                VulkanContext::Get().GetResourceCache().ReturnTexture(pooled);
                res.image = VK_NULL_HANDLE; res.view = VK_NULL_HANDLE; res.allocation = nullptr;
            }
        }
    }
}
