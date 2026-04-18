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

    RG::ResourceHandle RenderPipeline::AddBloomPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneColor)
    {
        if (!m_BloomExtractPipeline || !m_BloomBlurPipeline || !m_BloomA || !m_BloomB)
            return {}; // Post-process not initialized, skip bloom

        struct BloomPassData {
            RG::ResourceHandle output;
            RG::ResourceHandle input;
        };

        u32 halfW = m_BloomA->GetWidth();
        u32 halfH = m_BloomA->GetHeight();

        auto bloomAVk = std::static_pointer_cast<VKTexture>(m_BloomA);
        auto bloomBVk = std::static_pointer_cast<VKTexture>(m_BloomB);

        // --- Bloom Extract: SceneColor -> BloomA ---
        RG::ResourceHandle bloomAHandle;
        rg.AddPass<BloomPassData>("BloomExtract",
            [&](BloomPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "BloomA";
                desc.width  = halfW;
                desc.height = halfH;
                desc.format = RG::TextureFormat::RGBA16_Float;

                data.output = rg.ImportResource(desc,
                    (void*)bloomAVk->GetImage(), (void*)bloomAVk->GetImageView(),
                    RG::ResourceState::Undefined);
                data.output = builder.Write(data.output);

                data.input = builder.Read(sceneColor);
                bloomAHandle = data.output;
            },
            [this, halfW, halfH](BloomPassData& data, RG::RenderPassContext& ctx)
            {
                m_System.m_FrameDebugger.BeginCapturePass("BloomExtract", "BloomA", false,
                    { "bloomExtract", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_BloomExtractPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_BloomExtractPipeline->GetLayout(), 0, 1, &m_BloomExtractDescSet, 0, nullptr);

                VkViewport vp{}; vp.width = (float)halfW; vp.height = (float)halfH; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { halfW, halfH };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                float pc[4] = { m_System.m_PostProcessSettings.bloomThreshold, 0, 0, 0 };
                vkCmdPushConstants(cmd, m_BloomExtractPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                m_System.m_FrameDebugger.CaptureDrawCall("BloomExtract", "FullscreenTriangle", "BloomExtract", 0, 0, dummyPC,
                    { "bloomExtract", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                m_System.m_FrameDebugger.EndCapturePass();
            }
        );

        // --- Bloom Blur Horizontal: BloomA -> BloomB ---
        RG::ResourceHandle bloomBHandle;
        rg.AddPass<BloomPassData>("BloomBlurH",
            [&](BloomPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "BloomB";
                desc.width  = halfW;
                desc.height = halfH;
                desc.format = RG::TextureFormat::RGBA16_Float;

                data.output = rg.ImportResource(desc,
                    (void*)bloomBVk->GetImage(), (void*)bloomBVk->GetImageView(),
                    RG::ResourceState::Undefined);
                data.output = builder.Write(data.output);

                data.input = builder.Read(bloomAHandle);
                bloomBHandle = data.output;
            },
            [this, halfW, halfH](BloomPassData& data, RG::RenderPassContext& ctx)
            {
                m_System.m_FrameDebugger.BeginCapturePass("BloomBlurH", "BloomB", false,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_BloomBlurPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_BloomBlurPipeline->GetLayout(), 0, 1, &m_BloomBlurHDescSet, 0, nullptr);

                VkViewport vp{}; vp.width = (float)halfW; vp.height = (float)halfH; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { halfW, halfH };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                float pc[4] = { 1.0f / (float)halfW, 0.0f, 0.0f, 0.0f };
                vkCmdPushConstants(cmd, m_BloomBlurPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                m_System.m_FrameDebugger.CaptureDrawCall("BloomBlurH", "FullscreenTriangle", "BloomBlurH", 0, 0, dummyPC,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                m_System.m_FrameDebugger.EndCapturePass();
            }
        );

        // --- Bloom Blur Vertical: BloomB -> BloomA ---
        RG::ResourceHandle finalBloomHandle;
        rg.AddPass<BloomPassData>("BloomBlurV",
            [&](BloomPassData& data, RG::RenderPassBuilder& builder)
            {
                // Re-import BloomA with a new identity for the second write
                RG::TextureDesc desc;
                desc.name   = "BloomAFinal";
                desc.width  = halfW;
                desc.height = halfH;
                desc.format = RG::TextureFormat::RGBA16_Float;

                data.output = rg.ImportResource(desc,
                    (void*)bloomAVk->GetImage(), (void*)bloomAVk->GetImageView(),
                    RG::ResourceState::Undefined);
                data.output = builder.Write(data.output);

                data.input = builder.Read(bloomBHandle);
                finalBloomHandle = data.output;
            },
            [this, halfW, halfH](BloomPassData& data, RG::RenderPassContext& ctx)
            {
                m_System.m_FrameDebugger.BeginCapturePass("BloomBlurV", "BloomAFinal", false,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_BloomBlurPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_BloomBlurPipeline->GetLayout(), 0, 1, &m_BloomBlurVDescSet, 0, nullptr);

                VkViewport vp{}; vp.width = (float)halfW; vp.height = (float)halfH; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { halfW, halfH };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                float pc[4] = { 0.0f, 1.0f / (float)halfH, 0.0f, 0.0f };
                vkCmdPushConstants(cmd, m_BloomBlurPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                m_System.m_FrameDebugger.CaptureDrawCall("BloomBlurV", "FullscreenTriangle", "BloomBlurV", 0, 0, dummyPC,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                m_System.m_FrameDebugger.EndCapturePass();
            }
        );

        return finalBloomHandle;
    }

}
