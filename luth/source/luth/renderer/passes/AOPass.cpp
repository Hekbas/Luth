#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/FrameDebugger.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"

#include <cmath>
#include <vulkan/vulkan.h>

namespace Luth
{
    namespace
    {
        // std430/std140 layout for the depth prefilter push constants. Must match
        // the `PC` block in gtao_depth_prefilter.comp.
        struct GTAOPrefilterPC
        {
            IVec2 halfResSize;     // 0
            Vec2  invFullRes;      // 8
            float      nearZ;           // 16
            float      farZ;            // 20
            float      _pad0;           // 24
            float      _pad1;           // 28
        };
        static_assert(sizeof(GTAOPrefilterPC) == 32, "GTAOPrefilterPC layout mismatch");
    }

    RG::ResourceHandle RenderPipeline::AddGTAODepthPrefilterPass(
        RG::RenderGraph& rg, RG::ResourceHandle sceneDepth)
    {
        struct GTAOPrefilterData
        {
            RG::ResourceHandle sceneDepth;
            RG::ResourceHandle linearDepth;
        };

        RG::ResourceHandle outputHandle;

        rg.AddComputePass<GTAOPrefilterData>("GTAODepthPrefilter",
            [&](GTAOPrefilterData& data, RG::RenderPassBuilder& builder)
            {
                // Transition SceneDepth (depth-attachment) → SHADER_READ_ONLY for compute sampling.
                // ReadStorageImage despite the name gives us ComputeRead which maps to the right layout.
                data.sceneDepth = builder.ReadStorageImage(sceneDepth);

                // Import the persistent half-res linear depth as a storage write.
                RG::TextureDesc desc;
                desc.name   = "GTAOLinearDepth";
                desc.width  = m_CurrentViewResources->gtaoLinearDepth->GetWidth();
                desc.height = m_CurrentViewResources->gtaoLinearDepth->GetHeight();
                desc.format = RG::TextureFormat::R32_Float;

                auto vkLin = std::static_pointer_cast<VKTexture>(m_CurrentViewResources->gtaoLinearDepth);
                data.linearDepth = rg.ImportResource(desc,
                    (void*)vkLin->GetImage(),
                    (void*)vkLin->GetImageView(),
                    RG::ResourceState::Undefined);
                data.linearDepth = builder.WriteStorageImage(data.linearDepth);

                outputHandle = data.linearDepth;
            },
            [this](GTAOPrefilterData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                m_System.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "GTAODepthPrefilter", "GTAOLinearDepth", false,
                    { "gtao_depth_prefilter", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                if (!m_GTAOPrefilterPipeline || m_CurrentViewResources->gtaoPrefilterDescSet == VK_NULL_HANDLE)
                {
                    m_System.GetFrameDebugger().EndCapturePass();
                    return;
                }

                m_GTAOPrefilterPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_GTAOPrefilterPipeline->GetLayout(), 0, 1, &m_CurrentViewResources->gtaoPrefilterDescSet, 0, nullptr);

                const u32 halfW = m_CurrentViewResources->gtaoLinearDepth->GetWidth();
                const u32 halfH = m_CurrentViewResources->gtaoLinearDepth->GetHeight();
                const u32 fullW = m_CurrentView->targets->GetSceneDepth()->GetWidth();
                const u32 fullH = m_CurrentView->targets->GetSceneDepth()->GetHeight();

                GTAOPrefilterPC pc{};
                pc.halfResSize = { (i32)halfW, (i32)halfH };
                pc.invFullRes  = { 1.0f / float(fullW), 1.0f / float(fullH) };
                pc.nearZ       = m_CurrentView->camera.nearZ;
                pc.farZ        = m_CurrentView->camera.farZ;

                vkCmdPushConstants(cmd, m_GTAOPrefilterPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GTAOPrefilterPC), &pc);

                // 8x8 tiles — one thread per half-res pixel. Round up so the
                // last tile covers the edges; the shader bounds-checks.
                const u32 groupX = (halfW + 7) / 8;
                const u32 groupY = (halfH + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);

                m_System.GetFrameDebugger().CaptureComputeDispatch("GTAODepthPrefilter",
                    "gtao_depth_prefilter", groupX, groupY, 1);
                m_System.GetFrameDebugger().EndCapturePass();
            });

        return outputHandle;
    }

    namespace
    {
        struct GTAOMainPC
        {
            Vec2 projParams;   // 0   (P[0][0], |P[1][1]|)
            float     nearZ;        // 8
            float     farZ;         // 12
            u32       frameIndex;   // 16
            u32       _pad0;        // 20
            u32       _pad1;        // 24
            u32       _pad2;        // 28
        };
        static_assert(sizeof(GTAOMainPC) == 32, "GTAOMainPC layout mismatch");
    }

    RG::ResourceHandle RenderPipeline::AddGTAOMainPass(
        RG::RenderGraph& rg, RG::ResourceHandle linearDepth)
    {
        struct GTAOMainData
        {
            RG::ResourceHandle linearDepth;
            RG::ResourceHandle rawAO;
        };

        RG::ResourceHandle outputHandle;

        rg.AddComputePass<GTAOMainData>("GTAOMain",
            [&](GTAOMainData& data, RG::RenderPassBuilder& builder)
            {
                // Transition linear depth (written in prefilter as ComputeWrite) → sampled read.
                data.linearDepth = builder.ReadStorageImage(linearDepth);

                RG::TextureDesc desc;
                desc.name   = "GTAORawAO";
                desc.width  = m_CurrentViewResources->gtaoRawAO->GetWidth();
                desc.height = m_CurrentViewResources->gtaoRawAO->GetHeight();
                desc.format = RG::TextureFormat::R8_Unorm;

                auto vkRaw = std::static_pointer_cast<VKTexture>(m_CurrentViewResources->gtaoRawAO);
                data.rawAO = rg.ImportResource(desc,
                    (void*)vkRaw->GetImage(),
                    (void*)vkRaw->GetImageView(),
                    RG::ResourceState::Undefined);
                data.rawAO = builder.WriteStorageImage(data.rawAO);

                outputHandle = data.rawAO;
            },
            [this](GTAOMainData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                m_System.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "GTAOMain", "GTAORawAO", false,
                    { "gtao_main", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                if (!m_GTAOMainPipeline || m_CurrentViewResources->gtaoMainDescSet == VK_NULL_HANDLE)
                {
                    m_System.GetFrameDebugger().EndCapturePass();
                    return;
                }

                m_GTAOMainPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_GTAOMainPipeline->GetLayout(), 0, 1, &m_CurrentViewResources->gtaoMainDescSet, 0, nullptr);

                const u32 halfW = m_CurrentViewResources->gtaoRawAO->GetWidth();
                const u32 halfH = m_CurrentViewResources->gtaoRawAO->GetHeight();

                // Pull the projection factors straight from the camera matrix the
                // frame's GlobalUniforms were built with. Vulkan's Y-flipped
                // projection has P[1][1] < 0; pass the absolute value so the
                // shader works in a conventional +Y-up view space.
                const auto& P = m_CurrentView->camera.projection;
                GTAOMainPC pc{};
                pc.projParams  = { P[0][0], std::abs(P[1][1]) };
                pc.nearZ       = m_CurrentView->camera.nearZ;
                pc.farZ        = m_CurrentView->camera.farZ;
                pc.frameIndex  = (u32)Renderer::GetFrameData()->GetFrameIndex();

                vkCmdPushConstants(cmd, m_GTAOMainPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GTAOMainPC), &pc);

                const u32 groupX = (halfW + 7) / 8;
                const u32 groupY = (halfH + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);

                m_System.GetFrameDebugger().CaptureComputeDispatch("GTAOMain",
                    "gtao_main", groupX, groupY, 1);
                m_System.GetFrameDebugger().EndCapturePass();
            });

        return outputHandle;
    }

    RG::ResourceHandle RenderPipeline::AddGTAODenoisePass(
        RG::RenderGraph& rg, RG::ResourceHandle rawAO, RG::ResourceHandle linearDepth)
    {
        struct GTAODenoiseData
        {
            RG::ResourceHandle rawAO;
            RG::ResourceHandle linearDepth;
            RG::ResourceHandle finalAO;
        };

        RG::ResourceHandle outputHandle;

        rg.AddComputePass<GTAODenoiseData>("GTAODenoise",
            [&](GTAODenoiseData& data, RG::RenderPassBuilder& builder)
            {
                data.rawAO       = builder.ReadStorageImage(rawAO);
                data.linearDepth = builder.ReadStorageImage(linearDepth);

                RG::TextureDesc desc;
                desc.name   = "GTAOFinal";
                desc.width  = m_CurrentViewResources->gtaoFinal->GetWidth();
                desc.height = m_CurrentViewResources->gtaoFinal->GetHeight();
                desc.format = RG::TextureFormat::R8_Unorm;

                auto vkFinal = std::static_pointer_cast<VKTexture>(m_CurrentViewResources->gtaoFinal);
                data.finalAO = rg.ImportResource(desc,
                    (void*)vkFinal->GetImage(),
                    (void*)vkFinal->GetImageView(),
                    RG::ResourceState::Undefined);
                data.finalAO = builder.WriteStorageImage(data.finalAO);

                outputHandle = data.finalAO;
            },
            [this](GTAODenoiseData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                m_System.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "GTAODenoise", "GTAOFinal", false,
                    { "gtao_denoise", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                if (!m_GTAODenoisePipeline || m_CurrentViewResources->gtaoDenoiseDescSet == VK_NULL_HANDLE)
                {
                    m_System.GetFrameDebugger().EndCapturePass();
                    return;
                }

                m_GTAODenoisePipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_GTAODenoisePipeline->GetLayout(), 0, 1, &m_CurrentViewResources->gtaoDenoiseDescSet, 0, nullptr);

                const u32 halfW = m_CurrentViewResources->gtaoFinal->GetWidth();
                const u32 halfH = m_CurrentViewResources->gtaoFinal->GetHeight();
                const u32 groupX = (halfW + 7) / 8;
                const u32 groupY = (halfH + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);

                m_System.GetFrameDebugger().CaptureComputeDispatch("GTAODenoise",
                    "gtao_denoise", groupX, groupY, 1);
                m_System.GetFrameDebugger().EndCapturePass();
            });

        return outputHandle;
    }
}
