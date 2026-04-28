#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    RG::ResourceHandle RenderPipeline::AddOutlinePass(
        RG::RenderGraph& rg, RG::ResourceHandle ldrOutput, SelectionMaskOutput maskOutput, RG::ResourceHandle sceneDepth)
    {
        if (!m_OutlinePipeline || !m_CurrentView->targets->GetLDROutput())
            return ldrOutput;

        struct OutlinePassData {
            RG::ResourceHandle output;
            RG::ResourceHandle maskInput;
            RG::ResourceHandle selDepthInput;
            RG::ResourceHandle scnDepthInput;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<OutlinePassData>("OutlinePass",
            [&](OutlinePassData& data, RG::RenderPassBuilder& builder)
            {
                // Write to LDR output (alpha-blend outline on top)
                data.output = builder.Write(ldrOutput,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);

                // Read selection mask, selection depth, and scene depth
                data.maskInput     = builder.Read(maskOutput.mask);
                data.selDepthInput = builder.Read(maskOutput.depth);
                data.scnDepthInput = builder.Read(sceneDepth);

                outputHandle = data.output;
            },
            [this](OutlinePassData& data, RG::RenderPassContext& ctx)
            {
                m_System.m_FrameDebugger.BeginCapturePass(ctx.passIndex, "OutlinePass", "LDROutput", false,
                    { "outline", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_OutlinePipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_OutlinePipeline->GetLayout(), 0, 1, &m_CurrentViewResources->outlineDescSet, 0, nullptr);

                u32 w = m_CurrentView->targets->GetLDROutput()->GetWidth();
                u32 h = m_CurrentView->targets->GetLDROutput()->GetHeight();

                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                // Push constants: outlineWidth, texelSize, outlineColor, occludedAlpha
                struct OutlinePushConstants {
                    float outlineWidth;
                    float texelSizeX;
                    float texelSizeY;
                    float outlineColorR;
                    float outlineColorG;
                    float outlineColorB;
                    float outlineColorA;
                    float occludedAlpha;
                } pc;

                pc.outlineWidth     = 1.5f;
                pc.texelSizeX       = 1.0f / (float)w;
                pc.texelSizeY       = 1.0f / (float)h;
                pc.outlineColorR    = m_System.m_OutlineColor.r;
                pc.outlineColorG    = m_System.m_OutlineColor.g;
                pc.outlineColorB    = m_System.m_OutlineColor.b;
                pc.outlineColorA    = m_System.m_OutlineColor.a;
                pc.occludedAlpha    = 0.65f;

                vkCmdPushConstants(cmd, m_OutlinePipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                m_System.m_FrameDebugger.CaptureDrawCall("OutlinePass", "FullscreenTriangle", "OutlinePass", 0, 0, dummyPC,
                    { "outline", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                m_System.m_FrameDebugger.EndCapturePass();
            }
        );

        return outputHandle;
    }

}
