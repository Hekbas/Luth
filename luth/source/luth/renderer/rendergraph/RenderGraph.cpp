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
    // ===================================================================================
    // Builder (unchanged)
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

    ResourceHandle RenderPassBuilder::WriteDepth(ResourceHandle resource, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue)
    {
        ResourceHandle newHandle = m_Graph.RegisterWrite(m_PassIndex, resource, ResourceState::DepthStencilAttachment);
        m_Graph.RegisterDepthAttachment(m_PassIndex, newHandle, loadOp, storeOp, clearValue);
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
    // RenderGraph — Construction & Registration
    // ===================================================================================

    RenderGraph::RenderGraph(Memory::LinearAllocator& allocator)
        : m_Allocator(allocator)
    {
        m_Passes.reserve(64);
        m_Resources.reserve(256);
    }

    ResourceHandle RenderGraph::RegisterResource(const TextureDesc& desc)
    {
        u32 index = (u32)m_Resources.size() + 1;
        ResourceNode node{};
        node.desc = desc;
        node.isTransient = true;
        node.initialState = ResourceState::Undefined;
        node.currentState = ResourceState::Undefined;
        m_Resources.push_back(node);
        return { index, 0 };
    }

    ResourceHandle RenderGraph::ImportResource(const TextureDesc& desc, void* image, void* view, ResourceState initialState)
    {
        u32 index = (u32)m_Resources.size() + 1;
        ResourceNode node{};
        node.desc = desc;
        node.isTransient = false;
        node.initialState = initialState;
        node.currentState = initialState;
        node.image = (VkImage)image;
        node.view = (VkImageView)view;
        node.external = true;
        m_Resources.push_back(node);
        return { index, 0 };
    }

    void RenderGraph::RegisterRead(u32 passIndex, ResourceHandle handle, ResourceState state)
    {
        m_Passes[passIndex].reads.push_back(handle);
        m_Passes[passIndex].readStates.push_back(state);
    }

    ResourceHandle RenderGraph::RegisterWrite(u32 passIndex, ResourceHandle handle, ResourceState state)
    {
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

    // ===================================================================================
    // Compile — Cull → Lifetimes → Barriers
    // ===================================================================================

    void RenderGraph::Compile()
    {
        LH_PROFILE_FUNCTION();

        CullDeadPasses();
        ComputeLifetimes();
        SolveBarriers();
    }

    void RenderGraph::CullDeadPasses()
    {
        // Mark all passes as potentially culled
        for (auto& pass : m_Passes) pass.culled = true;

        // Walk backwards: any pass that writes to a resource read by a later
        // non-culled pass (or writes an external/imported resource) is alive.
        // For now: keep all passes with writes to external resources or that
        // have any color/depth attachments (rendering passes are never dead).
        for (size_t i = m_Passes.size(); i > 0; --i)
        {
            auto& pass = m_Passes[i - 1];
            
            // Any pass with color or depth attachments is alive (it renders something)
            if (!pass.colorAttachments.empty() || pass.hasDepth)
            {
                pass.culled = false;
            }

            // Any pass that writes to an external resource is alive
            for (const auto& handle : pass.writes)
            {
                ResourceNode& res = m_Resources[handle.index - 1];
                if (res.external)
                {
                    pass.culled = false;
                    break;
                }
            }

            // If alive, mark all resources it reads as "needed" by un-culling
            // the passes that produce them
            if (!pass.culled)
            {
                for (const auto& readHandle : pass.reads)
                {
                    // Find the pass that writes this resource (backwards)
                    for (size_t j = i - 1; j > 0; --j)
                    {
                        auto& producer = m_Passes[j - 1];
                        for (const auto& writeHandle : producer.writes)
                        {
                            if (writeHandle.index == readHandle.index)
                            {
                                producer.culled = false;
                                break;
                            }
                        }
                        if (!producer.culled) break;
                    }
                }
            }
        }
    }

    void RenderGraph::ComputeLifetimes()
    {
        for (size_t passIdx = 0; passIdx < m_Passes.size(); ++passIdx)
        {
            if (m_Passes[passIdx].culled) continue;

            auto updateLifetime = [&](ResourceHandle h)
            {
                ResourceNode& res = m_Resources[h.index - 1];
                if (passIdx < res.firstPass) res.firstPass = (u32)passIdx;
                if (passIdx > res.lastPass) res.lastPass = (u32)passIdx;
            };

            for (const auto& h : m_Passes[passIdx].reads) updateLifetime(h);
            for (const auto& h : m_Passes[passIdx].writes) updateLifetime(h);
        }
    }

    void RenderGraph::SolveBarriers()
    {
        // Reset resource states
        for (auto& res : m_Resources)
        {
            res.currentState = res.initialState;
            if (res.isTransient) res.currentState = ResourceState::Undefined;
        }

        // Generate barriers per pass
        for (auto& pass : m_Passes)
        {
            if (pass.culled) continue;

            // Read barriers
            for (size_t i = 0; i < pass.reads.size(); ++i)
            {
                ResourceHandle handle = pass.reads[i];
                ResourceState targetState = pass.readStates[i];
                ResourceNode& res = m_Resources[handle.index - 1];

                if (res.currentState != targetState)
                {
                    pass.preBarriers.push_back({ handle, res.currentState, targetState });
                    res.currentState = targetState;
                }
            }

            // Write barriers
            for (size_t i = 0; i < pass.writes.size(); ++i)
            {
                ResourceHandle handle = pass.writes[i];
                ResourceState targetState = pass.writeStates[i];
                ResourceNode& res = m_Resources[handle.index - 1];

                if (res.currentState != targetState)
                {
                    pass.preBarriers.push_back({ handle, res.currentState, targetState });
                    res.currentState = targetState;
                }
            }
        }
    }

    // ===================================================================================
    // State → Vulkan Mapping
    // ===================================================================================

    static std::pair<VkPipelineStageFlags2, VkAccessFlags2> GetStateInfo(ResourceState state)
    {
        switch (state)
        {
            case ResourceState::Undefined:              return { VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
            case ResourceState::ColorAttachment:        return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
            case ResourceState::DepthStencilAttachment: return { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
            case ResourceState::TransferDst:            return { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
            case ResourceState::TransferSrc:            return { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT };
            case ResourceState::ShaderResource:         return { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT };
            case ResourceState::Present:                return { VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0 };
            default:                                    return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
        }
    }

    static VkImageLayout GetLayout(ResourceState state)
    {
        switch (state)
        {
            case ResourceState::Undefined:              return VK_IMAGE_LAYOUT_UNDEFINED;
            case ResourceState::ColorAttachment:        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            case ResourceState::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            case ResourceState::TransferDst:            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            case ResourceState::TransferSrc:            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            case ResourceState::ShaderResource:         return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            case ResourceState::Present:                return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            default:                                    return VK_IMAGE_LAYOUT_UNDEFINED;
        }
    }

    static VkFormat GetVkFormat(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::RGBA8_Unorm:       return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::BGRA8_Unorm:       return VK_FORMAT_B8G8R8A8_UNORM;
            case TextureFormat::D32_Float:         return VK_FORMAT_D32_SFLOAT;
            case TextureFormat::D24_Unorm_S8_Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
            default:                               return VK_FORMAT_R8G8B8A8_UNORM;
        }
    }

    static VkImageAspectFlags GetAspect(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::D32_Float:         return VK_IMAGE_ASPECT_DEPTH_BIT;
            case TextureFormat::D24_Unorm_S8_Uint: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:                               return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    // ===================================================================================
    // Execute — Serial pass loop, parallel inner recording
    // ===================================================================================
    //
    // For each non-culled pass:
    //   1. Batch all pre-barriers into a single vkCmdPipelineBarrier2
    //   2. Build RenderPassInfo (Dynamic Rendering)
    //   3. Dispatch recording as RenderPassJob (records into secondary cmd buffer)
    //   4. WaitForCounter (may inline-execute per V5)
    //   5. BeginRendering + ExecuteCommands + EndRendering on primary cmd
    //
    // This ensures barriers are correct (serial ordering) while recording
    // is parallelized (worker threads record secondary buffers).

    void RenderGraph::Execute(VkCommandBuffer primaryCmd)
    {
        LH_PROFILE_FUNCTION();
        AllocatePhysicalResources();

        for (size_t i = 0; i < m_Passes.size(); ++i)
        {
            auto& pass = m_Passes[i];
            if (pass.culled) continue;

            // ── Step 1: Batched Barriers ──
            if (!pass.preBarriers.empty())
            {
                std::vector<VkImageMemoryBarrier2> barriers;
                barriers.reserve(pass.preBarriers.size());

                for (const auto& b : pass.preBarriers)
                {
                    ResourceNode& res = m_Resources[b.resource.index - 1];
                    auto [srcStage, srcAccess] = GetStateInfo(b.before);
                    auto [dstStage, dstAccess] = GetStateInfo(b.after);

                    VkImageMemoryBarrier2 vkBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                    vkBarrier.srcStageMask  = srcStage;
                    vkBarrier.srcAccessMask = srcAccess;
                    vkBarrier.dstStageMask  = dstStage;
                    vkBarrier.dstAccessMask = dstAccess;
                    vkBarrier.oldLayout     = GetLayout(b.before);
                    vkBarrier.newLayout     = GetLayout(b.after);
                    vkBarrier.image         = res.image;
                    vkBarrier.subresourceRange = { GetAspect(res.desc.format), 0, 1, 0, 1 };
                    barriers.push_back(vkBarrier);
                }

                VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep.imageMemoryBarrierCount = (u32)barriers.size();
                dep.pImageMemoryBarriers    = barriers.data();
                vkCmdPipelineBarrier2(primaryCmd, &dep);
            }

            // ── Step 2: Build RenderPassInfo ──
            std::vector<AttachmentInfo> colorAttachments;
            for (const auto& att : pass.colorAttachments)
            {
                ResourceNode& res = m_Resources[att.handle.index - 1];
                AttachmentInfo info{};
                info.ImageView = res.view;
                info.Format    = GetVkFormat(res.desc.format);
                info.LoadOp    = att.loadOp;
                info.StoreOp   = att.storeOp;
                info.ClearValue = att.clearValue;
                info.Layout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachments.push_back(info);
            }

            AttachmentInfo depthInfo{};
            if (pass.hasDepth)
            {
                ResourceNode& res = m_Resources[pass.depthAttachment.handle.index - 1];
                depthInfo.ImageView = res.view;
                depthInfo.Format    = GetVkFormat(res.desc.format);
                depthInfo.LoadOp    = pass.depthAttachment.loadOp;
                depthInfo.StoreOp   = pass.depthAttachment.storeOp;
                depthInfo.ClearValue = pass.depthAttachment.clearValue;
                depthInfo.Layout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }

            // ── Step 3: Dispatch RenderPassJob ──
            RenderPassJob job{};
            job.ColorAttachments = { colorAttachments.data(), colorAttachments.size() };
            if (pass.hasDepth)
            {
                job.DepthAttachment = depthInfo;
                job.HasDepth = true;
            }

            job.RecordFunction = [this, &pass](VkCommandBuffer cmd)
            {
                RenderPassContext ctx;
                ctx.commandBuffer = cmd;
                ctx.GetResource = [this](ResourceHandle h) -> void*
                {
                    return (h.index > 0 && h.index <= m_Resources.size()) ? &m_Resources[h.index - 1] : nullptr;
                };
                pass.execute(ctx);
            };

            JobSystem::Counter jobCounter;
            JobSystem::Execute([](JobSystem::JobArgs args) {
                RenderPassJob* j = (RenderPassJob*)args.data;
                RenderPassJob::Execute(j);
            }, &job, &jobCounter);

            JobSystem::WaitForCounter(&jobCounter);

            // ── Step 4: Execute secondary into primary ──
            if (job.CommandBuffer != VK_NULL_HANDLE)
            {
                RenderPassInfo rpInfo{};
                rpInfo.ColorAttachments = { colorAttachments.data(), colorAttachments.size() };
                if (pass.hasDepth) rpInfo.DepthAttachment = &depthInfo;
                rpInfo.Flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;

                // Determine render area from first color attachment or depth
                if (!colorAttachments.empty())
                {
                    ResourceHandle h = pass.colorAttachments[0].handle;
                    rpInfo.RenderArea = { {0, 0}, { m_Resources[h.index - 1].desc.width, m_Resources[h.index - 1].desc.height } };
                }
                else if (pass.hasDepth)
                {
                    ResourceHandle h = pass.depthAttachment.handle;
                    rpInfo.RenderArea = { {0, 0}, { m_Resources[h.index - 1].desc.width, m_Resources[h.index - 1].desc.height } };
                }

                DynamicRendering::BeginRendering(primaryCmd, rpInfo);
                vkCmdExecuteCommands(primaryCmd, 1, &job.CommandBuffer);
                DynamicRendering::EndRendering(primaryCmd);
            }
        }

        CleanupPhysicalResources();
    }

    // ===================================================================================
    // Physical Resource Management
    // ===================================================================================

    void RenderGraph::AllocatePhysicalResources()
    {
        for (auto& res : m_Resources)
        {
            if (!res.isTransient || res.image != VK_NULL_HANDLE) continue;
            PooledResource pooled = VulkanContext::Get().GetResourceCache().GetTexture(res.desc);
            res.image = pooled.image;
            res.view  = pooled.view;
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
                pooled.view  = res.view;
                pooled.allocation = (VmaAllocation)res.allocation;
                pooled.desc  = res.desc;
                VulkanContext::Get().GetResourceCache().ReturnTexture(pooled);
                res.image = VK_NULL_HANDLE;
                res.view  = VK_NULL_HANDLE;
                res.allocation = nullptr;
            }
        }
    }
}
