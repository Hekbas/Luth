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

#include <vma/vk_mem_alloc.h>

namespace Luth
{
    namespace
    {
        // Build a 0-instance TLAS, persistent for the lifetime of RtSubsystem. Seeds Set 0 binding 6
        // before any per-frame TlasBuildPass runs — required from B.3 onward since rt_sun_shadows.rgen
        // statically reads `topLevelAS` at binding 6 and a null handle there violates the descriptor
        // rules even with PARTIALLY_BOUND (validation interprets static use conservatively). The empty
        // TLAS is geometrically valid — vkCmdBuild with instanceCount=0 is legal per spec; rayQuery
        // against it always misses (no instances to intersect), yielding visibility=1.0.
        bool BuildEmptyTlas(VkAccelerationStructureKHR& outAS,
                            VkBuffer& outStorageBuf,
                            VmaAllocation& outStorageAlloc)
        {
            auto& ctx = VulkanContext::Get();
            VkDevice device = ctx.GetDevice();
            const auto& rt  = ctx.GetRtFn();

            VkAccelerationStructureGeometryInstancesDataKHR instData{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
            instData.arrayOfPointers   = VK_FALSE;
            instData.data.deviceAddress = 0;  // No instances, no buffer needed for the empty case.

            VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            geom.geometryType        = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geom.geometry.instances  = instData;
            geom.flags               = 0;

            VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            buildInfo.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            buildInfo.geometryCount = 1;
            buildInfo.pGeometries   = &geom;

            const u32 primitiveCount = 0;
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
            rt.vkGetAccelerationStructureBuildSizesKHR(
                device,
                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                &buildInfo,
                &primitiveCount,
                &sizes);

            // Some drivers report zero sizes for an empty TLAS; clamp to a small min so VMA accepts.
            const VkDeviceSize storageSize = sizes.accelerationStructureSize > 0
                ? sizes.accelerationStructureSize : 256;
            const VkDeviceSize scratchSize = sizes.buildScratchSize > 0
                ? sizes.buildScratchSize : 256;

            VkBufferCreateInfo storageCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            storageCi.size        = storageSize;
            storageCi.usage       = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                                  | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            storageCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            outStorageAlloc = VulkanAllocator::AllocateBuffer(
                storageCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, outStorageBuf);
            if (!outStorageBuf) return false;

            VkBufferCreateInfo scratchCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            scratchCi.size        = scratchSize;
            scratchCi.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                  | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            scratchCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer scratchBuf = VK_NULL_HANDLE;
            VmaAllocation scratchAlloc = nullptr;
            VulkanAllocator::AllocateBuffer(scratchCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, scratchBuf);

            VkBufferDeviceAddressInfo scratchAddrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            scratchAddrInfo.buffer = scratchBuf;
            const VkDeviceAddress scratchBda = vkGetBufferDeviceAddress(device, &scratchAddrInfo);

            VkAccelerationStructureCreateInfoKHR asCi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
            asCi.buffer = outStorageBuf;
            asCi.offset = 0;
            asCi.size   = storageSize;
            asCi.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            rt.vkCreateAccelerationStructureKHR(device, &asCi, nullptr, &outAS);

            buildInfo.dstAccelerationStructure  = outAS;
            buildInfo.scratchData.deviceAddress = scratchBda;

            VkAccelerationStructureBuildRangeInfoKHR range{};
            range.primitiveCount = 0;
            const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

            ctx.ImmediateSubmit([&](VkCommandBuffer cmd) {
                rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRange);
            });

            // Scratch lives only for the build — ImmediateSubmit blocks until the GPU finishes,
            // so an immediate free is safe (no in-flight cmd buffer references it).
            VulkanAllocator::FreeBuffer(scratchBuf, scratchAlloc);
            return true;
        }
    }

    void RtSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;

        // Persistent empty TLAS — backs GetTlas() before the first per-frame TlasBuildPass runs.
        // Kept SEPARATE from m_LastResult so the hash-skip PushDeletion in AddTlasBuildPass never
        // accidentally destroys it when a per-frame TLAS first replaces the m_LastResult slot.
        if (BuildEmptyTlas(m_PersistentEmptyTlas, m_PersistentEmptyTlasBuf, m_PersistentEmptyTlasAlloc))
        {
            LH_CORE_INFO("RtSubsystem: persistent empty TLAS built (frame-0 binding-6 safety)");
        }
        else
        {
            LH_CORE_CRITICAL("RtSubsystem: persistent empty TLAS build failed — Set 0 binding 6 will be null on frame 0");
        }

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
        // Persistent empty TLAS — push to deletion queue so it retires after the last in-flight
        // frame stops referencing it via Set 0 binding 6 (PushDeletion drains N+2 frames out).
        if (m_PersistentEmptyTlas != VK_NULL_HANDLE)
        {
            auto handle = m_PersistentEmptyTlas;
            auto buf    = m_PersistentEmptyTlasBuf;
            auto alloc  = m_PersistentEmptyTlasAlloc;
            VulkanContext::Get().PushDeletion([handle, buf, alloc]() {
                auto& ctx = VulkanContext::Get();
                if (handle != VK_NULL_HANDLE)
                    ctx.GetRtFn().vkDestroyAccelerationStructureKHR(ctx.GetDevice(), handle, nullptr);
                if (buf != VK_NULL_HANDLE) VulkanAllocator::FreeBuffer(buf, alloc);
            });
            m_PersistentEmptyTlas      = VK_NULL_HANDLE;
            m_PersistentEmptyTlasBuf   = VK_NULL_HANDLE;
            m_PersistentEmptyTlasAlloc = nullptr;
        }

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
