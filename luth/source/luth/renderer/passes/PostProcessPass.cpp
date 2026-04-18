#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/BoneMatrixBuffer.h"
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

    RG::ResourceHandle RenderingSystem::AddPostProcessPass(
        RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle bloomResult)
    {
        if (!m_PostProcessPipeline || !m_Targets.GetLDROutput())
            return sceneColor; // Fallback: pass HDR scene color through

        struct PostProcessPassData {
            RG::ResourceHandle output;
            RG::ResourceHandle hdrInput;
            RG::ResourceHandle bloomInput;
        };

        RG::ResourceHandle outputHandle;
        auto ldrVk = std::static_pointer_cast<VKTexture>(m_Targets.GetLDROutput());

        rg.AddPass<PostProcessPassData>("PostProcess",
            [&](PostProcessPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "LDROutput";
                desc.width  = m_Targets.GetLDROutput()->GetWidth();
                desc.height = m_Targets.GetLDROutput()->GetHeight();
                desc.format = RG::TextureFormat::RGBA8_Unorm;

                data.output = rg.ImportResource(desc,
                    (void*)ldrVk->GetImage(), (void*)ldrVk->GetImageView(),
                    RG::ResourceState::ShaderResource);
                data.output = builder.Write(data.output);

                data.hdrInput = builder.Read(sceneColor);
                if (bloomResult.IsValid())
                    data.bloomInput = builder.Read(bloomResult);

                outputHandle = data.output;
            },
            [this](PostProcessPassData& data, RG::RenderPassContext& ctx)
            {
                m_FrameDebugger.BeginCapturePass("PostProcess", "LDROutput", false,
                    { "postprocess", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_PostProcessPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_PostProcessPipeline->GetLayout(), 0, 1, &m_CompositeDescSet, 0, nullptr);

                u32 w = m_Targets.GetLDROutput()->GetWidth();
                u32 h = m_Targets.GetLDROutput()->GetHeight();

                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                m_FrameDebugger.CaptureDrawCall("PostProcess", "FullscreenTriangle", "PostProcess", 0, 0, dummyPC,
                    { "postprocess", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                m_FrameDebugger.EndCapturePass();
            }
        );

        return outputHandle;
    }

}
