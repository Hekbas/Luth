#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/FrameDebugger.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace Luth
{
    namespace
    {
        // std430/std140 layout for the depth prefilter push constants. Must match
        // the `PC` block in gtao_depth_prefilter.comp.
        struct GTAOPrefilterPC
        {
            glm::ivec2 halfResSize;     // 0
            glm::vec2  invFullRes;      // 8
            float      nearZ;           // 16
            float      farZ;            // 20
            float      _pad0;           // 24
            float      _pad1;           // 28
        };
        static_assert(sizeof(GTAOPrefilterPC) == 32, "GTAOPrefilterPC layout mismatch");
    }

    RG::ResourceHandle RenderingSystem::AddGTAODepthPrefilterPass(
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
                desc.width  = m_GTAOLinearDepth->GetWidth();
                desc.height = m_GTAOLinearDepth->GetHeight();
                desc.format = RG::TextureFormat::R32_Float;

                auto vkLin = std::static_pointer_cast<VKTexture>(m_GTAOLinearDepth);
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

                m_FrameDebugger.BeginCapturePass("GTAODepthPrefilter", "GTAOLinearDepth", false,
                    { "gtao_depth_prefilter", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                if (!m_GTAOPrefilterPipeline || m_GTAOPrefilterDescSet == VK_NULL_HANDLE)
                {
                    m_FrameDebugger.EndCapturePass();
                    return;
                }

                m_GTAOPrefilterPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_GTAOPrefilterPipeline->GetLayout(), 0, 1, &m_GTAOPrefilterDescSet, 0, nullptr);

                const u32 halfW = m_GTAOLinearDepth->GetWidth();
                const u32 halfH = m_GTAOLinearDepth->GetHeight();
                const u32 fullW = m_SceneDepth->GetWidth();
                const u32 fullH = m_SceneDepth->GetHeight();

                GTAOPrefilterPC pc{};
                pc.halfResSize = { (i32)halfW, (i32)halfH };
                pc.invFullRes  = { 1.0f / float(fullW), 1.0f / float(fullH) };
                pc.nearZ       = m_CameraParams.nearZ;
                pc.farZ        = m_CameraParams.farZ;

                vkCmdPushConstants(cmd, m_GTAOPrefilterPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GTAOPrefilterPC), &pc);

                // 8x8 tiles — one thread per half-res pixel. Round up so the
                // last tile covers the edges; the shader bounds-checks.
                const u32 groupX = (halfW + 7) / 8;
                const u32 groupY = (halfH + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);

                m_FrameDebugger.CaptureComputeDispatch("GTAODepthPrefilter",
                    "gtao_depth_prefilter", groupX, groupY, 1);
                m_FrameDebugger.EndCapturePass();
            });

        return outputHandle;
    }
}
