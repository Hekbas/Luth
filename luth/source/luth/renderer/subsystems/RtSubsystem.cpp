#include "luthpch.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanRayTracingPipeline.h"
#include "luth/renderer/backend/vulkan/RtShaderBindingTable.h"
#include "luth/renderer/shader/ShaderCompiler.h"
#include "luth/resources/FileSystem.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/core/BuildConfig.h"

namespace Luth
{
    void RtSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;

#if LUTH_ENABLE_VALIDATION
        // Smoke test: prove the full B.1 chain (extension load + fp loader + properties query
        // + pipeline create + SBT alignment + traceRays) works end-to-end. Without this,
        // B.2 lands on unproven infra and any breakage masquerades as a BLAS bug.
        // No descriptors / no push constants / no actual rays traced — pure plumbing check.
        auto raygenSpv = ShaderCompiler::Compile(FileSystem::EngineAssetsPath("shaders/rt_smoke.rgen"));
        if (raygenSpv.empty())
        {
            LH_CORE_CRITICAL("RtSubsystem: smoke shader compile failed — check ShaderCompiler RT mappings");
            return;
        }

        RayTracingStages stages;
        stages.stages.push_back({ VK_SHADER_STAGE_RAYGEN_BIT_KHR, raygenSpv, "main" });
        RayTracingShaderGroup grp{};
        grp.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        grp.generalShader = 0; // index into stages[]
        stages.groups.push_back(grp);

        VKRayTracingPipeline pipe(stages, /*layouts*/ {}, /*pushConstants*/ {}, /*maxRecursionDepth*/ 1);
        if (pipe.GetPipeline() == VK_NULL_HANDLE)
        {
            LH_CORE_CRITICAL("RtSubsystem: smoke pipeline create failed");
            return;
        }

        RtSbtCounts counts; counts.raygenCount = 1;
        RtShaderBindingTable sbt(pipe, counts);
        if (sbt.GetBuffer() == VK_NULL_HANDLE)
        {
            LH_CORE_CRITICAL("RtSubsystem: smoke SBT build failed");
            return;
        }

        auto& ctx = VulkanContext::Get();
        const VkStridedDeviceAddressRegionKHR empty{};
        ctx.ImmediateSubmit([&](VkCommandBuffer cmd) {
            pipe.Bind(cmd);
            ctx.GetRtFn().vkCmdTraceRaysKHR(
                cmd,
                &sbt.GetRaygenRegion(),
                &empty, // miss
                &empty, // hit
                &empty, // callable
                1, 1, 1);
        });

        LH_CORE_INFO("RtSubsystem: smoke-test traceRays OK");
        // pipe + sbt destruct here; SBT defers buffer free via PushDeletion (drains N+2 frames)
#else
        LH_CORE_INFO("RtSubsystem: idle (Release build — smoke test disabled)");
#endif
    }

    void RtSubsystem::Shutdown()
    {
        m_Pipeline = nullptr;
    }
}
