#include "luthpch.h"
#include "luth/renderer/debug/FrameDebuggerContext.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
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
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/FrameData.h"
#include "luth/core/EditorHooks.h"

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
        auto& fd = m_Pipeline.GetSystem().GetFrameDebugger();
        if (fd.blitPipeline) return; // Already initialized

        if (auto sh = ShaderLibrary::LoadEngine("shaders/debugBlit.frag"))
            fd.blitFragSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/debugDepth.frag"))
            fd.depthFragSpv = sh->GetSpirV();

        if (fd.blitFragSpv.empty() || fd.depthFragSpv.empty() || m_Pipeline.GetPostProcess().GetFullscreenVertSpv().empty())
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
            blitConfig, m_Pipeline.GetPostProcess().GetFullscreenVertSpv(), fd.blitFragSpv, layouts);

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
            depthConfig, m_Pipeline.GetPostProcess().GetFullscreenVertSpv(), fd.depthFragSpv, layouts);
    }

    RG::ResourceHandle FrameDebuggerContext::AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth)
    {
        auto& sys = m_Pipeline.GetSystem();
        if (!sys.GetFrameDebugger().blitPipeline || !sys.GetSceneTargets().GetLDROutput()) return inputHandle;

        struct DebugBlitData {
            RG::ResourceHandle output;
            RG::ResourceHandle input;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<DebugBlitData>("DebugDisplayBlit",
            [&](DebugBlitData& data, RG::RenderPassBuilder& builder)
            {
                auto ldrVk = std::static_pointer_cast<VKTexture>(sys.GetSceneTargets().GetLDROutput());
                RG::TextureDesc desc;
                desc.name   = "LDROutput";
                desc.width  = sys.GetSceneTargets().GetLDROutput()->GetWidth();
                desc.height = sys.GetSceneTargets().GetLDROutput()->GetHeight();
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
                auto& sys = m_Pipeline.GetSystem();
                VkCommandBuffer cmd = ctx.commandBuffer;

                u32 w = sys.GetSceneTargets().GetLDROutput()->GetWidth();
                u32 h = sys.GetSceneTargets().GetLDROutput()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                if (isDepth && sys.GetFrameDebugger().depthPipeline)
                {
                    sys.GetFrameDebugger().depthPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        sys.GetFrameDebugger().depthPipeline->GetLayout(), 0, 1, &sys.GetFrameDebugger().descSet, 0, nullptr);

                    float pc[2] = { 0.1f, 200.0f }; // near/far for shadow maps
                    vkCmdPushConstants(cmd, sys.GetFrameDebugger().depthPipeline->GetLayout(),
                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
                }
                else
                {
                    sys.GetFrameDebugger().blitPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        sys.GetFrameDebugger().blitPipeline->GetLayout(), 0, 1, &sys.GetFrameDebugger().descSet, 0, nullptr);
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

    namespace
    {
        // Trailing integer of "ShadowPass.C0" / "ShadowPass.C12" etc.
        // Returns -1 if the pass name doesn't match the cascade prefix.
        int CascadeIndexFromPassName(const std::string& name)
        {
            constexpr const char* k_Prefix = "ShadowPass.C";
            const size_t prefixLen = std::char_traits<char>::length(k_Prefix);
            if (name.size() <= prefixLen) return -1;
            if (name.compare(0, prefixLen, k_Prefix) != 0) return -1;
            int v = 0;
            for (size_t i = prefixLen; i < name.size(); ++i)
            {
                char c = name[i];
                if (c < '0' || c > '9') return -1;
                v = v * 10 + (c - '0');
            }
            return v;
        }
    }

    bool FrameDebuggerContext::ValidateCapturedView()
    {
        auto& sys = m_Pipeline.GetSystem();
        auto& cf  = sys.GetFrameDebugger().capturedFrame;
        if (m_Pipeline.HasViewResources(cf.capturedView.targets, cf.capturedView.viewResourcesId))
            return true;

        // Captured view's panel was closed mid-Freeze — clear the capture so
        // the panel returns to live mode rather than serving stale archives
        // against a missing view.
        sys.ExitCapture();
        if (auto* hooks = EditorHooks::Get())
            hooks->OnFrameDebuggerNotice("Captured view closed; capture cleared.");
        return false;
    }

    void FrameDebuggerContext::ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx)
    {
        auto& sys = m_Pipeline.GetSystem();

        if (sys.GetFrameDebugger().state != DebuggerState::Frozen) return;
        if (!sys.GetFrameDebugger().capturedFrame.valid) return;
        if (passIdx >= sys.GetFrameDebugger().capturedFrame.passes.size()) return;
        if (!ValidateCapturedView()) return;

        const auto& pass = sys.GetFrameDebugger().capturedFrame.passes[passIdx];

        // Dispatch by pass type. Unsupported pass names leave
        // m_PerDrawPreviewKey untouched so the caller (panel / viewport
        // overlay) detects the mismatch and falls back to the pass archive.
        if (pass.name == "GeometryPass")
        {
            ReplayGeometry(passIdx, localDrawIdx);
        }
        else if (const int cascadeIdx = CascadeIndexFromPassName(pass.name); cascadeIdx >= 0)
        {
            ReplayShadow(passIdx, localDrawIdx, cascadeIdx);
        }
        else if (pass.name == "DepthPrepass")
        {
            ReplayDepthPrepass(passIdx, localDrawIdx);
        }
        else if (pass.name == "SelectionMaskPass")
        {
            ReplaySelectionMask(passIdx, localDrawIdx);
        }
    }

    void FrameDebuggerContext::ReplayGeometry(u32 passIdx, u32 localDrawIdx)
    {
        // invariant: ReplayPassUpToDraw already validated cf.capturedView via
        // ValidateCapturedView — replay must render against THIS view's targets,
        // not m_CurrentViewResources (which points at whichever view ran last).
        auto& sys = m_Pipeline.GetSystem();
        auto& cf  = sys.GetFrameDebugger().capturedFrame;
        FrameTargets* targets = cf.capturedView.targets;
        if (!targets || !targets->GetSceneColor() || !targets->GetSceneDepth() || !targets->GetEntityIDBuffer()) return;

        ViewResources* capturedVr = m_Pipeline.GetViewResources(targets);
        if (!capturedVr) return;

        // Cache hit — same selection as last replay, nothing to do.
        const u64 key = ((u64)passIdx << 32) | (u64)localDrawIdx;
        if (key == m_PerDrawPreviewKey) return;

        const u32 width  = targets->GetSceneColor()->GetWidth();
        const u32 height = targets->GetSceneColor()->GetHeight();
        EnsurePerDrawPreviewTexture(width, height);
        if (m_PerDrawPreviewImage == VK_NULL_HANDLE) return;

        auto vkSceneColor = std::static_pointer_cast<VKTexture>(targets->GetSceneColor());
        auto vkSceneDepth = std::static_pointer_cast<VKTexture>(targets->GetSceneDepth());
        auto vkEntityID   = std::static_pointer_cast<VKTexture>(targets->GetEntityIDBuffer());
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
        VkPolygonMode polyMode = (sys.GetShadeMode() == ShadeMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        UUID pbrUUID = ShaderLibrary::Get("pbr.vert")->Handle;

        VulkanContext::Get().ImmediateSubmit([&, this](VkCommandBuffer cmd)
        {
            auto& sys = m_Pipeline.GetSystem();
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
            auto* opaquePipeline = rp.GetGeometry().GetGeoPipelineManager().GetOrCreate(
                pbrUUID, Material::RenderMode::Opaque, Material::CullMode::Back,
                polyMode, rp.GetGeometry().GetPBRVertSpv(), rp.GetGeometry().GetPBRFragSpv());
            if (!opaquePipeline)
            {
                DynamicRendering::EndRendering(cmd);
                return;
            }
            VkPipelineLayout pipelineLayout = opaquePipeline->GetLayout();

            // Use captured slot — Frozen state pins descriptor data; live
            // GetRenderFrameIndex() would index a slot the live loop has rotated past.
            // invariant: do NOT rewrite Set 0 binding 0 here — the set may be in
            // use by a still-pending cmd buffer from before Frozen engaged (no
            // UAB flag set on Set 0). Captured-time region stays alive across
            // Frozen because the live RG isn't running to advance FreeTag.
            const u32 slot = cf.capturedRenderFrameIndex % MAX_FRAMES_IN_FLIGHT;

            VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
            VkDescriptorSet sets[] = {
                capturedVr->globalDescriptorSet[slot], bindlessSet, MaterialSystem::GetDescriptorSet(slot),
                rp.GetLighting().GetLightDescSet(slot), BoneMatrixBuffer::GetDescriptorSet(slot), rp.GetGeometry().GetObjectSSBODescSet(slot)
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
                auto* pipeline = rp.GetGeometry().GetGeoPipelineManager().GetOrCreate(
                    pbrUUID, mode, currentCull, polyMode, rp.GetGeometry().GetPBRVertSpv(), rp.GetGeometry().GetPBRFragSpv());
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
                            ? rp.GetGeometry().GetGeoSkinnedPipelineManager().GetOrCreate(pbrUUID, mode, currentCull, polyMode, rp.GetGeometry().GetPBRSkinnedVertSpv(), rp.GetGeometry().GetPBRFragSpv())
                            : rp.GetGeometry().GetGeoPipelineManager().GetOrCreate       (pbrUUID, mode, currentCull, polyMode, rp.GetGeometry().GetPBRVertSpv(),        rp.GetGeometry().GetPBRFragSpv());
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

                    // invariant: cmdIndex must match the camera-region layout the live cull wrote into.
                    const u32 viewBaseRegion = cf.capturedView.viewIndex * RenderPipeline::k_IndirectRegionsPerView;
                    const u32 cmdIndex = viewBaseRegion * RenderPipeline::k_IndirectRegionStride + dc.gpuObjectIndex;
                    VkDeviceSize indirectOffset = rp.GetGeometry().GetIndirectRegion().offset + cmdIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, rp.GetGeometry().GetIndirectRegion().buffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));

                    --drawsRemaining;
                }
            };

            ReplayBatch(sys.GetDrawList().opaque,      Material::RenderMode::Opaque);
            ReplayBatch(sys.GetDrawList().cutout,      Material::RenderMode::Cutout);
            ReplayBatch(sys.GetDrawList().transparent, Material::RenderMode::Transparent);

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

    void FrameDebuggerContext::ReplayShadow(u32 passIdx, u32 localDrawIdx, int cascadeIdx)
    {
        auto& sys = m_Pipeline.GetSystem();
        auto& cf  = sys.GetFrameDebugger().capturedFrame;
        if (cascadeIdx < 0 || cascadeIdx >= (int)k_ShadowCascadeCount) return;

        const u64 key = ((u64)passIdx << 32) | (u64)localDrawIdx;
        if (key == m_PerDrawPreviewKey) return;

        ViewResources* capturedVr = m_Pipeline.GetViewResources(cf.capturedView.targets);
        if (!capturedVr) return;

        auto& lighting = m_Pipeline.GetLighting();
        VkImageView shadowLayerView = lighting.GetShadowLayerView((u32)cascadeIdx);
        auto vkShadowMap = std::static_pointer_cast<VKTexture>(lighting.GetShadowMap());
        VKPipeline* shadowPipeline = lighting.GetShadowPipeline();
        VKPipeline* shadowSkinnedPipeline = lighting.GetShadowSkinnedPipeline();
        if (!shadowPipeline || !vkShadowMap || shadowLayerView == VK_NULL_HANDLE) return;

        VkImage shadowImg = vkShadowMap->GetImage();

        // invariant: live RG halts in Frozen, so writing through the cascade-layer
        // view into m_ShadowMap doesn't race the renderer. Next ExitCapture → live
        // frame fully clears+rewrites this cascade.
        InitDebugBlitResources();
        if (!sys.GetFrameDebugger().depthPipeline) return;

        EnsureDepthPreviewTexture(k_ShadowResolution, k_ShadowResolution);
        EnsurePerDrawPreviewTexture(k_ShadowResolution, k_ShadowResolution);
        if (m_DepthPreviewImage == VK_NULL_HANDLE || m_PerDrawPreviewImage == VK_NULL_HANDLE) return;

        // Update fd.descSet to sample the shadow cascade view. Pre-existing UAB
        // hazard noted in spawned task; ImmediateSubmit's fence makes back-to-back
        // calls safe in practice.
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = sys.GetFrameDebugger().sampler;
        imgInfo.imageView   = shadowLayerView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = sys.GetFrameDebugger().descSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);

        const u32 cascade = (u32)cascadeIdx;
        const u32 maxDraws = localDrawIdx + 1;

        VulkanContext::Get().ImmediateSubmit([&, this, cascade, maxDraws](VkCommandBuffer cmd)
        {
            auto& rp = m_Pipeline;

            // Shadow cascade slice → DEPTH_ATTACHMENT (UNDEFINED to discard prior).
            VkImageMemoryBarrier2 prep{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            prep.srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            prep.dstStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            prep.dstAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            prep.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            prep.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            prep.image               = shadowImg;
            prep.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, cascade, 1 };
            prep.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            prep.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depPrep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depPrep.imageMemoryBarrierCount = 1;
            depPrep.pImageMemoryBarriers    = &prep;
            vkCmdPipelineBarrier2(cmd, &depPrep);

            AttachmentInfo depthAtt{};
            depthAtt.ImageView  = shadowLayerView;
            depthAtt.Format     = VK_FORMAT_D32_SFLOAT;
            depthAtt.LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.ClearValue.depthStencil = { 1.0f, 0 };
            depthAtt.Layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            RenderPassInfo rpInfo{};
            rpInfo.DepthAttachment = &depthAtt;
            rpInfo.RenderArea      = { {0, 0}, { k_ShadowResolution, k_ShadowResolution } };

            DynamicRendering::BeginRendering(cmd, rpInfo);

            const u32 slot = cf.capturedRenderFrameIndex % MAX_FRAMES_IN_FLIGHT;
            VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
            VkDescriptorSet sets[] = {
                capturedVr->globalDescriptorSet[slot],
                bindlessSet,
                MaterialSystem::GetDescriptorSet(slot),
                rp.GetLighting().GetLightDescSet(slot),
                BoneMatrixBuffer::GetDescriptorSet(slot),
                rp.GetGeometry().GetObjectSSBODescSet(slot)
            };

            shadowPipeline->Bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                shadowPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
            vkCmdPushConstants(cmd, shadowPipeline->GetLayout(),
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(u32), &cascade);

            VkViewport vp{}; vp.width = (float)k_ShadowResolution; vp.height = (float)k_ShadowResolution; vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D sc{}; sc.extent = { k_ShadowResolution, k_ShadowResolution };
            vkCmdSetScissor(cmd, 0, 1, &sc);

            u32 drawsRemaining = maxDraws;
            bool currentSkinned = false;

            auto ReplayBatch = [&](const std::vector<DrawCommand>& draws)
            {
                if (drawsRemaining == 0) return;
                for (const auto& dc : draws)
                {
                    if (drawsRemaining == 0) return;
                    if (dc.isSkinned != currentSkinned)
                    {
                        currentSkinned = dc.isSkinned;
                        VKPipeline* p = currentSkinned ? shadowSkinnedPipeline : shadowPipeline;
                        if (!p) { drawsRemaining--; continue; }
                        p->Bind(cmd);
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p->GetLayout(), 0, 6, sets, 0, nullptr);
                        vkCmdPushConstants(cmd, p->GetLayout(),
                            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(u32), &cascade);
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

                    const u32 viewBaseRegion = cf.capturedView.viewIndex * RenderPipeline::k_IndirectRegionsPerView;
                    const u32 cmdIndex = (viewBaseRegion + cascade + 1) * RenderPipeline::k_IndirectRegionStride + dc.gpuObjectIndex;
                    VkDeviceSize indirectOffset = rp.GetGeometry().GetIndirectRegion().offset + cmdIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, rp.GetGeometry().GetIndirectRegion().buffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));
                    --drawsRemaining;
                }
            };

            ReplayBatch(sys.GetDrawList().opaque);
            ReplayBatch(sys.GetDrawList().cutout);
            ReplayBatch(sys.GetDrawList().transparent);

            DynamicRendering::EndRendering(cmd);

            // Cascade slice DEPTH_WRITE → SHADER_READ for the tonemap pass.
            VkImageMemoryBarrier2 toRead{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            toRead.srcStageMask        = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            toRead.srcAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            toRead.dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toRead.dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            toRead.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            toRead.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.image               = shadowImg;
            toRead.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, cascade, 1 };
            toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            // m_DepthPreviewImage UNDEFINED → COLOR_ATTACHMENT for tonemap output.
            VkImageMemoryBarrier2 dpPrep{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            dpPrep.srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            dpPrep.dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            dpPrep.dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            dpPrep.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            dpPrep.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            dpPrep.image               = m_DepthPreviewImage;
            dpPrep.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            dpPrep.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            dpPrep.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkImageMemoryBarrier2 mid[2] = { toRead, dpPrep };
            VkDependencyInfo depMid{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depMid.imageMemoryBarrierCount = 2;
            depMid.pImageMemoryBarriers    = mid;
            vkCmdPipelineBarrier2(cmd, &depMid);

            // Tonemap shadow depth → m_DepthPreviewImage (RGBA8).
            AttachmentInfo dpAtt{};
            dpAtt.ImageView  = m_DepthPreviewView;
            dpAtt.Format     = VK_FORMAT_R8G8B8A8_UNORM;
            dpAtt.LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            dpAtt.StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            dpAtt.ClearValue = { { {0.f, 0.f, 0.f, 1.f} } };
            dpAtt.Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            RenderPassInfo dpRp{};
            dpRp.ColorAttachments = std::span<AttachmentInfo>(&dpAtt, 1);
            dpRp.RenderArea       = { {0, 0}, { k_ShadowResolution, k_ShadowResolution } };

            DynamicRendering::BeginRendering(cmd, dpRp);
            VkViewport dpVp{}; dpVp.width = (float)k_ShadowResolution; dpVp.height = (float)k_ShadowResolution; dpVp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &dpVp);
            VkRect2D dpSc{}; dpSc.extent = { k_ShadowResolution, k_ShadowResolution };
            vkCmdSetScissor(cmd, 0, 1, &dpSc);

            sys.GetFrameDebugger().depthPipeline->Bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                sys.GetFrameDebugger().depthPipeline->GetLayout(), 0, 1, &sys.GetFrameDebugger().descSet, 0, nullptr);

            const u32 prevSplitIdx = (cascade == 0) ? 0 : (cascade - 1);
            const float nearZ = (cascade == 0) ? 0.1f : cf.cascadeSplitsViewZ[prevSplitIdx];
            const float farZ  = cf.cascadeSplitsViewZ[cascade];
            float pc[2] = { nearZ, farZ <= nearZ ? nearZ + 1.0f : farZ };
            vkCmdPushConstants(cmd, sys.GetFrameDebugger().depthPipeline->GetLayout(),
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            DynamicRendering::EndRendering(cmd);

            // Blit m_DepthPreviewImage (RGBA8) → m_PerDrawPreviewImage (RGBA16F)
            // so the panel reads progressive shadow depth via GetPerDrawPreviewView.
            VkImageMemoryBarrier2 toBlit[2]{};
            toBlit[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toBlit[0].srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toBlit[0].srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toBlit[0].dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toBlit[0].dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
            toBlit[0].oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toBlit[0].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toBlit[0].image               = m_DepthPreviewImage;
            toBlit[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toBlit[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toBlit[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            toBlit[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toBlit[1].srcStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toBlit[1].srcAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            toBlit[1].dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toBlit[1].dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toBlit[1].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            toBlit[1].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toBlit[1].image               = m_PerDrawPreviewImage;
            toBlit[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toBlit[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toBlit[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depBlit{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depBlit.imageMemoryBarrierCount = 2;
            depBlit.pImageMemoryBarriers    = toBlit;
            vkCmdPipelineBarrier2(cmd, &depBlit);

            VkImageBlit2 blit{ VK_STRUCTURE_TYPE_IMAGE_BLIT_2 };
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[0] = { 0, 0, 0 };
            blit.srcOffsets[1] = { (i32)k_ShadowResolution, (i32)k_ShadowResolution, 1 };
            blit.dstOffsets[0] = { 0, 0, 0 };
            blit.dstOffsets[1] = { (i32)m_PerDrawPreviewWidth, (i32)m_PerDrawPreviewHeight, 1 };

            VkBlitImageInfo2 blitInfo{ VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2 };
            blitInfo.srcImage       = m_DepthPreviewImage;
            blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            blitInfo.dstImage       = m_PerDrawPreviewImage;
            blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blitInfo.regionCount    = 1;
            blitInfo.pRegions       = &blit;
            blitInfo.filter         = VK_FILTER_LINEAR;
            vkCmdBlitImage2(cmd, &blitInfo);

            // Final transitions: previews → SHADER_READ; shadow stays SHADER_READ
            // (next live frame's ShadowPass clear-load resets it).
            VkImageMemoryBarrier2 fin[2]{};
            fin[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            fin[0].srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            fin[0].srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
            fin[0].dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            fin[0].dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            fin[0].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            fin[0].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fin[0].image               = m_DepthPreviewImage;
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
        m_DepthPreviewKey   = UINT64_MAX;  // archive depth blit cache invalid
    }

    void FrameDebuggerContext::ReplayDepthPrepass(u32 passIdx, u32 localDrawIdx)
    {
        // TODO(I2-DepthPrepass): replay depth-only draws[0..localDrawIdx] into
        // SceneDepth, then tonemap into m_PerDrawPreviewImage.
        (void)passIdx; (void)localDrawIdx;
    }

    void FrameDebuggerContext::ReplaySelectionMask(u32 passIdx, u32 localDrawIdx)
    {
        // TODO(I2-SelectionMask): replay selection draws[0..localDrawIdx] into
        // the selection mask buffer, then copy into m_PerDrawPreviewImage.
        (void)passIdx; (void)localDrawIdx;
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
        auto& sys = m_Pipeline.GetSystem();

        if (sys.GetFrameDebugger().state != DebuggerState::Frozen) return;
        if (!sys.GetFrameDebugger().capturedFrame.valid) return;
        if (archiveIdx >= sys.GetFrameDebugger().capturedFrame.archivedImages.size()) return;

        auto& archive = sys.GetFrameDebugger().capturedFrame.archivedImages[archiveIdx];
        if (!archive.isDepth || archive.image == VK_NULL_HANDLE) return;

        InitDebugBlitResources();  // idempotent — needed for sampler + depthPipeline + descSet
        if (!sys.GetFrameDebugger().depthPipeline || sys.GetFrameDebugger().descSet == VK_NULL_HANDLE) return;

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
        imgInfo.sampler     = sys.GetFrameDebugger().sampler;
        imgInfo.imageView   = srcView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = sys.GetFrameDebugger().descSet;
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
            auto& sys = m_Pipeline.GetSystem();

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

            sys.GetFrameDebugger().depthPipeline->Bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                sys.GetFrameDebugger().depthPipeline->GetLayout(), 0, 1, &sys.GetFrameDebugger().descSet, 0, nullptr);

            float pc[2] = { nearZ, farZ };
            vkCmdPushConstants(cmd, sys.GetFrameDebugger().depthPipeline->GetLayout(),
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
