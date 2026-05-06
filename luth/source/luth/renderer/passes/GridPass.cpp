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
                m_System.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "GridPass", "SceneColor", false,
                    { "grid", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_GridPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_GridPipeline->GetLayout(), 0, 1, &m_CurrentViewResources->gridDescSet, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.colorTex);
                VkViewport vp{};
                vp.width  = (float)res->desc.width;
                vp.height = (float)res->desc.height;
                vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                // Must match GridPushConstants in grid.frag (16 floats / 64 bytes).
                // All values flow from EditorSettings → EditorViewportState → CameraParams.
                const auto& cp = m_System.GetCameraParams();
                struct GridPushConstants {
                    float axisXColor[4];
                    float axisZColor[4];
                    float gridColor[4];
                    float majorScale;
                    float fadeStart;
                    float fadeEnd;
                    float lineThickness;
                } gpc{};

                gpc.axisXColor[0] = cp.gridAxisXColor.r; gpc.axisXColor[1] = cp.gridAxisXColor.g; gpc.axisXColor[2] = cp.gridAxisXColor.b; gpc.axisXColor[3] = cp.gridAxisXColor.a;
                gpc.axisZColor[0] = cp.gridAxisZColor.r; gpc.axisZColor[1] = cp.gridAxisZColor.g; gpc.axisZColor[2] = cp.gridAxisZColor.b; gpc.axisZColor[3] = cp.gridAxisZColor.a;
                gpc.gridColor[0]  = cp.gridColor.r;      gpc.gridColor[1]  = cp.gridColor.g;      gpc.gridColor[2]  = cp.gridColor.b;      gpc.gridColor[3]  = cp.gridColor.a;
                gpc.majorScale    = cp.gridMajorScale;
                gpc.fadeStart     = cp.gridFadeStart;
                gpc.fadeEnd       = cp.gridFadeEnd;
                gpc.lineThickness = cp.gridLineThickness;

                vkCmdPushConstants(cmd, m_GridPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gpc), &gpc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                m_System.GetFrameDebugger().CaptureDrawCall("GridPass", "FullscreenTriangle", "GridPass", 0, 0, dummyPC,
                    { "grid", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                m_System.GetFrameDebugger().EndCapturePass();
            }
        );

        return outputHandle;
    }

}
