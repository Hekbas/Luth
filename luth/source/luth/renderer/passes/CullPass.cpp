#include "luthpch.h"
#include "CullPass.h"
#include "luth/renderer/FrameDebugger.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace Luth
{
    struct CullPassData
    {
        RG::BufferHandle objectBuffer;
        RG::BufferHandle indirectBuffer;
    };

    struct CullPushConstants
    {
        glm::vec4 frustumPlanes[6]; // 96B
        u32       objectCount;      // 4B
    };

    void AddCullComputePass(
        RG::RenderGraph&                rg,
        RG::BufferHandle                objectBuffer,
        RG::BufferHandle                indirectBuffer,
        VKComputePipeline*              pipeline,
        VkDescriptorSet                 descSet,
        const std::array<glm::vec4, 6>& frustumPlanes,
        u32                             objectCount,
        FrameDebugger*                  debugger)
    {
        if (!pipeline || objectCount == 0) return;

        rg.AddComputePass<CullPassData>("FrustumCull",
            [=](CullPassData& data, RG::RenderPassBuilder& builder)
            {
                data.objectBuffer   = builder.ReadBuffer(objectBuffer);
                data.indirectBuffer = builder.WriteBuffer(indirectBuffer);
            },
            [pipeline, descSet, frustumPlanes, objectCount, debugger](CullPassData&, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                if (debugger)
                {
                    debugger->BeginCapturePass("FrustumCull", "", false,
                        { "gpu_cull", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });
                }

                pipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline->GetLayout(), 0, 1, &descSet, 0, nullptr);

                CullPushConstants pc{};
                for (int i = 0; i < 6; ++i)
                    pc.frustumPlanes[i] = frustumPlanes[i];
                pc.objectCount = objectCount;
                vkCmdPushConstants(cmd, pipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPushConstants), &pc);

                u32 groupCountX = (objectCount + 255) / 256;
                vkCmdDispatch(cmd, groupCountX, 1, 1);

                if (debugger)
                {
                    debugger->CaptureComputeDispatch("FrustumCull", "gpu_cull", groupCountX, 1, 1);
                    debugger->EndCapturePass();
                }
            });
    }
}
