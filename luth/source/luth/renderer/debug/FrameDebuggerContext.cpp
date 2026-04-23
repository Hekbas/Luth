#include "luthpch.h"
#include "luth/renderer/debug/FrameDebuggerContext.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/FrameDebugger.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/DynamicRendering.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/Buffer.h"

namespace Luth
{
    FrameDebuggerContext::FrameDebuggerContext(RenderPipeline& pipeline)
        : m_Pipeline(pipeline)
    {
    }

    FrameDebuggerContext::~FrameDebuggerContext() = default;

    void FrameDebuggerContext::Shutdown()
    {
        DestroyPerDrawPreviewTexture();
        DestroyDepthPreviewTexture();
    }

    void FrameDebuggerContext::InitDebugBlitResources()
    {
        auto& fd = m_Pipeline.m_System.m_FrameDebugger;
        if (fd.blitPipeline) return; // Already initialized

        if (auto sh = ShaderLibrary::LoadEngine("shaders/debugBlit.frag"))
            fd.blitFragSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/debugDepth.frag"))
            fd.depthFragSpv = sh->GetSpirV();

        if (fd.blitFragSpv.empty() || fd.depthFragSpv.empty() || m_Pipeline.m_FullscreenVertSpv.empty())
        {
            LH_CORE_ERROR("Failed to compile debug blit shaders");
            return;
        }

        auto device = VulkanContext::Get().GetDevice();

        // Create sampler
        VkSamplerCreateInfo samplerCI{};
        samplerCI.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerCI.magFilter = VK_FILTER_LINEAR;
        samplerCI.minFilter = VK_FILTER_LINEAR;
        samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(device, &samplerCI, nullptr, &fd.sampler);

        // Descriptor set layout: binding 0 = combined image sampler
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCI{};
        layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCI.bindingCount = 1;
        layoutCI.pBindings    = &binding;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &fd.descSetLayout);

        // Descriptor pool
        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo poolCI{};
        poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCI.maxSets       = 1;
        poolCI.poolSizeCount = 1;
        poolCI.pPoolSizes    = &poolSize;
        poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &poolCI, nullptr, &fd.descPool);

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocCI{};
        allocCI.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocCI.descriptorPool     = fd.descPool;
        allocCI.descriptorSetCount = 1;
        allocCI.pSetLayouts        = &fd.descSetLayout;
        vkAllocateDescriptorSets(device, &allocCI, &fd.descSet);

        // Create blit pipeline (color)
        std::vector<VkDescriptorSetLayout> layouts = { fd.descSetLayout };
        PipelineConfig blitConfig;
        blitConfig.depthTest  = false;
        blitConfig.depthWrite = false;
        blitConfig.cullMode         = VK_CULL_MODE_NONE;
        blitConfig.colorFormats     = { VK_FORMAT_R8G8B8A8_UNORM };
        fd.blitPipeline = std::make_unique<VKPipeline>(
            blitConfig, m_Pipeline.m_FullscreenVertSpv, fd.blitFragSpv, layouts);

        // Create depth visualization pipeline
        PipelineConfig depthConfig;
        depthConfig.depthTest  = false;
        depthConfig.depthWrite = false;
        depthConfig.cullMode         = VK_CULL_MODE_NONE;
        depthConfig.colorFormats     = { VK_FORMAT_R8G8B8A8_UNORM };

        VkPushConstantRange depthPC{};
        depthPC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        depthPC.offset     = 0;
        depthPC.size       = sizeof(float) * 2; // near, far
        depthConfig.pushConstantRanges = { depthPC };

        fd.depthPipeline = std::make_unique<VKPipeline>(
            depthConfig, m_Pipeline.m_FullscreenVertSpv, fd.depthFragSpv, layouts);
    }

    RG::ResourceHandle FrameDebuggerContext::AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth)
    {
        auto& sys = m_Pipeline.m_System;
        if (!sys.m_FrameDebugger.blitPipeline || !sys.m_SceneTargets.GetLDROutput()) return inputHandle;

        struct DebugBlitData {
            RG::ResourceHandle output;
            RG::ResourceHandle input;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<DebugBlitData>("DebugDisplayBlit",
            [&](DebugBlitData& data, RG::RenderPassBuilder& builder)
            {
                auto ldrVk = std::static_pointer_cast<VKTexture>(sys.m_SceneTargets.GetLDROutput());
                RG::TextureDesc desc;
                desc.name   = "LDROutput";
                desc.width  = sys.m_SceneTargets.GetLDROutput()->GetWidth();
                desc.height = sys.m_SceneTargets.GetLDROutput()->GetHeight();
                desc.format = RG::TextureFormat::RGBA8_Unorm;

                data.output = rg.ImportResource(desc,
                    (void*)ldrVk->GetImage(), (void*)ldrVk->GetImageView(),
                    RG::ResourceState::ShaderResource);
                data.output = builder.Write(data.output);

                data.input = builder.Read(inputHandle);
                outputHandle = data.output;
            },
            [this, isDepth](DebugBlitData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline.m_System;
                VkCommandBuffer cmd = ctx.commandBuffer;

                u32 w = sys.m_SceneTargets.GetLDROutput()->GetWidth();
                u32 h = sys.m_SceneTargets.GetLDROutput()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                if (isDepth && sys.m_FrameDebugger.depthPipeline)
                {
                    sys.m_FrameDebugger.depthPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        sys.m_FrameDebugger.depthPipeline->GetLayout(), 0, 1, &sys.m_FrameDebugger.descSet, 0, nullptr);

                    float pc[2] = { 0.1f, 200.0f }; // near/far for shadow maps
                    vkCmdPushConstants(cmd, sys.m_FrameDebugger.depthPipeline->GetLayout(),
                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
                }
                else
                {
                    sys.m_FrameDebugger.blitPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        sys.m_FrameDebugger.blitPipeline->GetLayout(), 0, 1, &sys.m_FrameDebugger.descSet, 0, nullptr);
                }

                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
        );

        return outputHandle;
    }

    void FrameDebuggerContext::EnsurePerDrawPreviewTexture(u32 width, u32 height)
    {
        if (m_PerDrawPreviewImage != VK_NULL_HANDLE
            && m_PerDrawPreviewWidth == width
            && m_PerDrawPreviewHeight == height) return;

        // Tear down any prior preview at a different size first. Deferred so
        // an in-flight ImGui frame can finish sampling the old view safely.
        if (m_PerDrawPreviewImage != VK_NULL_HANDLE)
        {
            VkImage       img   = m_PerDrawPreviewImage;
            VkImageView   view  = m_PerDrawPreviewView;
            VmaAllocation alloc = m_PerDrawPreviewAlloc;
            VulkanContext::Get().PushDeletion([img, view, alloc]() {
                auto dev = VulkanContext::Get().GetDevice();
                vkDestroyImageView(dev, view, nullptr);
                VulkanAllocator::FreeImage(img, alloc);
            });
            m_PerDrawPreviewImage = VK_NULL_HANDLE;
            m_PerDrawPreviewView  = VK_NULL_HANDLE;
            m_PerDrawPreviewAlloc = nullptr;
        }

        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.extent        = { width, height, 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        // Match the scene color target's format so vkCmdCopyImage is layout-compatible.
        ci.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        m_PerDrawPreviewAlloc = VulkanAllocator::AllocateImage(ci, VMA_MEMORY_USAGE_AUTO, m_PerDrawPreviewImage);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_PerDrawPreviewImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = ci.format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(VulkanContext::Get().GetDevice(), &vci, nullptr, &m_PerDrawPreviewView);

        m_PerDrawPreviewWidth  = width;
        m_PerDrawPreviewHeight = height;
        m_PerDrawPreviewKey    = UINT64_MAX;  // dimensions changed → invalidate cache
    }

    void FrameDebuggerContext::DestroyPerDrawPreviewTexture()
    {
        if (m_PerDrawPreviewImage == VK_NULL_HANDLE) return;
        auto dev = VulkanContext::Get().GetDevice();
        vkDestroyImageView(dev, m_PerDrawPreviewView, nullptr);
        VulkanAllocator::FreeImage(m_PerDrawPreviewImage, m_PerDrawPreviewAlloc);
        m_PerDrawPreviewImage  = VK_NULL_HANDLE;
        m_PerDrawPreviewView   = VK_NULL_HANDLE;
        m_PerDrawPreviewAlloc  = nullptr;
        m_PerDrawPreviewWidth  = 0;
        m_PerDrawPreviewHeight = 0;
        m_PerDrawPreviewKey    = UINT64_MAX;
    }

    void FrameDebuggerContext::ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx)
    {
        auto& sys = m_Pipeline.m_System;

        if (sys.m_FrameDebugger.state != DebuggerState::Frozen) return;
        if (!sys.m_FrameDebugger.capturedFrame.valid) return;
        if (passIdx >= sys.m_FrameDebugger.capturedFrame.passes.size()) return;

        const auto& pass = sys.m_FrameDebugger.capturedFrame.passes[passIdx];

        // v1 — only GeometryPass per-draw replay is wired. Other passes leave
        // the cache key unchanged so the panel falls back to pass-output archive.
        if (pass.name != "GeometryPass") return;
        if (!sys.m_SceneTargets.GetSceneColor() || !sys.m_SceneTargets.GetSceneDepth() || !sys.m_SceneTargets.GetEntityIDBuffer()) return;

        // Cache hit — same selection as last replay, nothing to do.
        const u64 key = ((u64)passIdx << 32) | (u64)localDrawIdx;
        if (key == m_PerDrawPreviewKey) return;

        const u32 width  = sys.m_SceneTargets.GetSceneColor()->GetWidth();
        const u32 height = sys.m_SceneTargets.GetSceneColor()->GetHeight();
        EnsurePerDrawPreviewTexture(width, height);
        if (m_PerDrawPreviewImage == VK_NULL_HANDLE) return;

        auto vkSceneColor = std::static_pointer_cast<VKTexture>(sys.m_SceneTargets.GetSceneColor());
        auto vkSceneDepth = std::static_pointer_cast<VKTexture>(sys.m_SceneTargets.GetSceneDepth());
        auto vkEntityID   = std::static_pointer_cast<VKTexture>(sys.m_SceneTargets.GetEntityIDBuffer());
        VkImage     sceneColorImg  = vkSceneColor->GetImage();
        VkImageView sceneColorView = vkSceneColor->GetImageView();
        VkImage     sceneDepthImg  = vkSceneDepth->GetImage();
        VkImageView sceneDepthView = vkSceneDepth->GetImageView();
        VkImage     entityIDImg    = vkEntityID->GetImage();
        VkImageView entityIDView   = vkEntityID->GetImageView();

        // Total draws to issue (1-based count). Original GeometryPass emits
        // draws in opaque → cutout → transparent order, which matches the
        // capture-time CapturedDrawCall ordering.
        const u32 maxDraws = localDrawIdx + 1;

        // Capture the CPU-side data we need by value (the lambda runs inside
        // ImmediateSubmit and must be self-contained).
        VkPolygonMode polyMode = (sys.m_ShadeMode == ShadeMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        UUID pbrUUID = ShaderLibrary::Get("pbr.vert")->Handle;

        VulkanContext::Get().ImmediateSubmit([&, this](VkCommandBuffer cmd)
        {
            auto& sys = m_Pipeline.m_System;
            auto& rp  = m_Pipeline;

            // ---- Phase 1: prep all attachments + preview ----
            // We use UNDEFINED → ATTACHMENT to ignore the prior contents (the
            // graph CLEARs anyway) and avoid relying on whatever layout the
            // last live frame left them in.
            VkImageMemoryBarrier2 prep[4]{};
            auto fillAtt = [](VkImageMemoryBarrier2& b, VkImage img,
                              VkImageAspectFlags aspect, VkImageLayout newLayout,
                              VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
            {
                b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                b.srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                b.srcAccessMask       = 0;
                b.dstStageMask        = dstStage;
                b.dstAccessMask       = dstAccess;
                b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout           = newLayout;
                b.image               = img;
                b.subresourceRange    = { aspect, 0, 1, 0, 1 };
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            };
            fillAtt(prep[0], sceneColorImg, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            fillAtt(prep[1], sceneDepthImg, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            fillAtt(prep[2], entityIDImg, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            VkDependencyInfo depPrep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depPrep.imageMemoryBarrierCount = 3;
            depPrep.pImageMemoryBarriers    = prep;
            vkCmdPipelineBarrier2(cmd, &depPrep);

            // ---- Phase 2: BeginRendering with cleared attachments ----
            AttachmentInfo colorAtt[2]{};
            colorAtt[0].ImageView  = sceneColorView;
            colorAtt[0].Format     = VK_FORMAT_R16G16B16A16_SFLOAT;
            colorAtt[0].LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt[0].StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt[0].ClearValue = { { {0.f, 0.f, 0.f, 0.f} } };
            colorAtt[0].Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            colorAtt[1].ImageView  = entityIDView;
            colorAtt[1].Format     = VK_FORMAT_R32_UINT;
            colorAtt[1].LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt[1].StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt[1].ClearValue.color.uint32[0] = 0;
            colorAtt[1].Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            AttachmentInfo depthAtt{};
            depthAtt.ImageView  = sceneDepthView;
            depthAtt.Format     = VK_FORMAT_D32_SFLOAT;
            depthAtt.LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.ClearValue.depthStencil = { 1.0f, 0 };
            depthAtt.Layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            RenderPassInfo rpInfo{};
            rpInfo.ColorAttachments = std::span<AttachmentInfo>(colorAtt, 2);
            rpInfo.DepthAttachment  = &depthAtt;
            rpInfo.RenderArea       = { {0, 0}, { width, height } };

            DynamicRendering::BeginRendering(cmd, rpInfo);

            // ---- Phase 3: bind pipelines + descriptors, replay draws ----
            // Same descriptor sets as the live GeometryPass — the underlying
            // UBOs/SSBOs are byte-stable in Frozen state (no live writers).
            auto* opaquePipeline = rp.m_GeoPipelineManager.GetOrCreate(
                pbrUUID, Material::RenderMode::Opaque, Material::CullMode::Back,
                polyMode, rp.m_PBRVertSpv, rp.m_PBRFragSpv);
            if (!opaquePipeline)
            {
                DynamicRendering::EndRendering(cmd);
                return;
            }
            VkPipelineLayout pipelineLayout = opaquePipeline->GetLayout();

            VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
            VkDescriptorSet sets[] = {
                rp.m_GlobalDescriptorSet, bindlessSet, MaterialSystem::GetDescriptorSet(),
                rp.m_LightDescSet, BoneMatrixBuffer::GetDescriptorSet(), rp.m_ObjectSSBODescSet
            };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipelineLayout, 0, 6, sets, 0, nullptr);

            VkViewport vp{}; vp.width = (float)width; vp.height = (float)height; vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D scRect{}; scRect.extent = { width, height };
            vkCmdSetScissor(cmd, 0, 1, &scRect);

            u32 drawsRemaining = maxDraws;

            auto ReplayBatch = [&](const std::vector<DrawCommand>& draws, Material::RenderMode mode)
            {
                if (drawsRemaining == 0 || draws.empty()) return;

                Material::CullMode currentCull = Material::CullMode::Back;
                bool currentSkinned = false;
                auto* pipeline = rp.m_GeoPipelineManager.GetOrCreate(
                    pbrUUID, mode, currentCull, polyMode, rp.m_PBRVertSpv, rp.m_PBRFragSpv);
                if (!pipeline) return;
                pipeline->Bind(cmd);

                for (const auto& dc : draws)
                {
                    if (drawsRemaining == 0) return;

                    if (dc.cullMode != currentCull || dc.isSkinned != currentSkinned)
                    {
                        currentCull    = dc.cullMode;
                        currentSkinned = dc.isSkinned;
                        VKPipeline* newPipeline = currentSkinned
                            ? rp.m_GeoSkinnedPipelineManager.GetOrCreate(pbrUUID, mode, currentCull, polyMode, rp.m_PBRSkinnedVertSpv, rp.m_PBRFragSpv)
                            : rp.m_GeoPipelineManager.GetOrCreate       (pbrUUID, mode, currentCull, polyMode, rp.m_PBRVertSpv,        rp.m_PBRFragSpv);
                        if (!newPipeline) continue;
                        newPipeline->Bind(cmd);
                    }

                    auto mesh = dc.model->GetMesh(dc.meshIndex);
                    if (!mesh) { drawsRemaining--; continue; }
                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer >(mesh->GetIndexBuffer ());
                    if (!vb || !ib) { drawsRemaining--; continue; }

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                    // Use the captured indirect buffer (camera region, byte-stable
                    // in Frozen state). gpuObjectIndex is also the firstInstance
                    // base — same as live GeometryPass.
                    VkDeviceSize indirectOffset = dc.gpuObjectIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, rp.m_IndirectBuffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));

                    --drawsRemaining;
                }
            };

            ReplayBatch(sys.m_DrawList.opaque,      Material::RenderMode::Opaque);
            ReplayBatch(sys.m_DrawList.cutout,      Material::RenderMode::Cutout);
            ReplayBatch(sys.m_DrawList.transparent, Material::RenderMode::Transparent);

            DynamicRendering::EndRendering(cmd);

            // ---- Phase 4: copy SceneColor → preview ----
            VkImageMemoryBarrier2 toCopy[2]{};
            toCopy[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toCopy[0].srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toCopy[0].srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toCopy[0].dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toCopy[0].dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
            toCopy[0].oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toCopy[0].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toCopy[0].image               = sceneColorImg;
            toCopy[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toCopy[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            toCopy[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toCopy[1].srcStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toCopy[1].srcAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            toCopy[1].dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toCopy[1].dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            // First-ever replay is UNDEFINED, subsequent replays come from
            // SHADER_READ_ONLY_OPTIMAL after the panel sampled the previous
            // preview. Treat both uniformly via UNDEFINED — discards content
            // (which we'd overwrite anyway).
            toCopy[1].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            toCopy[1].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toCopy[1].image               = m_PerDrawPreviewImage;
            toCopy[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toCopy[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depCopy{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depCopy.imageMemoryBarrierCount = 2;
            depCopy.pImageMemoryBarriers    = toCopy;
            vkCmdPipelineBarrier2(cmd, &depCopy);

            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.extent         = { width, height, 1 };
            vkCmdCopyImage(cmd,
                sceneColorImg,         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_PerDrawPreviewImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &copy);

            // ---- Phase 5: restore layouts for next consumer ----
            // SceneColor → SHADER_READ_ONLY (matches what the live pipeline
            // expects after PostProcessPass; next live frame's barriers will
            // re-transition as needed).
            // Preview → SHADER_READ_ONLY for ImGui sampling.
            VkImageMemoryBarrier2 fin[2]{};
            fin[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            fin[0].srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            fin[0].srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
            fin[0].dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            fin[0].dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            fin[0].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            fin[0].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fin[0].image               = sceneColorImg;
            fin[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fin[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fin[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            fin[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            fin[1].srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            fin[1].srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            fin[1].dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            fin[1].dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            fin[1].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            fin[1].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fin[1].image               = m_PerDrawPreviewImage;
            fin[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fin[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fin[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depFin{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depFin.imageMemoryBarrierCount = 2;
            depFin.pImageMemoryBarriers    = fin;
            vkCmdPipelineBarrier2(cmd, &depFin);
        });

        m_PerDrawPreviewKey = key;
    }

    void FrameDebuggerContext::EnsureDepthPreviewTexture(u32 width, u32 height)
    {
        if (m_DepthPreviewImage != VK_NULL_HANDLE
            && m_DepthPreviewWidth == width
            && m_DepthPreviewHeight == height) return;

        if (m_DepthPreviewImage != VK_NULL_HANDLE)
        {
            VkImage       img   = m_DepthPreviewImage;
            VkImageView   view  = m_DepthPreviewView;
            VmaAllocation alloc = m_DepthPreviewAlloc;
            VulkanContext::Get().PushDeletion([img, view, alloc]() {
                auto dev = VulkanContext::Get().GetDevice();
                vkDestroyImageView(dev, view, nullptr);
                VulkanAllocator::FreeImage(img, alloc);
            });
            m_DepthPreviewImage = VK_NULL_HANDLE;
            m_DepthPreviewView  = VK_NULL_HANDLE;
            m_DepthPreviewAlloc = nullptr;
        }

        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.extent        = { width, height, 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        // RGBA8 — depth blit shader writes a tonemapped grayscale that ImGui
        // can sample directly without HDR clipping concerns.
        ci.format        = VK_FORMAT_R8G8B8A8_UNORM;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ci.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        m_DepthPreviewAlloc = VulkanAllocator::AllocateImage(ci, VMA_MEMORY_USAGE_AUTO, m_DepthPreviewImage);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_DepthPreviewImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = ci.format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(VulkanContext::Get().GetDevice(), &vci, nullptr, &m_DepthPreviewView);

        m_DepthPreviewWidth  = width;
        m_DepthPreviewHeight = height;
        m_DepthPreviewKey    = UINT64_MAX;
    }

    void FrameDebuggerContext::DestroyDepthPreviewTexture()
    {
        if (m_DepthPreviewImage == VK_NULL_HANDLE) return;
        auto dev = VulkanContext::Get().GetDevice();
        vkDestroyImageView(dev, m_DepthPreviewView, nullptr);
        VulkanAllocator::FreeImage(m_DepthPreviewImage, m_DepthPreviewAlloc);
        m_DepthPreviewImage  = VK_NULL_HANDLE;
        m_DepthPreviewView   = VK_NULL_HANDLE;
        m_DepthPreviewAlloc  = nullptr;
        m_DepthPreviewWidth  = 0;
        m_DepthPreviewHeight = 0;
        m_DepthPreviewKey    = UINT64_MAX;
    }

    void FrameDebuggerContext::BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ)
    {
        auto& sys = m_Pipeline.m_System;

        if (sys.m_FrameDebugger.state != DebuggerState::Frozen) return;
        if (!sys.m_FrameDebugger.capturedFrame.valid) return;
        if (archiveIdx >= sys.m_FrameDebugger.capturedFrame.archivedImages.size()) return;

        auto& archive = sys.m_FrameDebugger.capturedFrame.archivedImages[archiveIdx];
        if (!archive.isDepth || archive.image == VK_NULL_HANDLE) return;

        InitDebugBlitResources();  // idempotent — needed for sampler + depthPipeline + descSet
        if (!sys.m_FrameDebugger.depthPipeline || sys.m_FrameDebugger.descSet == VK_NULL_HANDLE) return;

        // Cache short-circuit (use layer+1 in the low bits so layer == -1 maps to 0).
        const u64 key = ((u64)archiveIdx << 32) | (u64)((layer < 0 ? 0u : (u32)layer + 1u));
        if (key == m_DepthPreviewKey && m_DepthPreviewImage != VK_NULL_HANDLE) return;

        // Preview matches archive dimensions so sampling = 1:1 texel mapping.
        EnsureDepthPreviewTexture(archive.width, archive.height);
        if (m_DepthPreviewImage == VK_NULL_HANDLE) return;

        // Resolve the source view.
        //
        // - Multi-layer archive + layer in range → per-layer view (sampled as 2D).
        // - Single-layer archive (e.g. cascade "ShadowMap.C<i>" — each cascade
        //   pass owns its own single-layer archive backed by the layer-i view of
        //   the shared 4-layer image) → archive.view, which is already 2D.
        // - layer < 0 → whole image (only meaningful for non-array archives).
        //
        // Falling back to archive.view when (layer >= archive.layers) is what
        // makes cascade slices renderable: the EventNode's archiveLayer holds
        // the *cascade index* (0..3) for detail-panel lookups, even though
        // the underlying archive only has 1 layer.
        VkDevice device = VulkanContext::Get().GetDevice();
        VkImageView srcView = (layer < 0 || archive.layers <= 1 || (u32)layer >= archive.layers)
            ? archive.view
            : archive.GetOrCreateLayerView(device, (u32)layer);
        if (srcView == VK_NULL_HANDLE) return;

        // Update the shared debug descriptor to point at this view. Safe to do
        // synchronously: ImmediateSubmit below blocks on a fence so the
        // descriptor isn't in flight while we're rewriting it.
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = sys.m_FrameDebugger.sampler;
        imgInfo.imageView   = srcView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = sys.m_FrameDebugger.descSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);

        const u32 width  = archive.width;
        const u32 height = archive.height;
        VkImage     dstImg  = m_DepthPreviewImage;
        VkImageView dstView = m_DepthPreviewView;

        VulkanContext::Get().ImmediateSubmit([this, dstImg, dstView, width, height, nearZ, farZ](VkCommandBuffer cmd)
        {
            auto& sys = m_Pipeline.m_System;

            // Preview UNDEFINED → COLOR_ATTACHMENT_OPTIMAL (clear-on-load).
            VkImageMemoryBarrier2 prep{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            prep.srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            prep.srcAccessMask       = 0;
            prep.dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            prep.dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            prep.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            prep.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            prep.image               = dstImg;
            prep.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            prep.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            prep.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depPrep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depPrep.imageMemoryBarrierCount = 1;
            depPrep.pImageMemoryBarriers    = &prep;
            vkCmdPipelineBarrier2(cmd, &depPrep);

            AttachmentInfo colorAtt{};
            colorAtt.ImageView  = dstView;
            colorAtt.Format     = VK_FORMAT_R8G8B8A8_UNORM;
            colorAtt.LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt.StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt.ClearValue = { { {0.f, 0.f, 0.f, 1.f} } };
            colorAtt.Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            RenderPassInfo rpInfo{};
            rpInfo.ColorAttachments = std::span<AttachmentInfo>(&colorAtt, 1);
            rpInfo.RenderArea       = { {0, 0}, { width, height } };

            DynamicRendering::BeginRendering(cmd, rpInfo);

            VkViewport vp{}; vp.width = (float)width; vp.height = (float)height; vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D scRect{}; scRect.extent = { width, height };
            vkCmdSetScissor(cmd, 0, 1, &scRect);

            sys.m_FrameDebugger.depthPipeline->Bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                sys.m_FrameDebugger.depthPipeline->GetLayout(), 0, 1, &sys.m_FrameDebugger.descSet, 0, nullptr);

            float pc[2] = { nearZ, farZ };
            vkCmdPushConstants(cmd, sys.m_FrameDebugger.depthPipeline->GetLayout(),
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

            // Fullscreen triangle baked into the vertex shader (3 verts, no VB).
            vkCmdDraw(cmd, 3, 1, 0, 0);

            DynamicRendering::EndRendering(cmd);

            // Preview → SHADER_READ_ONLY for ImGui sampling.
            VkImageMemoryBarrier2 fin{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            fin.srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            fin.srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            fin.dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            fin.dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            fin.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            fin.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fin.image               = dstImg;
            fin.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fin.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fin.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depFin{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depFin.imageMemoryBarrierCount = 1;
            depFin.pImageMemoryBarriers    = &fin;
            vkCmdPipelineBarrier2(cmd, &depFin);
        });

        m_DepthPreviewKey = key;
    }
}
