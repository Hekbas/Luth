#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/animation/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/shader/ShaderCompiler.h"
#include <glm/gtc/matrix_transform.hpp>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    RG::ResourceHandle RenderPipeline::AddGridPass(
        RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth)
    {
        if (!m_GridPipeline)
            return sceneColor;

        struct GridPassData {
            RG::ResourceHandle colorTex;
            RG::ResourceHandle depthInput;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<GridPassData>("GridPass",
            [&](GridPassData& data, RG::RenderPassBuilder& builder)
            {
                // Load existing scene color and alpha-blend grid on top
                data.colorTex = builder.Write(sceneColor,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);

                // Scene depth as shader resource (not attachment)
                data.depthInput = builder.Read(sceneDepth);

                outputHandle = data.colorTex;
            },
            [this](GridPassData& data, RG::RenderPassContext& ctx)
            {
                m_System.m_FrameDebugger.BeginCapturePass("GridPass", "SceneColor", false,
                    { "grid", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_GridPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_GridPipeline->GetLayout(), 0, 1, &m_GridDescSet, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.colorTex);
                VkViewport vp{};
                vp.width  = (float)res->desc.width;
                vp.height = (float)res->desc.height;
                vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                // Must match GridPushConstants in grid.frag (16 floats / 64 bytes).
                struct GridPushConstants {
                    float axisXColor[4];
                    float axisZColor[4];
                    float gridColor[4];
                    float majorScale;
                    float fadeStart;
                    float fadeEnd;
                    float lineThickness;
                } gpc{};

                // Axis colors mirror EditorColors::AxisX/AxisZ (engine cannot depend on the editor lib).
                gpc.axisXColor[0] = 0.80f; gpc.axisXColor[1] = 0.10f; gpc.axisXColor[2] = 0.15f; gpc.axisXColor[3] = 1.00f;
                gpc.axisZColor[0] = 0.10f; gpc.axisZColor[1] = 0.25f; gpc.axisZColor[2] = 0.80f; gpc.axisZColor[3] = 1.00f;
                gpc.gridColor[0]  = 0.41f; gpc.gridColor[1]  = 0.41f; gpc.gridColor[2]  = 0.41f; gpc.gridColor[3]  = 0.50f;
                gpc.majorScale    = 1.0f;
                gpc.fadeStart     = 20.0f;
                gpc.fadeEnd       = 200.0f;
                gpc.lineThickness = 1.00f;

                vkCmdPushConstants(cmd, m_GridPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gpc), &gpc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                m_System.m_FrameDebugger.CaptureDrawCall("GridPass", "FullscreenTriangle", "GridPass", 0, 0, dummyPC,
                    { "grid", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                m_System.m_FrameDebugger.EndCapturePass();
            }
        );

        return outputHandle;
    }

}
