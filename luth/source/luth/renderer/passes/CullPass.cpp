#include "luthpch.h"
#include "CullPass.h"
#include "luth/renderer/FrameDebugger.h"

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
        Vec4 frustumPlanes[6]; // 96B
        u32  objectCount;      // 4B
        u32  destOffset;       // 4B — index offset into commands[] (active slice's region)
        u32  srcOffset;        // 4B — index offset into objects[] (active slice base)
    };

    void AddCullComputePass(
        RG::RenderGraph&                rg,
        RG::BufferHandle                objectBuffer,
        RG::BufferHandle                indirectBuffer,
        VKComputePipeline*              pipeline,
        VkDescriptorSet                 descSet,
        const std::array<Vec4, 6>& frustumPlanes,
        u32                             objectCount,
        u32                             destOffset,
        u32                             srcOffset,
        const char*                     passName,
        FrameDebugger*                  debugger)
    {
        if (!pipeline || objectCount == 0) return;

        std::string name = passName ? passName : "FrustumCull";

        rg.AddComputePass<CullPassData>(name,
            [=](CullPassData& data, RG::RenderPassBuilder& builder)
            {
                data.objectBuffer   = builder.ReadBuffer(objectBuffer);
                data.indirectBuffer = builder.WriteBuffer(indirectBuffer);
            },
            [pipeline, descSet, frustumPlanes, objectCount, destOffset, srcOffset, name, debugger](CullPassData&, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                if (debugger)
                {
                    debugger->BeginCapturePass(ctx.passIndex, name, "", false,
                        { "gpu_cull", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });
                }

                pipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline->GetLayout(), 0, 1, &descSet, 0, nullptr);

                CullPushConstants pc{};
                for (int i = 0; i < 6; ++i)
                    pc.frustumPlanes[i] = frustumPlanes[i];
                pc.objectCount = objectCount;
                pc.destOffset  = destOffset;
                pc.srcOffset   = srcOffset;
                vkCmdPushConstants(cmd, pipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPushConstants), &pc);

                u32 groupCountX = (objectCount + 255) / 256;
                vkCmdDispatch(cmd, groupCountX, 1, 1);

                if (debugger)
                {
                    debugger->CaptureComputeDispatch(name, "gpu_cull", groupCountX, 1, 1);
                    debugger->EndCapturePass();
                }
            });
    }
}
