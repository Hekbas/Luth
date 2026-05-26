#include "luthpch.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanRayTracingPipeline.h"
#include "luth/renderer/backend/vulkan/RtShaderBindingTable.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/shader/ShaderCompiler.h"
#include "luth/renderer/subsystems/SkinningSubsystem.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/resources/FileSystem.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/core/BuildConfig.h"
#include "luth/core/FrameData.h"
#include "luth/core/RenderSnapshot.h"

namespace Luth
{
    void RtSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;

#if LUTH_ENABLE_VALIDATION
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
        grp.generalShader = 0;
        stages.groups.push_back(grp);

        VKRayTracingPipeline pipe(stages, {}, {}, 1);
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
                &empty, &empty, &empty,
                1, 1, 1);
        });

        LH_CORE_INFO("RtSubsystem: smoke-test traceRays OK");
#else
        LH_CORE_INFO("RtSubsystem: idle (Release build — smoke test disabled)");
#endif
    }

    void RtSubsystem::Shutdown()
    {
        // Final-frame TLAS storage retires via PushDeletion at the next AcquireImage drain; for
        // the very last frame, FlushAllDeletionQueues in the backend Shutdown catches them.
        m_LastResult = {};
        m_LastBuildFrame = ~u64(0);
        m_Pipeline = nullptr;
    }

    void RtSubsystem::AddTlasBuildPass(RG::RenderGraph& rg)
    {
        struct TlasBuildData {};
        rg.AddComputePass<TlasBuildData>(
            "TlasBuild",
            RG::QueueFamily::AsyncCompute,
            [&](TlasBuildData&, RG::RenderPassBuilder&) {
                // No RG-tracked resources for B.2 — all per-frame allocations live outside the RG
                // (per-frame VMA + PushDeletion / tagged-heap large-one-shot scratch). The cross-pass
                // barrier into the future B.3 RT consumer composes via the new AccelerationStructure*
                // ResourceState entries once that consumer declares a Read.
            },
            [this](TlasBuildData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                const u64 frameAbs = Renderer::GetFrameData()->GetRenderFrameIndex();

                // Multi-view guard — Execute runs per view; TLAS is scene-global. Second view
                // returns the same m_LastResult.tlas without re-recording any GPU commands.
                if (m_LastBuildFrame == frameAbs) return;
                m_LastBuildFrame = frameAbs;

                auto* rs = SystemRegistry::GetSystem<RenderingSystem>();
                if (!rs) return;
                const RenderSnapshot& snapshot = rs->GetActiveSnapshot();

                // 1. Per-skinned-mesh compute-skin into deformed-VBs.
                m_Pipeline->GetSkinning().DispatchAllSkinned(cmd, snapshot);

                // 2. Compute-write → AS-build-read global barrier per NVIDIA RTX guidance (one
                // global UAV barrier before AS work, not per-resource). Covers every skinned
                // mesh's deformed-VB at once.
                VkMemoryBarrier2 mem{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mem.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mem.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                mem.dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                mem.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers    = &mem;
                vkCmdPipelineBarrier2(cmd, &dep);

                // 3. Batched skinned-BLAS refits — one vkCmdBuildAccelerationStructuresKHR call
                // with N infos sharing one tagged scratch (per-mesh sub-regions, no overlap).
                TlasBuilder::RefitSkinnedBLASes(cmd, snapshot.meshes, static_cast<u32>(frameAbs));

                // 4. Refit-write → TLAS-build-read barrier. Same shape as above; the TLAS build
                // reads the freshly-refitted BLAS device addresses through the instance buffer.
                VkMemoryBarrier2 mem2{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mem2.srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                mem2.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                mem2.dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                mem2.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                VkDependencyInfo dep2{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep2.memoryBarrierCount = 1;
                dep2.pMemoryBarriers    = &mem2;
                vkCmdPipelineBarrier2(cmd, &dep2);

                // 5. TLAS build with hash-skip. When skip fires, prior TLAS + storage stay alive
                // — we only PushDeletion when an actual rebuild replaces them.
                TlasBuildResult fresh = TlasBuilder::BuildTlas(
                    cmd, snapshot.meshes, static_cast<u32>(frameAbs), m_LastResult);
                if (!fresh.reused && m_LastResult.tlas != VK_NULL_HANDLE)
                {
                    auto old      = m_LastResult.tlas;
                    auto oldBuf   = m_LastResult.storageBuffer;
                    auto oldAlloc = m_LastResult.storageAlloc;
                    VulkanContext::Get().PushDeletion([old, oldBuf, oldAlloc]() {
                        auto& ctx2 = VulkanContext::Get();
                        if (old != VK_NULL_HANDLE)
                            ctx2.GetRtFn().vkDestroyAccelerationStructureKHR(ctx2.GetDevice(), old, nullptr);
                        if (oldBuf != VK_NULL_HANDLE) VulkanAllocator::FreeBuffer(oldBuf, oldAlloc);
                    });
                }
                m_LastResult = fresh;
            });
    }
}
