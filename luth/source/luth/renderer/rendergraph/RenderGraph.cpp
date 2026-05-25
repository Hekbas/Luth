#include "luthpch.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/rendergraph/IArchiveSink.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/GPUTimerPool.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/RenderPassJob.h"
#include <vma/vk_mem_alloc.h>

namespace Luth::RG
{
    // ── Builder ──

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

    ResourceHandle RenderPassBuilder::ReadStorageImage(ResourceHandle resource)
    {
        m_Graph.RegisterRead(m_PassIndex, resource, ResourceState::ComputeRead);
        return resource;
    }

    ResourceHandle RenderPassBuilder::WriteStorageImage(ResourceHandle resource)
    {
        return m_Graph.RegisterWrite(m_PassIndex, resource, ResourceState::ComputeWrite);
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

    BufferHandle RenderPassBuilder::ReadBuffer(BufferHandle buffer)
    {
        m_Graph.RegisterBufferRead(m_PassIndex, buffer, ResourceState::StorageBufferRead);
        return buffer;
    }

    BufferHandle RenderPassBuilder::WriteBuffer(BufferHandle buffer)
    {
        return m_Graph.RegisterBufferWrite(m_PassIndex, buffer, ResourceState::StorageBufferWrite);
    }

    BufferHandle RenderPassBuilder::ReadIndirectBuffer(BufferHandle buffer)
    {
        m_Graph.RegisterBufferRead(m_PassIndex, buffer, ResourceState::IndirectRead);
        return buffer;
    }

    // ── RenderGraph — Construction & Registration ──

    RenderGraph::RenderGraph(Memory::LinearAllocator& allocator)
        : m_Allocator(allocator)
    {
        m_Passes.reserve(64);
        m_Resources.reserve(256);
        m_Buffers.reserve(64);
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
        return ImportResource(desc, image, view, initialState, /*baseArrayLayer*/ 0u, /*layerCount*/ 1u);
    }

    ResourceHandle RenderGraph::ImportResource(const TextureDesc& desc, void* image, void* view, ResourceState initialState,
                                                u32 baseArrayLayer, u32 layerCount)
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
        node.baseArrayLayer = baseArrayLayer;
        node.layerCount     = layerCount;
        m_Resources.push_back(node);
        return { index, 0 };
    }

    ResourceHandle RenderGraph::ImportResource(const TextureDesc& desc, void* image, void* view,
                                                ResourceState initialState, ResourceState finalState)
    {
        u32 index = (u32)m_Resources.size() + 1;
        ResourceNode node{};
        node.desc = desc;
        node.isTransient = false;
        node.initialState = initialState;
        node.currentState = initialState;
        node.finalState = finalState;
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

    BufferHandle RenderGraph::RegisterBuffer(const BufferDesc& desc)
    {
        u32 index = (u32)m_Buffers.size() + 1;
        BufferNode node{};
        node.desc = desc;
        node.isTransient = true;
        node.initialState = ResourceState::Undefined;
        node.currentState = ResourceState::Undefined;
        m_Buffers.push_back(node);
        return { index, 0 };
    }

    BufferHandle RenderGraph::ImportBuffer(const BufferDesc& desc, void* buffer, ResourceState initialState)
    {
        u32 index = (u32)m_Buffers.size() + 1;
        BufferNode node{};
        node.desc = desc;
        node.isTransient = false;
        node.initialState = initialState;
        node.currentState = initialState;
        node.buffer = (VkBuffer)buffer;
        node.external = true;
        m_Buffers.push_back(node);
        return { index, 0 };
    }

    void RenderGraph::RegisterBufferRead(u32 passIndex, BufferHandle handle, ResourceState state)
    {
        m_Passes[passIndex].bufferReads.push_back(handle);
        m_Passes[passIndex].bufferReadStates.push_back(state);
    }

    BufferHandle RenderGraph::RegisterBufferWrite(u32 passIndex, BufferHandle handle, ResourceState state)
    {
        BufferNode& node = m_Buffers[handle.index - 1];
        node.version++;
        BufferHandle newHandle = { handle.index, node.version };
        m_Passes[passIndex].bufferWrites.push_back(newHandle);
        m_Passes[passIndex].bufferWriteStates.push_back(state);
        return newHandle;
    }

    // ── Compile — Cull → Lifetimes → Barriers ──

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

        for (size_t i = m_Passes.size(); i > 0; --i)
        {
            auto& pass = m_Passes[i - 1];

            // Any pass with color or depth attachments is alive (it renders something)
            if (!pass.colorAttachments.empty() || pass.hasDepth)
                pass.culled = false;

            // Any pass that writes to an external image resource is alive
            for (const auto& handle : pass.writes)
            {
                ResourceNode& res = m_Resources[handle.index - 1];
                if (res.external) { pass.culled = false; break; }
            }

            // Any pass that writes to an external buffer resource is alive
            for (const auto& handle : pass.bufferWrites)
            {
                BufferNode& buf = m_Buffers[handle.index - 1];
                if (buf.external) { pass.culled = false; break; }
            }

            // If alive, un-cull passes that produce what this pass reads.
            // Track `found` per producer search so an already-alive intervening
            // pass doesn't short-circuit before we locate the actual writer of
            // this resource.
            if (!pass.culled)
            {
                for (const auto& readHandle : pass.reads)
                {
                    for (size_t j = i - 1; j > 0; --j)
                    {
                        auto& producer = m_Passes[j - 1];
                        bool found = false;
                        for (const auto& writeHandle : producer.writes)
                        {
                            if (writeHandle.index == readHandle.index)
                            {
                                producer.culled = false;
                                found = true;
                                break;
                            }
                        }
                        if (found) break;
                    }
                }

                for (const auto& readHandle : pass.bufferReads)
                {
                    for (size_t j = i - 1; j > 0; --j)
                    {
                        auto& producer = m_Passes[j - 1];
                        bool found = false;
                        for (const auto& writeHandle : producer.bufferWrites)
                        {
                            if (writeHandle.index == readHandle.index)
                            {
                                producer.culled = false;
                                found = true;
                                break;
                            }
                        }
                        if (found) break;
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

            auto updateImageLifetime = [&](ResourceHandle h)
            {
                ResourceNode& res = m_Resources[h.index - 1];
                if (passIdx < res.firstPass) res.firstPass = (u32)passIdx;
                if (passIdx > res.lastPass) res.lastPass = (u32)passIdx;
            };

            auto updateBufferLifetime = [&](BufferHandle h)
            {
                BufferNode& buf = m_Buffers[h.index - 1];
                if (passIdx < buf.firstPass) buf.firstPass = (u32)passIdx;
                if (passIdx > buf.lastPass) buf.lastPass = (u32)passIdx;
            };

            for (const auto& h : m_Passes[passIdx].reads)       updateImageLifetime(h);
            for (const auto& h : m_Passes[passIdx].writes)      updateImageLifetime(h);
            for (const auto& h : m_Passes[passIdx].bufferReads) updateBufferLifetime(h);
            for (const auto& h : m_Passes[passIdx].bufferWrites) updateBufferLifetime(h);
        }
    }

    void RenderGraph::SolveBarriers()
    {
        // Reset image resource states
        for (auto& res : m_Resources)
        {
            res.currentState = res.initialState;
            if (res.isTransient) res.currentState = ResourceState::Undefined;
            res.lastWriter = UINT32_MAX;
        }

        // Reset buffer resource states
        for (auto& buf : m_Buffers)
        {
            buf.currentState = buf.initialState;
            if (buf.isTransient) buf.currentState = ResourceState::Undefined;
            buf.lastWriter = UINT32_MAX;
        }

        for (size_t passIdx = 0; passIdx < m_Passes.size(); ++passIdx)
        {
            auto& pass = m_Passes[passIdx];
            if (pass.culled) continue;

            // Cross-queue handoff detection — the cross-queue semaphore at submit time provides the actual memory
            // dependency, so the reader's pre-barrier needs TOP_OF_PIPE / NONE on the src side (the writer's stage
            // mask would be graphics-only when emitted on the compute primary). Drives Execute's emission.
            auto isCrossQueue = [&](u32 lastWriter) -> bool {
                if (lastWriter == UINT32_MAX) return false;
                return m_Passes[lastWriter].queueFamily != pass.queueFamily;
            };

            // Image read barriers
            for (size_t i = 0; i < pass.reads.size(); ++i)
            {
                ResourceHandle handle = pass.reads[i];
                ResourceState targetState = pass.readStates[i];
                ResourceNode& res = m_Resources[handle.index - 1];

                if (res.currentState != targetState)
                {
                    pass.preBarriers.push_back({ handle, res.currentState, targetState, isCrossQueue(res.lastWriter) });
                    res.currentState = targetState;
                }
            }

            // WAW: consecutive same-state writes still need an exec barrier (Vulkan ordering rule).
            for (size_t i = 0; i < pass.writes.size(); ++i)
            {
                ResourceHandle handle = pass.writes[i];
                ResourceState targetState = pass.writeStates[i];
                ResourceNode& res = m_Resources[handle.index - 1];

                bool needBarrier = (res.currentState != targetState)
                                || (res.lastWriter != UINT32_MAX && res.lastWriter != (u32)passIdx);
                if (needBarrier)
                {
                    pass.preBarriers.push_back({ handle, res.currentState, targetState, isCrossQueue(res.lastWriter) });
                    res.currentState = targetState;
                }
                res.lastWriter = (u32)passIdx;
            }

            // Buffer read barriers
            for (size_t i = 0; i < pass.bufferReads.size(); ++i)
            {
                BufferHandle handle = pass.bufferReads[i];
                ResourceState targetState = pass.bufferReadStates[i];
                BufferNode& buf = m_Buffers[handle.index - 1];

                if (buf.currentState != targetState)
                {
                    pass.bufferPreBarriers.push_back({ handle, buf.currentState, targetState, isCrossQueue(buf.lastWriter) });
                    buf.currentState = targetState;
                }
            }

            // Buffer write barriers — same WAW policy as images.
            for (size_t i = 0; i < pass.bufferWrites.size(); ++i)
            {
                BufferHandle handle = pass.bufferWrites[i];
                ResourceState targetState = pass.bufferWriteStates[i];
                BufferNode& buf = m_Buffers[handle.index - 1];

                bool needBarrier = (buf.currentState != targetState)
                                || (buf.lastWriter != UINT32_MAX && buf.lastWriter != (u32)passIdx);
                if (needBarrier)
                {
                    pass.bufferPreBarriers.push_back({ handle, buf.currentState, targetState, isCrossQueue(buf.lastWriter) });
                    buf.currentState = targetState;
                }
                buf.lastWriter = (u32)passIdx;
            }
        }

        // External finalState (e.g., swapchain → Present) — postBarrier on the last writer.
        for (size_t i = 0; i < m_Resources.size(); ++i)
        {
            ResourceNode& res = m_Resources[i];
            if (!res.external) continue;
            if (res.finalState == ResourceState::Undefined) continue;
            if (res.lastWriter == UINT32_MAX) continue;
            if (res.currentState == res.finalState) continue;

            ResourceHandle h{ (u32)i + 1, res.version };
            m_Passes[res.lastWriter].postBarriers.push_back({ h, res.currentState, res.finalState });
            res.currentState = res.finalState;
        }
    }

    // ── State → Vulkan Mapping ──

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
            case ResourceState::ComputeRead:            return { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT };
            case ResourceState::ComputeWrite:           return { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT };
            case ResourceState::StorageBufferRead:      return { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT };
            case ResourceState::StorageBufferWrite:     return { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT };
            case ResourceState::IndirectRead:           return { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT };
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
            case ResourceState::ComputeRead:            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            case ResourceState::ComputeWrite:           return VK_IMAGE_LAYOUT_GENERAL;
            // Buffer states have no image layout — return UNDEFINED (never used for image barriers)
            default:                                    return VK_IMAGE_LAYOUT_UNDEFINED;
        }
    }

    static VkFormat GetVkFormat(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::RGBA8_Unorm:       return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::BGRA8_Unorm:       return VK_FORMAT_B8G8R8A8_UNORM;
            case TextureFormat::R8_Unorm:          return VK_FORMAT_R8_UNORM;
            case TextureFormat::RGBA16_Float:      return VK_FORMAT_R16G16B16A16_SFLOAT;
            case TextureFormat::RG16_Float:        return VK_FORMAT_R16G16_SFLOAT;
            case TextureFormat::R32_Float:         return VK_FORMAT_R32_SFLOAT;
            case TextureFormat::D32_Float:         return VK_FORMAT_D32_SFLOAT;
            case TextureFormat::D24_Unorm_S8_Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
            case TextureFormat::R32_Uint:          return VK_FORMAT_R32_UINT;
            case TextureFormat::R16_Uint:          return VK_FORMAT_R16_UINT;
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

    // ── Execute — two-phase: parallel secondary recording, serial primary emission ──
    //
    // Phase 1: dispatch one RenderPassJob per graphics pass against a single
    //          counter; render fiber yields once per frame regardless of pass
    //          count. SetSerialize(true) falls back to per-pass dispatch+wait
    //          when pass lambdas touch shared state (FrameDebugger capture).
    // Phase 2: emit primary cmd buffer in pass order — batched barriers,
    //          compute exec inline / graphics BeginRendering + ExecuteCommands
    //          + EndRendering. Barrier order and pass-order semantics preserved.
    //
    // Compute passes bypass phase 1 (would serialize on primary anyway).

    namespace {
        struct PassExecState
        {
            // Phase-1 attachments info has to outlive the WaitForCounter so
            // phase 2 can read it; can't be stack-local to the build loop.
            static constexpr u32 k_MaxColorAtt = 8;
            AttachmentInfo colorAttachmentsBuf[k_MaxColorAtt];
            u32 colorAttachmentCount = 0;
            AttachmentInfo depthInfo{};
            bool hasDepth = false;
            RenderPassJob job{}; // Graphics only; CommandBuffer set by phase 1.
        };
    }

    bool RenderGraph::Execute(QueueRecorders recorders, Luth::GPUTimerPool* timers)
    {
        LH_PROFILE_FUNCTION();
        AllocatePhysicalResources();

        // Timer query pool is shared across queues per arch/multi-queue.md (timestampValidBits compatibility
        // asserted at startup). Reset is fine on either queue — using gA keeps the cmd ordering predictable.
        if (timers) timers->ResetForFrame(recorders.gA);

        // hasComputeWork = "did any pass route to compute"; returned to SubmitView so it can skip the compute
        // submit when the graph stays graphics-only. seenAsyncCompute drives the graphics-A→graphics-B split:
        // graphics passes before the first AsyncCompute pass go to gA; after, gB. Inter-frame the split resets.
        bool hasComputeWork  = false;
        bool seenAsyncCompute = false;

        // Indexed by m_Passes index; slots for culled / compute passes stay default-constructed and unused.
        std::vector<PassExecState> passStates(m_Passes.size());

        // ── Phase 1: secondary cmd buffer recording ──
        const bool serializeDispatch = m_SerializeDispatch;
        JobSystem::Counter recordCounter;

        for (size_t i = 0; i < m_Passes.size(); ++i)
        {
            auto& pass = m_Passes[i];
            if (pass.culled || pass.isCompute) continue;

            PassExecState& state = passStates[i];

            LH_CORE_ASSERT(pass.colorAttachments.size() <= PassExecState::k_MaxColorAtt,
                "Too many color attachments!");
            for (const auto& att : pass.colorAttachments)
            {
                ResourceNode& res = m_Resources[att.handle.index - 1];
                AttachmentInfo& info = state.colorAttachmentsBuf[state.colorAttachmentCount++];
                info = {};
                info.ImageView  = res.view;
                info.Format     = GetVkFormat(res.desc.format);
                info.LoadOp     = att.loadOp;
                info.StoreOp    = att.storeOp;
                info.ClearValue = att.clearValue;
                info.Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }

            if (pass.hasDepth)
            {
                ResourceNode& res = m_Resources[pass.depthAttachment.handle.index - 1];
                state.depthInfo.ImageView  = res.view;
                state.depthInfo.Format     = GetVkFormat(res.desc.format);
                state.depthInfo.LoadOp     = pass.depthAttachment.loadOp;
                state.depthInfo.StoreOp    = pass.depthAttachment.storeOp;
                state.depthInfo.ClearValue = pass.depthAttachment.clearValue;
                state.depthInfo.Layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                state.hasDepth = true;
            }

            state.job.ColorAttachments = { state.colorAttachmentsBuf, state.colorAttachmentCount };
            if (state.hasDepth)
            {
                state.job.DepthAttachment = state.depthInfo;
                state.job.HasDepth = true;
            }

            state.job.RecordFunction = [this, &pass, i](VkCommandBuffer cmd)
            {
                RenderPassContext ctx;
                ctx.commandBuffer = cmd;
                ctx.passIndex     = (u32)i;
                ctx.GetResource = [this](ResourceHandle h) -> void*
                {
                    return (h.index > 0 && h.index <= m_Resources.size()) ? &m_Resources[h.index - 1] : nullptr;
                };
                ctx.GetBuffer = [this](BufferHandle h) -> void*
                {
                    return (h.index > 0 && h.index <= m_Buffers.size()) ? &m_Buffers[h.index - 1] : nullptr;
                };
                pass.execute(ctx);
            };

            if (serializeDispatch)
            {
                JobSystem::Counter perPassCounter;
                JobSystem::Execute([](JobSystem::JobArgs args) {
                    RenderPassJob* j = (RenderPassJob*)args.data;
                    RenderPassJob::Execute(j);
                }, &state.job, &perPassCounter, "RGPassRecord");
                JobSystem::WaitForCounter(&perPassCounter);
            }
            else
            {
                JobSystem::Execute([](JobSystem::JobArgs args) {
                    RenderPassJob* j = (RenderPassJob*)args.data;
                    RenderPassJob::Execute(j);
                }, &state.job, &recordCounter, "RGPassRecord");
            }
        }

        // Single yield for parallel mode; serial mode already waited per pass.
        if (!serializeDispatch)
            JobSystem::WaitForCounter(&recordCounter);

        // ── Phase 2: primary cmd buffer emission (pass order) ──
        u32 timerPassIdx = 0;

        for (size_t i = 0; i < m_Passes.size(); ++i)
        {
            auto& pass = m_Passes[i];
            if (pass.culled) continue;

            LH_PROFILE_SCOPE_DYNAMIC(pass.name);

            // Per-pass primary selection: AsyncCompute → compute; Graphics → gA before first AsyncCompute, gB after.
            // First AsyncCompute encountered also flips hasComputeWork (returned to SubmitView) and seenAsyncCompute
            // (drives the gA/gB split for subsequent graphics passes).
            VkCommandBuffer primaryCmd;
            if (pass.queueFamily == QueueFamily::AsyncCompute)
            {
                primaryCmd       = recorders.compute;
                hasComputeWork   = true;
                seenAsyncCompute = true;
            }
            else
            {
                primaryCmd = seenAsyncCompute ? recorders.gB : recorders.gA;
            }

            // Batched pre-barriers (image + buffer combined into one call). Cross-queue handoffs detected during
            // SolveBarriers carry b.crossQueueSrc — in that case the src stage / access become TOP_OF_PIPE / NONE
            // since the cross-queue semaphore at submit time already supplies the memory dependency per spec.
            static constexpr u32 k_MaxBarriers = 16;
            LH_CORE_ASSERT(pass.preBarriers.size() <= k_MaxBarriers, "Too many image barriers per pass!");
            LH_CORE_ASSERT(pass.bufferPreBarriers.size() <= k_MaxBarriers, "Too many buffer barriers per pass!");

            VkImageMemoryBarrier2  imgBarriers[k_MaxBarriers];
            VkBufferMemoryBarrier2 bufBarriers[k_MaxBarriers];
            u32 imgBarrierCount = 0;
            u32 bufBarrierCount = 0;

            for (const auto& b : pass.preBarriers)
            {
                ResourceNode& res = m_Resources[b.resource.index - 1];
                auto [srcStage, srcAccess] = GetStateInfo(b.before);
                auto [dstStage, dstAccess] = GetStateInfo(b.after);
                if (b.crossQueueSrc) { srcStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; srcAccess = 0; }

                VkImageMemoryBarrier2& vkBarrier = imgBarriers[imgBarrierCount++];
                vkBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                vkBarrier.srcStageMask        = srcStage;
                vkBarrier.srcAccessMask       = srcAccess;
                vkBarrier.dstStageMask        = dstStage;
                vkBarrier.dstAccessMask       = dstAccess;
                vkBarrier.oldLayout           = GetLayout(b.before);
                vkBarrier.newLayout           = GetLayout(b.after);
                vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                vkBarrier.image               = res.image;
                vkBarrier.subresourceRange    = { GetAspect(res.desc.format), 0, 1, res.baseArrayLayer, res.layerCount };
            }

            for (const auto& b : pass.bufferPreBarriers)
            {
                BufferNode& buf = m_Buffers[b.resource.index - 1];
                auto [srcStage, srcAccess] = GetStateInfo(b.before);
                auto [dstStage, dstAccess] = GetStateInfo(b.after);
                if (b.crossQueueSrc) { srcStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; srcAccess = 0; }

                VkBufferMemoryBarrier2& vkBarrier = bufBarriers[bufBarrierCount++];
                vkBarrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
                vkBarrier.srcStageMask        = srcStage;
                vkBarrier.srcAccessMask       = srcAccess;
                vkBarrier.dstStageMask        = dstStage;
                vkBarrier.dstAccessMask       = dstAccess;
                vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                vkBarrier.buffer              = buf.buffer;
                vkBarrier.offset              = 0;
                vkBarrier.size                = VK_WHOLE_SIZE;
            }

            if (imgBarrierCount > 0 || bufBarrierCount > 0)
            {
                VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep.imageMemoryBarrierCount  = imgBarrierCount;
                dep.pImageMemoryBarriers     = imgBarriers;
                dep.bufferMemoryBarrierCount = bufBarrierCount;
                dep.pBufferMemoryBarriers    = bufBarriers;
                vkCmdPipelineBarrier2(primaryCmd, &dep);
            }

            // After pass body + archive sink (sink reads attachments before transition).
            auto emitPostBarriers = [&]()
            {
                if (pass.postBarriers.empty()) return;

                static constexpr u32 k_MaxPostBarriers = 16;
                LH_CORE_ASSERT(pass.postBarriers.size() <= k_MaxPostBarriers, "Too many post-barriers per pass!");
                VkImageMemoryBarrier2 imgBarriers[k_MaxPostBarriers];
                u32 imgBarrierCount = 0;

                for (const auto& b : pass.postBarriers)
                {
                    ResourceNode& res = m_Resources[b.resource.index - 1];
                    auto [srcStage, srcAccess] = GetStateInfo(b.before);
                    auto [dstStage, dstAccess] = GetStateInfo(b.after);
                    if (b.crossQueueSrc) { srcStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; srcAccess = 0; }

                    VkImageMemoryBarrier2& vkBarrier = imgBarriers[imgBarrierCount++];
                    vkBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                    vkBarrier.srcStageMask        = srcStage;
                    vkBarrier.srcAccessMask       = srcAccess;
                    vkBarrier.dstStageMask        = dstStage;
                    vkBarrier.dstAccessMask       = dstAccess;
                    vkBarrier.oldLayout           = GetLayout(b.before);
                    vkBarrier.newLayout           = GetLayout(b.after);
                    vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    vkBarrier.image               = res.image;
                    vkBarrier.subresourceRange    = { GetAspect(res.desc.format), 0, 1, res.baseArrayLayer, res.layerCount };
                }
                VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep.imageMemoryBarrierCount = imgBarrierCount;
                dep.pImageMemoryBarriers    = imgBarriers;
                vkCmdPipelineBarrier2(primaryCmd, &dep);
            };

            // Compute pass: inline on primary (bypasses phase 1)
            if (pass.isCompute)
            {
                if (timers) timers->WriteTimestamp(primaryCmd, timerPassIdx, true);

                RenderPassContext ctx;
                ctx.commandBuffer = primaryCmd;
                ctx.passIndex     = (u32)i;
                ctx.GetResource = [this](ResourceHandle h) -> void*
                {
                    return (h.index > 0 && h.index <= m_Resources.size()) ? &m_Resources[h.index - 1] : nullptr;
                };
                ctx.GetBuffer = [this](BufferHandle h) -> void*
                {
                    return (h.index > 0 && h.index <= m_Buffers.size()) ? &m_Buffers[h.index - 1] : nullptr;
                };
                pass.execute(ctx);

                if (timers) timers->WriteTimestamp(primaryCmd, timerPassIdx, false);

                if (m_ArchiveSink) m_ArchiveSink->OnPassExecuted((u32)i, *this, primaryCmd, pass.queueFamily);

                emitPostBarriers();

                timerPassIdx++;
                continue;
            }

            // Graphics pass: execute the pre-recorded secondary into the primary
            PassExecState& state = passStates[i];
            if (state.job.CommandBuffer != VK_NULL_HANDLE)
            {
                RenderPassInfo rpInfo{};
                rpInfo.ColorAttachments = { state.colorAttachmentsBuf, state.colorAttachmentCount };
                if (state.hasDepth) rpInfo.DepthAttachment = &state.depthInfo;
                rpInfo.Flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;

                if (state.colorAttachmentCount > 0)
                {
                    ResourceHandle h = pass.colorAttachments[0].handle;
                    rpInfo.RenderArea = { {0, 0}, { m_Resources[h.index - 1].desc.width, m_Resources[h.index - 1].desc.height } };
                }
                else if (pass.hasDepth)
                {
                    ResourceHandle h = pass.depthAttachment.handle;
                    rpInfo.RenderArea = { {0, 0}, { m_Resources[h.index - 1].desc.width, m_Resources[h.index - 1].desc.height } };
                }

                if (timers) timers->WriteTimestamp(primaryCmd, timerPassIdx, true);

                DynamicRendering::BeginRendering(primaryCmd, rpInfo);
                vkCmdExecuteCommands(primaryCmd, 1, &state.job.CommandBuffer);
                DynamicRendering::EndRendering(primaryCmd);

                if (timers) timers->WriteTimestamp(primaryCmd, timerPassIdx, false);

                if (m_ArchiveSink) m_ArchiveSink->OnPassExecuted((u32)i, *this, primaryCmd, pass.queueFamily);

                emitPostBarriers();
            }

            timerPassIdx++;
        }

        CleanupPhysicalResources();
        return hasComputeWork;
    }

    // ── Physical Resource Management ──

    void RenderGraph::AllocatePhysicalResources()
    {
        // Allocate transient image resources
        for (auto& res : m_Resources)
        {
            if (!res.isTransient || res.image != VK_NULL_HANDLE) continue;
            PooledResource pooled = VulkanContext::Get().GetResourceCache().GetTexture(res.desc);
            res.image = pooled.image;
            res.view  = pooled.view;
            res.allocation = (VmaAllocation_T*)pooled.allocation;
        }

        // Allocate transient buffer resources
        for (auto& buf : m_Buffers)
        {
            if (!buf.isTransient || buf.buffer != VK_NULL_HANDLE) continue;
            PooledBuffer pooled = VulkanContext::Get().GetResourceCache().GetBuffer(buf.desc);
            buf.buffer = pooled.buffer;
            buf.allocation = (VmaAllocation_T*)pooled.allocation;
        }
    }

    void RenderGraph::CleanupPhysicalResources()
    {
        // Return transient image resources to pool
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

        // Return transient buffer resources to pool
        for (auto& buf : m_Buffers)
        {
            if (buf.isTransient && buf.buffer != VK_NULL_HANDLE)
            {
                PooledBuffer pooled;
                pooled.buffer = buf.buffer;
                pooled.allocation = (VmaAllocation)buf.allocation;
                pooled.desc = buf.desc;
                VulkanContext::Get().GetResourceCache().ReturnBuffer(pooled);
                buf.buffer = VK_NULL_HANDLE;
                buf.allocation = nullptr;
            }
        }
    }
}
