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
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    RG::ResourceHandle RenderPipeline::AddSkyboxPass(
        RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth)
    {
        struct SkyboxPassData {
            RG::ResourceHandle colorTex;
            RG::ResourceHandle depthTex;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<SkyboxPassData>("SkyboxPass",
            [&](SkyboxPassData& data, RG::RenderPassBuilder& builder)
            {
                // Load existing scene color and depth from geometry pass
                data.colorTex = builder.Write(sceneColor,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                data.depthTex = builder.WriteDepth(sceneDepth,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE);

                outputHandle = data.colorTex;
            },
            [this](SkyboxPassData& data, RG::RenderPassContext& ctx)
            {
                m_System.m_FrameDebugger.BeginCapturePass("SkyboxPass", "SceneColor", false,
                    { "skybox", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, false, false });

                if (!m_SkyboxPipeline || !m_SkyboxVB) { m_System.m_FrameDebugger.EndCapturePass(); return; }

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_SkyboxPipeline->Bind(cmd);

                // Bind all 5 descriptor sets (skybox only uses set 0, others required by layout)
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet,
                    BoneMatrixBuffer::GetDescriptorSet()
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_SkyboxPipeline->GetLayout(), 0, 5, sets, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.colorTex);
                VkViewport viewport{};
                viewport.width  = (float)res->desc.width;
                viewport.height = (float)res->desc.height;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                VkBuffer vb = m_SkyboxVB->GetVulkanBuffer();
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
                vkCmdDraw(cmd, 36, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                m_System.m_FrameDebugger.CaptureDrawCall("SkyboxPass", "SkyboxCube", "Skybox", 0, 0, dummyPC,
                    { "skybox", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, false, false });
                m_System.m_FrameDebugger.EndCapturePass();
            }
        );
        return outputHandle;
    }

}
