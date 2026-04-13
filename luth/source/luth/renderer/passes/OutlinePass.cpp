#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/MaterialSystem.h"
#include "luth/renderer/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/ShaderCompiler.h"
#include <glm/gtc/matrix_transform.hpp>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    RG::ResourceHandle RenderingSystem::AddOutlinePass(
        RG::RenderGraph& rg, RG::ResourceHandle ldrOutput, SelectionMaskOutput maskOutput, RG::ResourceHandle sceneDepth)
    {
        if (!m_OutlinePipeline || !m_LDROutput)
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
                m_FrameDebugger.BeginCapturePass("OutlinePass", "LDROutput", false,
                    { "outline", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_OutlinePipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_OutlinePipeline->GetLayout(), 0, 1, &m_OutlineDescSet, 0, nullptr);

                u32 w = m_LDROutput->GetWidth();
                u32 h = m_LDROutput->GetHeight();

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
                pc.outlineColorR    = m_OutlineColor.r;
                pc.outlineColorG    = m_OutlineColor.g;
                pc.outlineColorB    = m_OutlineColor.b;
                pc.outlineColorA    = m_OutlineColor.a;
                pc.occludedAlpha    = 0.65f;

                vkCmdPushConstants(cmd, m_OutlinePipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                m_FrameDebugger.CaptureDrawCall("OutlinePass", "FullscreenTriangle", "OutlinePass", 0, 0, dummyPC,
                    { "outline", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                m_FrameDebugger.EndCapturePass();
            }
        );

        return outputHandle;
    }

}
