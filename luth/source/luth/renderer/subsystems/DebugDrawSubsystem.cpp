#include "luthpch.h"

#include "luth/renderer/subsystems/DebugDrawSubsystem.h"

#include "luth/core/DebugDraw.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/subsystems/GlobalSubsystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/scene/systems/RenderingSystem.h"

namespace Luth
{
    void DebugDrawSubsystem::Init(RenderPipeline& pipeline)
    {
        LH_PROFILE_FUNCTION();
        m_Pipeline = &pipeline;

        auto loadSpv = [](const char* relPath) -> std::vector<u32> {
            auto sh = ShaderLibrary::LoadEngine(relPath);
            return sh ? sh->GetSpirV() : std::vector<u32>{};
        };
        m_VertSpv = loadSpv("shaders/debugDraw_vert.slang");
        m_FragSpv = loadSpv("shaders/debugDraw.slang");

        if (m_VertSpv.empty() || m_FragSpv.empty())
        {
            LH_LOG(Renderer, error, "DebugDrawSubsystem: shader SPIR-V empty after asset load!");
        }
    }

    void DebugDrawSubsystem::BuildPipelines()
    {
        LH_PROFILE_FUNCTION();
        BuildLinePipeline();
    }

    void DebugDrawSubsystem::BuildLinePipeline()
    {
        LH_PROFILE_FUNCTION();
        if (m_VertSpv.empty() || m_FragSpv.empty()) return;

        // No descriptor set: viewProj rides on the push constant; vertex data binds as VBO.
        std::vector<VkDescriptorSetLayout> layouts;

        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(Mat4);

        // DebugVertex: position (vec3) + colorRGBA (RGBA8 unorm). 16 bytes total.
        VkVertexInputBindingDescription binding{};
        binding.binding   = 0;
        binding.stride    = sizeof(DebugVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attribs[2] = {};
        attribs[0].location = 0;
        attribs[0].binding  = 0;
        attribs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attribs[0].offset   = static_cast<u32>(offsetof(DebugVertex, position));
        attribs[1].location = 1;
        attribs[1].binding  = 0;
        attribs[1].format   = VK_FORMAT_R8G8B8A8_UNORM;
        attribs[1].offset   = static_cast<u32>(offsetof(DebugVertex, colorRGBA));

        PipelineConfig cfg;
        cfg.colorFormats          = { VK_FORMAT_R8G8B8A8_UNORM };  // matches LDR target
        cfg.depthFormat           = VK_FORMAT_UNDEFINED;          // depth disabled; always-visible
        cfg.depthTest             = false;
        cfg.depthWrite            = false;
        cfg.blendEnabled          = true;                         // alpha-blend over scene
        cfg.cullMode              = VK_CULL_MODE_NONE;
        cfg.frontFace             = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        cfg.topology              = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        cfg.polygonMode           = VK_POLYGON_MODE_FILL;
        cfg.bindingDescriptions   = { binding };
        cfg.attributeDescriptions = { attribs[0], attribs[1] };
        cfg.pushConstantRanges    = { pcRange };

        m_LinePipeline = std::make_unique<VKPipeline>(cfg, m_VertSpv, m_FragSpv, layouts);
    }

    void DebugDrawSubsystem::Shutdown()
    {
        LH_PROFILE_FUNCTION();
        m_LinePipeline.reset();
    }

    bool DebugDrawSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        LH_PROFILE_FUNCTION();
        if      (name == "debugDraw_vert.slang") m_VertSpv = spv;
        else if (name == "debugDraw.slang") m_FragSpv = spv;
        else return false;

        if (auto* raw = m_LinePipeline.release(); raw)
            VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        BuildLinePipeline();
        return true;
    }

    RG::ResourceHandle DebugDrawSubsystem::AddDebugDrawPass(RG::RenderGraph& rg, RG::ResourceHandle ldrOutput)
    {
        LH_PROFILE_FUNCTION();
        if (!m_LinePipeline) return ldrOutput;

        // Read the lines for the frame the render stage is consuming. Span is valid until
        // BeginGameFrame is called for the same modulo-2 slot; at least two frames out.
        const u64 renderFrameIdx = Renderer::GetFrameData()->GetRenderFrameIndex();
        const auto lines = DebugDraw::GetForRender(renderFrameIdx);
        if (lines.empty()) return ldrOutput;

        struct DebugDrawPassData { RG::ResourceHandle output; };
        RG::ResourceHandle outputHandle;

        rg.AddPass<DebugDrawPassData>("DebugDrawPass",
            [&, ldrOutput](DebugDrawPassData& data, RG::RenderPassBuilder& builder)
            {
                data.output  = builder.Write(ldrOutput, VK_ATTACHMENT_LOAD_OP_LOAD,
                                             VK_ATTACHMENT_STORE_OP_STORE);
                outputHandle = data.output;
            },
            [this, lines](DebugDrawPassData& /*data*/, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                const auto* view = m_Pipeline->GetCurrentView();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "DebugDrawPass", "LDROutput", false,
                    { "debugDraw", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_LinePipeline->Bind(cmd);

                u32 w = view->targets->GetLDROutput()->GetWidth();
                u32 h = view->targets->GetLDROutput()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                // Allocate a transient VB from the GPU heap. Tag-released two frames out by the
                // VulkanBackend's GPU-N-2 wait, so the buffer outlives this command buffer's GPU
                // execution. CurrentTag must be set per-frame: VulkanBackend retires by absolute
                // render-frame index, so a stale/zero tag here means the page is never freed.
                auto* jctx = Luth::JobSystem::GetCurrentJobContext();
                auto& cache = jctx->GpuCache;
                cache.CurrentTag = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u64 vbBytes = lines.size_bytes();
                auto region = Memory::GPUTaggedPageAllocator::Get().Allocate(cache, vbBytes, /*alignment*/16);
                std::memcpy(region.mappedPtr, lines.data(), vbBytes);
                Memory::GPUTaggedPageAllocator::Get().FlushRegion(region);

                const Mat4 viewProj = m_Pipeline->GetGlobal().GetCachedViewProj();
                vkCmdPushConstants(cmd, m_LinePipeline->GetLayout(),
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &viewProj);

                VkBuffer     vbuf[]    = { region.buffer };
                VkDeviceSize offsets[] = { region.offset };
                vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                vkCmdDraw(cmd, static_cast<u32>(lines.size()), 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("DebugDrawPass", "Lines", "DebugDrawPass",
                    0, static_cast<u32>(lines.size()), dummyPC,
                    { "debugDraw", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );

        return outputHandle;
    }
}
