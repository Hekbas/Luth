#include "luthpch.h"
#include "luth/renderer/FrameDebugger.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/rendergraph/FrameEventTree.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/core/FrameData.h"

#include <vma/vk_mem_alloc.h>
#include <backends/imgui_impl_vulkan.h>

namespace Luth
{
    // ---- Local helpers (mirror RenderGraph.cpp internal mappings) ----

    namespace
    {
        VkFormat ToVkFormat(RG::TextureFormat f)
        {
            switch (f)
            {
                case RG::TextureFormat::RGBA8_Unorm:       return VK_FORMAT_R8G8B8A8_UNORM;
                case RG::TextureFormat::BGRA8_Unorm:       return VK_FORMAT_B8G8R8A8_UNORM;
                case RG::TextureFormat::R8_Unorm:          return VK_FORMAT_R8_UNORM;
                case RG::TextureFormat::RGBA16_Float:      return VK_FORMAT_R16G16B16A16_SFLOAT;
                case RG::TextureFormat::R32_Float:         return VK_FORMAT_R32_SFLOAT;
                case RG::TextureFormat::D32_Float:         return VK_FORMAT_D32_SFLOAT;
                case RG::TextureFormat::D24_Unorm_S8_Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
                case RG::TextureFormat::R32_Uint:          return VK_FORMAT_R32_UINT;
                default:                                   return VK_FORMAT_R8G8B8A8_UNORM;
            }
        }

        bool IsDepthFormat(RG::TextureFormat f)
        {
            return f == RG::TextureFormat::D32_Float || f == RG::TextureFormat::D24_Unorm_S8_Uint;
        }

        VkImageLayout StateToLayout(RG::ResourceState s)
        {
            using S = RG::ResourceState;
            switch (s)
            {
                case S::ColorAttachment:        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                case S::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                case S::ShaderResource:         return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                case S::ComputeRead:            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                case S::ComputeWrite:           return VK_IMAGE_LAYOUT_GENERAL;
                case S::TransferSrc:            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                case S::TransferDst:            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                default:                         return VK_IMAGE_LAYOUT_UNDEFINED;
            }
        }

        struct StageAccess { VkPipelineStageFlags2 stage; VkAccessFlags2 access; };
        StageAccess StateToStageAccess(RG::ResourceState s)
        {
            using S = RG::ResourceState;
            switch (s)
            {
                case S::ColorAttachment:
                    return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
                case S::DepthStencilAttachment:
                    return { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
                case S::ComputeWrite:
                    return { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT };
                case S::ShaderResource:
                case S::ComputeRead:
                    return { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT };
                default:
                    return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
            }
        }

        // Allocate a fresh ArchivedImage matching the source's subresource extent.
        // For per-layer imports (e.g. cascade slice) the archive is sized to layerCount layers.
        u32 AllocateArchiveImage(VkDevice device, VmaAllocator /*allocator*/,
                                  const RG::RenderGraph::ResourceNode& src,
                                  RG::CapturedFrame& frame)
        {
            RG::ArchivedImage a{};
            a.name    = src.desc.name;
            a.width   = src.desc.width;
            a.height  = src.desc.height;
            a.layers  = src.layerCount;
            a.mips    = 1;
            a.format  = ToVkFormat(src.desc.format);
            a.isDepth = IsDepthFormat(src.desc.format);

            VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ci.imageType     = VK_IMAGE_TYPE_2D;
            ci.extent        = { src.desc.width, src.desc.height, 1 };
            ci.mipLevels     = 1;
            ci.arrayLayers   = src.layerCount;
            ci.format        = a.format;
            ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ci.samples       = VK_SAMPLE_COUNT_1_BIT;
            ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

            a.alloc = VulkanAllocator::AllocateImage(ci, VMA_MEMORY_USAGE_AUTO, a.image);

            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image    = a.image;
            vci.viewType = (src.layerCount > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            vci.format   = a.format;
            vci.subresourceRange.aspectMask     = a.isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            vci.subresourceRange.baseMipLevel   = 0;
            vci.subresourceRange.levelCount     = 1;
            vci.subresourceRange.baseArrayLayer = 0;
            vci.subresourceRange.layerCount     = src.layerCount;
            vkCreateImageView(device, &vci, nullptr, &a.view);

            a.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            u32 idx = (u32)frame.archivedImages.size();
            frame.archivedImages.push_back(std::move(a));
            return idx;
        }

        // Bracket a vkCmdCopyImage with the layout transitions needed to:
        //   * pull the source out of its post-pass state into TransferSrc, and back,
        //   * push the archive from Undefined → TransferDst → ShaderReadOnly.
        // The source is restored to its original layout so the RG's compile-time
        // barrier solver, which is unaware of this hook, stays consistent.
        void EmitArchiveCopy(VkCommandBuffer cmd,
                              const RG::RenderGraph::ResourceNode& src,
                              RG::ResourceState srcState,
                              RG::ArchivedImage& dst)
        {
            VkImageAspectFlags aspect = dst.isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageLayout srcLayout = StateToLayout(srcState);
            StageAccess srcSA = StateToStageAccess(srcState);

            VkImageMemoryBarrier2 pre[2]{};
            pre[0].sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            pre[0].srcStageMask         = srcSA.stage;
            pre[0].srcAccessMask        = srcSA.access;
            pre[0].dstStageMask         = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            pre[0].dstAccessMask        = VK_ACCESS_2_TRANSFER_READ_BIT;
            pre[0].oldLayout            = srcLayout;
            pre[0].newLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            pre[0].image                = src.image;
            pre[0].subresourceRange     = { aspect, 0, 1, src.baseArrayLayer, src.layerCount };
            pre[0].srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
            pre[0].dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;

            pre[1].sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            pre[1].srcStageMask         = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            pre[1].srcAccessMask        = 0;
            pre[1].dstStageMask         = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            pre[1].dstAccessMask        = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            pre[1].oldLayout            = dst.currentLayout;
            pre[1].newLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            pre[1].image                = dst.image;
            pre[1].subresourceRange     = { aspect, 0, 1, 0, dst.layers };
            pre[1].srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
            pre[1].dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depPre{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depPre.imageMemoryBarrierCount = 2;
            depPre.pImageMemoryBarriers    = pre;
            vkCmdPipelineBarrier2(cmd, &depPre);

            VkImageCopy copy{};
            copy.srcSubresource.aspectMask     = aspect;
            copy.srcSubresource.mipLevel       = 0;
            copy.srcSubresource.baseArrayLayer = src.baseArrayLayer;
            copy.srcSubresource.layerCount     = src.layerCount;
            copy.dstSubresource.aspectMask     = aspect;
            copy.dstSubresource.mipLevel       = 0;
            copy.dstSubresource.baseArrayLayer = 0;
            copy.dstSubresource.layerCount     = src.layerCount;
            copy.extent                        = { dst.width, dst.height, 1 };
            vkCmdCopyImage(cmd,
                src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &copy);

            VkImageMemoryBarrier2 post[2]{};
            post[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            post[0].srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            post[0].srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
            post[0].dstStageMask        = srcSA.stage;
            post[0].dstAccessMask       = srcSA.access;
            post[0].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            post[0].newLayout           = srcLayout;
            post[0].image               = src.image;
            post[0].subresourceRange    = { aspect, 0, 1, src.baseArrayLayer, src.layerCount };
            post[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            post[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            post[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            post[1].srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            post[1].srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            post[1].dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            post[1].dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            post[1].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            post[1].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            post[1].image               = dst.image;
            post[1].subresourceRange    = { aspect, 0, 1, 0, dst.layers };
            post[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            post[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depPost{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depPost.imageMemoryBarrierCount = 2;
            depPost.pImageMemoryBarriers    = post;
            vkCmdPipelineBarrier2(cmd, &depPost);

            dst.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // Queue an ArchivedImage's GPU resources for deferred destruction.
        // Defers by MAX_FRAMES_IN_FLIGHT so any in-flight ImGui frame still
        // sampling these views via cached descriptors completes first.
        // Mutates the input: nulls handles so the caller's vector slot stays
        // benign (handles=NULL → re-touching it during shutdown is a no-op).
        void PushArchiveDeletion(VkDevice device, RG::ArchivedImage& a)
        {
            VkImage         img        = a.image;
            VkImageView     view       = a.view;
            VmaAllocation   alloc      = a.alloc;
            VkDescriptorSet imguiDesc  = a.imguiDescSet;
            std::vector<VkImageView> layerViews = std::move(a.layerViews);

            VulkanContext::Get().PushDeletion(
                [device, img, view, alloc, imguiDesc,
                 layerViews = std::move(layerViews)]() mutable
                {
                    if (imguiDesc != VK_NULL_HANDLE)
                        ImGui_ImplVulkan_RemoveTexture(imguiDesc);
                    for (VkImageView lv : layerViews)
                        if (lv != VK_NULL_HANDLE)
                            vkDestroyImageView(device, lv, nullptr);
                    if (view != VK_NULL_HANDLE)
                        vkDestroyImageView(device, view, nullptr);
                    if (img != VK_NULL_HANDLE && alloc != nullptr)
                    {
                        // Route through VulkanAllocator so MemoryTracker sees
                        // the free — bypassing it leaves the editor's GPU
                        // memory counter rising on every Enable/Disable.
                        VulkanAllocator::FreeImage(img, alloc);
                    }
                });

            a.image        = VK_NULL_HANDLE;
            a.view         = VK_NULL_HANDLE;
            a.alloc        = nullptr;
            a.imguiDescSet = VK_NULL_HANDLE;
        }
    } // anonymous namespace

    // ---- Capture metadata helpers (unchanged) ----

    void FrameDebugger::BeginCapturePass(u32 graphPassIndex,
                                         const std::string& name, const std::string& activeTarget,
                                         bool isDepth, const RG::CapturedPipelineState& ps)
    {
        if (state != DebuggerState::CaptureRequested) return;

        RG::CapturedPass cp;
        cp.name               = name;
        cp.firstDrawIndex     = (u32)capturedFrame.drawCalls.size();
        cp.drawCallCount      = 0;
        cp.graphPassIndex     = graphPassIndex;
        cp.pipelineState      = ps;
        cp.activeRenderTarget = activeTarget;
        cp.isDepthTarget      = isDepth;
        capturedFrame.passes.push_back(std::move(cp));
    }

    void FrameDebugger::EndCapturePass()
    {
        if (state != DebuggerState::CaptureRequested) return;
        if (capturedFrame.passes.empty()) return;

        auto& cp = capturedFrame.passes.back();
        cp.drawCallCount = (u32)capturedFrame.drawCalls.size() - cp.firstDrawIndex;
    }

    void FrameDebugger::CaptureDrawCall(const std::string& passName, const std::string& meshName,
                                        const std::string& entityName, u32 entityIndex, u32 indexCount,
                                        const ObjectPushConstants& pc, const RG::CapturedPipelineState& ps)
    {
        if (state != DebuggerState::CaptureRequested) return;

        RG::CapturedDrawCall cdc;
        cdc.globalIndex    = (u32)capturedFrame.drawCalls.size();
        cdc.passIndex      = capturedFrame.passes.empty() ? 0 : (u32)(capturedFrame.passes.size() - 1);
        cdc.passLocalIndex = capturedFrame.passes.empty() ? 0
                           : (u32)(capturedFrame.drawCalls.size() - capturedFrame.passes.back().firstDrawIndex);
        cdc.passName       = passName;
        cdc.meshName       = meshName;
        cdc.entityName     = entityName;
        cdc.entityIndex    = entityIndex;
        cdc.indexCount     = indexCount;
        cdc.modelMatrix    = pc.modelMatrix;
        cdc.materialIndex  = pc.materialIndex;
        cdc.shadeMode      = pc.shadeMode;
        cdc.entityID       = pc.entityID;
        cdc.boneOffset     = pc.boneOffset;
        cdc.pipelineState  = ps;
        capturedFrame.drawCalls.push_back(std::move(cdc));
    }

    void FrameDebugger::CaptureIndirectDraw(const std::string& passName, const std::string& meshName,
                                            const std::string& entityName, u32 entityIndex, u32 indexCount,
                                            u32 gpuObjectIndex, VkDeviceSize indirectOffset,
                                            const RG::CapturedPipelineState& ps)
    {
        if (state != DebuggerState::CaptureRequested) return;

        RG::CapturedDrawCall cdc;
        cdc.globalIndex       = (u32)capturedFrame.drawCalls.size();
        cdc.passIndex         = capturedFrame.passes.empty() ? 0 : (u32)(capturedFrame.passes.size() - 1);
        cdc.passLocalIndex    = capturedFrame.passes.empty() ? 0
                              : (u32)(capturedFrame.drawCalls.size() - capturedFrame.passes.back().firstDrawIndex);
        cdc.passName          = passName;
        cdc.meshName          = meshName;
        cdc.entityName        = entityName;
        cdc.entityIndex       = entityIndex;
        cdc.indexCount        = indexCount;
        cdc.pipelineState     = ps;
        cdc.kind              = RG::DispatchKind::IndexedIndirect;
        cdc.gpuObjectIndex    = gpuObjectIndex;
        cdc.indirectOffset    = indirectOffset;
        cdc.indirectDrawCount = 1;
        cdc.indirectStride    = (u32)sizeof(VkDrawIndexedIndirectCommand);
        capturedFrame.drawCalls.push_back(std::move(cdc));
    }

    void FrameDebugger::CaptureComputeDispatch(const std::string& passName, const std::string& shaderName,
                                               u32 groupCountX, u32 groupCountY, u32 groupCountZ)
    {
        if (state != DebuggerState::CaptureRequested) return;

        RG::CapturedDrawCall cdc;
        cdc.globalIndex    = (u32)capturedFrame.drawCalls.size();
        cdc.passIndex      = capturedFrame.passes.empty() ? 0 : (u32)(capturedFrame.passes.size() - 1);
        cdc.passLocalIndex = capturedFrame.passes.empty() ? 0
                           : (u32)(capturedFrame.drawCalls.size() - capturedFrame.passes.back().firstDrawIndex);
        cdc.passName       = passName;
        cdc.meshName       = shaderName;   // reused as shader label
        cdc.entityName     = "Dispatch";
        cdc.kind           = RG::DispatchKind::Compute;
        cdc.groupCountX    = groupCountX;
        cdc.groupCountY    = groupCountY;
        cdc.groupCountZ    = groupCountZ;
        cdc.pipelineState.shaderName = shaderName;
        capturedFrame.drawCalls.push_back(std::move(cdc));
    }

    // ---- Phase 14B — Archive lifecycle ----

    void FrameDebugger::BeginCapture(VkDevice device, VmaAllocator allocator)
    {
        archiveDevice    = device;
        archiveAllocator = allocator;

        // Reuse path: keep capturedFrame.archivedImages and m_ArchiveSlotMap
        // alive across captures. OnPassExecuted will rewrite contents into
        // existing VkImages instead of allocating fresh ones, so the panel's
        // view-pointer-keyed descriptor caches stay valid frame-to-frame.
        // Full cleanup happens via ExitCapture → DestroyArchives.
        capturedFrame.passArchives.clear();
        capturedFrame.Clear();   // metadata-only reset; preserves archivedImages
        capturedFrame.capturedRenderFrameIndex = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        trackedRTs.clear();
        m_ArchiveSlotInUseThisCapture.clear();
    }

    void FrameDebugger::RegisterTrackedRT(const std::string& name)
    {
        trackedRTs.insert(name);
    }

    void FrameDebugger::FinalizeCapture(const Mat4& viewProj)
    {
        capturedFrame.captureViewProj = viewProj;

        // Phase 1 records graphics secondaries (graphics lambdas push into
        // capturedFrame.passes here) before Phase 2 runs compute lambdas
        // inline — so passes arrive in graphics-first-then-compute order
        // rather than graph order. Sort by graphPassIndex; remap drawCall
        // back-references through the inverse permutation.
        const u32 passCount = (u32)capturedFrame.passes.size();
        if (passCount > 1)
        {
            std::vector<u32> perm(passCount);
            for (u32 i = 0; i < passCount; ++i) perm[i] = i;
            std::stable_sort(perm.begin(), perm.end(),
                [this](u32 a, u32 b) {
                    return capturedFrame.passes[a].graphPassIndex
                         < capturedFrame.passes[b].graphPassIndex;
                });

            std::vector<RG::CapturedPass> sorted;
            sorted.reserve(passCount);
            for (u32 oldIdx : perm)
                sorted.push_back(std::move(capturedFrame.passes[oldIdx]));
            capturedFrame.passes = std::move(sorted);

            // Inverse permutation: oldIdx → newIdx for drawCall.passIndex remap.
            std::vector<u32> inv(passCount);
            for (u32 newIdx = 0; newIdx < passCount; ++newIdx)
                inv[perm[newIdx]] = newIdx;

            for (auto& dc : capturedFrame.drawCalls)
                if (dc.passIndex < passCount)
                    dc.passIndex = inv[dc.passIndex];

            // invariant: cf.drawCalls is contiguous in graph-execution order so
            // panel slider scrubs sequentially. firstDrawIndex / globalIndex updated.
            std::vector<RG::CapturedDrawCall> sortedDraws;
            sortedDraws.reserve(capturedFrame.drawCalls.size());
            for (u32 newPassIdx = 0; newPassIdx < passCount; ++newPassIdx)
            {
                auto& pass = capturedFrame.passes[newPassIdx];
                const u32 oldFirst = pass.firstDrawIndex;
                const u32 count    = pass.drawCallCount;
                pass.firstDrawIndex = (u32)sortedDraws.size();
                for (u32 i = 0; i < count; ++i)
                {
                    if (oldFirst + i >= capturedFrame.drawCalls.size()) break;
                    auto dc = std::move(capturedFrame.drawCalls[oldFirst + i]);
                    dc.globalIndex = (u32)sortedDraws.size();
                    dc.passIndex   = newPassIdx;
                    sortedDraws.push_back(std::move(dc));
                }
            }
            capturedFrame.drawCalls = std::move(sortedDraws);
        }

        // Build the hierarchical event tree from the just-finished capture.
        // Pure CPU work; safe to run after ExecuteGraph returned and all the
        // per-pass / per-draw metadata has been appended to capturedFrame.
        capturedFrame.rootEvent = RG::BuildEventTree(capturedFrame);

        // Free archive slots not touched this capture (e.g. SelectionMaskPass
        // entries left over after the user disabled drawSelectionOutline).
        // Defer GPU destruction so any in-flight ImGui frame still sampling
        // the old view completes first.
        if (archiveDevice != VK_NULL_HANDLE && !m_ArchiveSlotMap.empty())
        {
            for (auto it = m_ArchiveSlotMap.begin(); it != m_ArchiveSlotMap.end(); )
            {
                if (m_ArchiveSlotInUseThisCapture.find(it->first) == m_ArchiveSlotInUseThisCapture.end())
                {
                    if (it->second < capturedFrame.archivedImages.size())
                        PushArchiveDeletion(archiveDevice, capturedFrame.archivedImages[it->second]);
                    it = m_ArchiveSlotMap.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }

    void FrameDebugger::DestroyArchives()
    {
        if (capturedFrame.archivedImages.empty()) return;

        VkDevice     device    = archiveDevice;
        VmaAllocator allocator = archiveAllocator ? archiveAllocator : VulkanAllocator::Get();

        if (device == VK_NULL_HANDLE || allocator == nullptr)
        {
            // Best effort — leak the GPU memory rather than crash if context is gone.
            capturedFrame.archivedImages.clear();
            capturedFrame.passArchives.clear();
            return;
        }

        (void)allocator;  // VulkanAllocator::FreeImage uses the singleton allocator;
                          // we keep `allocator` only to gate the early-return above.

        // Defer the actual GPU resource destruction by MAX_FRAMES_IN_FLIGHT so
        // that any in-flight ImGui frame still sampling these views via cached
        // descriptor sets completes before the views/images are freed.
        for (auto& a : capturedFrame.archivedImages)
            PushArchiveDeletion(device, a);

        capturedFrame.archivedImages.clear();
        capturedFrame.passArchives.clear();
        m_ArchiveSlotMap.clear();
        m_ArchiveSlotInUseThisCapture.clear();
    }

    // IArchiveSink — called by RenderGraph::Execute after each non-culled pass.
    // Find-or-allocate via m_ArchiveSlotMap: reuse the existing ArchivedImage
    // when (passName, rtName) hits with matching dims/format, allocate fresh
    // only on first sight or viewport resize.
    void FrameDebugger::OnPassExecuted(u32 passIdx, RG::RenderGraph& graph, VkCommandBuffer cmd)
    {
        if (state != DebuggerState::CaptureRequested) return;
        if (archiveDevice == VK_NULL_HANDLE) return;

        const auto& passes = graph.GetPasses();
        if (passIdx >= passes.size()) return;
        const auto& pass = passes[passIdx];

        auto& resources = graph.GetResources();

        if (capturedFrame.passArchives.size() <= passIdx)
            capturedFrame.passArchives.resize(passIdx + 1);

        for (size_t i = 0; i < pass.writes.size(); ++i)
        {
            u32 resIdx = pass.writes[i].index;
            if (resIdx == 0 || resIdx > resources.size()) continue;
            auto& res = resources[resIdx - 1];
            if (res.image == VK_NULL_HANDLE) continue;
            if (trackedRTs.find(res.desc.name) == trackedRTs.end()) continue;

            // Slot key: (pass name, RT name). Both stable across captures,
            // unlike pass index which shifts with culling.
            const std::string slotKey = pass.name + "/" + res.desc.name;
            u32 archiveIdx = UINT32_MAX;

            auto it = m_ArchiveSlotMap.find(slotKey);
            if (it != m_ArchiveSlotMap.end() && it->second < capturedFrame.archivedImages.size())
            {
                auto& existing = capturedFrame.archivedImages[it->second];
                const VkFormat fmt = ToVkFormat(res.desc.format);
                if (existing.image  != VK_NULL_HANDLE &&
                    existing.width  == res.desc.width  &&
                    existing.height == res.desc.height &&
                    existing.format == fmt &&
                    existing.layers == res.layerCount)
                {
                    archiveIdx = it->second;
                }
                else
                {
                    // Stale — viewport resize / format change. Free the old
                    // backing and let the allocator hand us a fresh slot at
                    // the end of archivedImages. The vector grows by one
                    // null'd slot per resize event; bounded and rare, so we
                    // don't bother compacting here.
                    PushArchiveDeletion(archiveDevice, existing);
                    m_ArchiveSlotMap.erase(it);
                }
            }

            if (archiveIdx == UINT32_MAX)
            {
                archiveIdx = AllocateArchiveImage(archiveDevice, archiveAllocator, res, capturedFrame);
                m_ArchiveSlotMap[slotKey] = archiveIdx;
            }

            m_ArchiveSlotInUseThisCapture.insert(slotKey);
            EmitArchiveCopy(cmd, res, pass.writeStates[i], capturedFrame.archivedImages[archiveIdx]);
            capturedFrame.passArchives[passIdx].push_back(archiveIdx);
        }
    }

    void FrameDebugger::Shutdown(VkDevice device)
    {
        // Free archived images first while the device + allocator are still valid.
        DestroyArchives();
        archiveDevice    = VK_NULL_HANDLE;
        archiveAllocator = nullptr;
        trackedRTs.clear();

        if (sampler)
            vkDestroySampler(device, sampler, nullptr);
        if (descSetLayout)
            vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
        if (descPool)
            vkDestroyDescriptorPool(device, descPool, nullptr);
        blitPipeline.reset();
        depthPipeline.reset();
    }
}
