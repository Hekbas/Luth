#include "luthpch.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/subsystems/GlobalSubsystem.h"
#include "luth/renderer/subsystems/LightingSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanRayTracingPipeline.h"
#include "luth/renderer/backend/vulkan/RtShaderBindingTable.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/shader/ShaderCompiler.h"
#include "luth/renderer/shader/ShaderLibrary.h"
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
        // before any per-frame TlasBuildPass runs — rt_sun_shadows.comp statically reads `topLevelAS`
        // at binding 6, and a null handle there violates the descriptor
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
            ctx.ApplyConcurrentSharing(storageCi);  // raygen reads on AsyncCompute cross-queue
            outStorageAlloc = VulkanAllocator::AllocateBuffer(
                storageCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, outStorageBuf);
            if (!outStorageBuf) return false;

            VkBufferCreateInfo scratchCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            scratchCi.size        = scratchSize;
            scratchCi.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                  | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            scratchCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer scratchBuf = VK_NULL_HANDLE;
            VmaAllocation scratchAlloc = VulkanAllocator::AllocateBuffer(
                scratchCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, scratchBuf);

            VkBufferDeviceAddressInfo scratchAddrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            scratchAddrInfo.buffer = scratchBuf;
            const VkDeviceAddress scratchBda = vkGetBufferDeviceAddress(device, &scratchAddrInfo);

            VkAccelerationStructureCreateInfoKHR asCi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
            asCi.buffer = outStorageBuf;
            asCi.offset = 0;
            asCi.size   = storageSize;
            asCi.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            rt.vkCreateAccelerationStructureKHR(device, &asCi, nullptr, &outAS);
            VulkanContext::SetDebugName(outAS, "TLAS.seed");

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

        // Pass-local sampler — linear clamp-to-edge for both SceneDepth + SlimNormal reads.
        // Out-of-range UVs (off-screen) clamp to the edge pixel; raygen guards against background
        // (depth >= 1.0) but the sampler edge behavior is the safe default.
        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter    = VK_FILTER_LINEAR;
        samplerInfo.minFilter    = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(VulkanContext::Get().GetDevice(), &samplerInfo, nullptr, &m_ShadowPassSampler);

        // Pass-local descriptor layout (Set 2 in the compute pipeline-layout).
        VkDescriptorSetLayoutBinding shadowBindings[3] = {};
        shadowBindings[0].binding         = 0;  // SceneDepth sampler
        shadowBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBindings[0].descriptorCount = 1;
        shadowBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        shadowBindings[1].binding         = 1;  // SlimNormal sampler
        shadowBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBindings[1].descriptorCount = 1;
        shadowBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        shadowBindings[2].binding         = 2;  // sunShadowMask storage image (write)
        shadowBindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        shadowBindings[2].descriptorCount = 1;
        shadowBindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo shadowLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        shadowLayoutInfo.bindingCount = 3;
        shadowLayoutInfo.pBindings    = shadowBindings;
        vkCreateDescriptorSetLayout(VulkanContext::Get().GetDevice(), &shadowLayoutInfo, nullptr, &m_ShadowPassSetLayout);

        // Load the sun-shadow compute SPV. ShaderLibrary::LoadEngine routes through the standard asset
        // path (with hot-reload watching) so RtSubsystem::OnShaderReloaded receives rt_sun_shadows.slang
        // when it changes on disk.
        if (auto sh = ShaderLibrary::LoadEngine("shaders/rt_sun_shadows.slang"))
            m_ShadowSpv = sh->GetSpirV();
        if (m_ShadowSpv.empty())
        {
            LH_CORE_ERROR("RtSubsystem: failed to load rt_sun_shadows.slang SPIR-V");
        }
        else
        {
            BuildShadowPipeline();
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

        // Sun-shadow compute pipeline retires here (RAII). Caller fence drain in the backend Shutdown
        // catches in-flight cmd buffers referencing the pipeline.
        m_SunShadowsPipeline.reset();
        m_ShadowSpv.clear();

        if (m_ShadowPassSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(VulkanContext::Get().GetDevice(), m_ShadowPassSetLayout, nullptr);
            m_ShadowPassSetLayout = VK_NULL_HANDLE;
        }
        if (m_ShadowPassSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(VulkanContext::Get().GetDevice(), m_ShadowPassSampler, nullptr);
            m_ShadowPassSampler = VK_NULL_HANDLE;
        }

        // Final per-frame TLAS — push to deletion so FlushAllDeletionQueues catches it on shutdown.
        // The hash-skip path inside AddTlasBuildPass only pushes when REPLACING the slot, so a
        // long-stable m_LastResult lives until shutdown without ever being deferred.
        if (m_LastResult.tlas != VK_NULL_HANDLE)
        {
            auto handle  = m_LastResult.tlas;
            auto buf     = m_LastResult.storageBuffer;
            auto alloc   = m_LastResult.storageAlloc;
            auto geom    = m_LastResult.geomTableBuffer;
            auto geomAl  = m_LastResult.geomTableAlloc;
            VulkanContext::Get().PushDeletion([handle, buf, alloc, geom, geomAl]() {
                auto& ctx = VulkanContext::Get();
                if (handle != VK_NULL_HANDLE)
                    ctx.GetRtFn().vkDestroyAccelerationStructureKHR(ctx.GetDevice(), handle, nullptr);
                if (buf != VK_NULL_HANDLE)  VulkanAllocator::FreeBuffer(buf, alloc);
                if (geom != VK_NULL_HANDLE) VulkanAllocator::FreeBuffer(geom, geomAl);
            });
        }
        m_LastResult = {};
        m_LastBuildFrame = ~u64(0);
        m_Pipeline = nullptr;
    }

    void RtSubsystem::BuildShadowPipeline()
    {
        if (m_ShadowSpv.empty()) return;
        if (!m_Pipeline) return;

        // Set 0 = Global (TLAS b6 + UBO b0), Set 1 = Light SSBO (PBR's Set 3 remapped to Set 1; the
        // shader's `set = 1` matches), Set 2 = pass-local (depth + normal + mask), Set 3 = Material SSBO +
        // Set 4 = bindless textures for material_bindings_rt.slang's cutout alpha-test. The geom-table BDA rides a
        // push constant; all other RT-shadow params still come from GlobalUniforms.rtShadowParams.
        std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_ShadowPassSetLayout,
            MaterialSystem::GetDescriptorSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
        };
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkDeviceAddress) };

        m_SunShadowsPipeline = std::make_unique<VKComputePipeline>(
            m_ShadowSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
    }

    bool RtSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        if (name != "rt_sun_shadows.slang") return false;
        m_ShadowSpv = spv;

        // Defer-destroy the old pipeline — in-flight cmd buffers may still reference it.
        if (m_SunShadowsPipeline)
        {
            auto* oldPipe = m_SunShadowsPipeline.release();
            VulkanContext::Get().PushDeletion([oldPipe]() { delete oldPipe; });
        }
        BuildShadowPipeline();
        LH_CORE_INFO("RtSubsystem: sun-shadow pipeline rebuilt after shader reload ({})", name);
        return true;
    }

    void RtSubsystem::WriteShadowPassView(ViewResources& vr, FrameTargets& targets)
    {
        if (m_ShadowPassSetLayout == VK_NULL_HANDLE) return;
        if (!targets.GetSceneDepth() || !targets.GetSlimNormal() || !vr.sunShadowMask) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        const VkImageView depthView  = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth())->GetImageView();
        const VkImageView normalView = std::static_pointer_cast<VKTexture>(targets.GetSlimNormal())->GetImageView();
        const VkImageView maskView   = std::static_pointer_cast<VKTexture>(vr.sunShadowMask)->GetImageView();

        for (u32 slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
        {
            VkDescriptorSet set = vr.rtShadowPassDescSet[slot];
            if (set == VK_NULL_HANDLE) continue;

            VkDescriptorImageInfo depthInfo{};
            depthInfo.sampler     = m_ShadowPassSampler;
            depthInfo.imageView   = depthView;
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo normalInfo{};
            normalInfo.sampler     = m_ShadowPassSampler;
            normalInfo.imageView   = normalView;
            normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo maskInfo{};
            maskInfo.sampler     = VK_NULL_HANDLE;
            maskInfo.imageView   = maskView;
            maskInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[3]{};
            writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[0].dstSet          = set;
            writes[0].dstBinding      = 0;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo      = &depthInfo;

            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[1].dstSet          = set;
            writes[1].dstBinding      = 1;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo      = &normalInfo;

            writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[2].dstSet          = set;
            writes[2].dstBinding      = 2;
            writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[2].descriptorCount = 1;
            writes[2].pImageInfo      = &maskInfo;

            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }
    }

    RG::ResourceHandle RtSubsystem::AddRtSunShadowsPass(RG::RenderGraph& rg,
                                                        RG::ResourceHandle sceneDepth,
                                                        RG::ResourceHandle slimNormal)
    {
        // Pre-flight: pipeline must exist (shaders loaded). Mask must exist (view allocated).
        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!m_SunShadowsPipeline || !preflightVr || !preflightVr->sunShadowMask)
            return {};

        struct RtSunShadowsData {
            RG::ResourceHandle mask;
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
        };
        RG::ResourceHandle outputHandle{};
        rg.AddComputePass<RtSunShadowsData>(
            "RtSunShadows",
            RG::QueueFamily::AsyncCompute,
            [&, this](RtSunShadowsData& data, RG::RenderPassBuilder& builder) {
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                auto maskTex = std::static_pointer_cast<VKTexture>(vr->sunShadowMask);

                RG::TextureDesc desc;
                desc.name   = "SunShadowMask";
                desc.width  = maskTex->GetWidth();
                desc.height = maskTex->GetHeight();
                desc.format = RG::TextureFormat::R8_Unorm;

                data.mask = rg.ImportResource(desc,
                    (void*)maskTex->GetImage(), (void*)maskTex->GetImageView(),
                    RG::ResourceState::Undefined);
                data.mask = builder.WriteStorageImage(data.mask);
                // SceneDepth + slimNormal are descriptor-bound to the pass-local set (set 2 b0, b1)
                // with imageLayout = SHADER_READ_ONLY_OPTIMAL. ReadStorageImage maps to
                // ResourceState::ComputeRead (COMPUTE_SHADER | RAY_TRACING_SHADER stages,
                // SHADER_READ_ONLY_OPTIMAL layout) — compatible with AsyncCompute, unlike
                // plain Read which uses FRAGMENT_SHADER_BIT and would error on this queue.
                // Despite the name, the descriptor type is COMBINED_IMAGE_SAMPLER, not storage;
                // the "StorageImage" suffix here is about queue affinity (same convention used
                // by VolumetricSubsystem::AddInjectScatterPass for the cascade shadow reads).
                if (sceneDepth.IsValid()) data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid()) data.normal = builder.ReadStorageImage(slimNormal);
                outputHandle = data.mask;
            },
            [this](RtSunShadowsData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr) return;

                const u64 frameAbs = Renderer::GetFrameData()->GetRenderFrameIndex();
                const u32 slot     = static_cast<u32>(frameAbs % MAX_FRAMES_IN_FLIGHT);

                // AS-build → AS-read barrier. TlasBuildPass (same AsyncCompute primary) emits
                // BLAS→TLAS-build barriers internally but not the final AS-write → read hop. Without
                // this, the dispatch may sample a TLAS that's still being built. dstStage = COMPUTE_SHADER
                // (NOT RAY_TRACING — rayQuery runs in compute; a RAY_TRACING dst here is a TDR trap).
                VkMemoryBarrier2 asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                asBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                asBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                VkDependencyInfo asDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                asDep.memoryBarrierCount = 1;
                asDep.pMemoryBarriers    = &asBarrier;
                vkCmdPipelineBarrier2(cmd, &asDep);

                m_SunShadowsPipeline->Bind(cmd);

                // Sets: 0 = global (TLAS + UBO), 1 = light SSBO (PBR's Set 3 remapped to Set 1), 2 = per-view
                // pass-local (depth + normal + mask), 3 = Material SSBO, 4 = bindless (cutout alpha-test).
                VkDescriptorSet sets[5] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->rtShadowPassDescSet[slot],
                    MaterialSystem::GetDescriptorSet(slot),
                    VulkanContext::Get().GetBindlessSet().GetSet(),
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        m_SunShadowsPipeline->GetLayout(),
                                        /*firstSet*/ 0, 5, sets, 0, nullptr);

                const VkDeviceAddress geomTableBDA = GetGeometryTableBDA();
                vkCmdPushConstants(cmd, m_SunShadowsPipeline->GetLayout(),
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(geomTableBDA), &geomTableBDA);

                const u32 groupX = (vr->width  + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        return outputHandle;
    }

    void RtSubsystem::AddTlasBuildPass(RG::RenderGraph& rg)
    {
        struct TlasBuildData {};
        rg.AddComputePass<TlasBuildData>(
            "TlasBuild",
            RG::QueueFamily::AsyncCompute,
            [&](TlasBuildData&, RG::RenderPassBuilder& builder) {
                // No RG-tracked resources — all per-frame allocations live outside the RG
                // (per-frame VMA + PushDeletion / tagged-heap large-one-shot scratch). The
                // pass's actual output (m_LastResult.tlas → Set 0 binding 6 via UpdateUBO)
                // is an engine-side side effect; SetHasSideEffect keeps the pass alive
                // through CullDeadPasses (which otherwise drops passes with no Write/Read).
                builder.SetHasSideEffect();
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

                // 2. Compute-write barrier (one global UAV barrier per NVIDIA RTX guidance, covering
                // every skinned mesh's deformed-VB at once). The skinning SHADER_WRITE must reach two
                // dst readers: the AS-build vertex-input read of positions, AND the rayQuery-in-compute
                // trace reads of normal/tangent/UV — the skinned geometry-table vertexBDA points at
                // this buffer, so GatherHitGeometry/AlphaTestUV deref it at COMPUTE_SHADER. Both later
                // passes share this AsyncCompute primary, so the single barrier suffices.
                VkMemoryBarrier2 mem{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mem.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mem.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                mem.dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                                  | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mem.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;  // AS-build vertex read + trace BDA read
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

                // 5. TLAS build with hash-skip. When skip fires, prior TLAS + storage + geom table
                // stay alive — we only PushDeletion when an actual rebuild replaces them. The geom
                // table shares the TLAS lifetime exactly (same retire schedule).
                TlasBuildResult fresh = TlasBuilder::BuildTlas(
                    cmd, snapshot.meshes, static_cast<u32>(frameAbs), m_LastResult,
                    m_Pipeline->GetMaterialSlotMap());
                if (!fresh.reused && m_LastResult.tlas != VK_NULL_HANDLE)
                {
                    auto old       = m_LastResult.tlas;
                    auto oldBuf    = m_LastResult.storageBuffer;
                    auto oldAlloc  = m_LastResult.storageAlloc;
                    auto oldGeom   = m_LastResult.geomTableBuffer;
                    auto oldGeomAl = m_LastResult.geomTableAlloc;
                    VulkanContext::Get().PushDeletion([old, oldBuf, oldAlloc, oldGeom, oldGeomAl]() {
                        auto& ctx2 = VulkanContext::Get();
                        if (old != VK_NULL_HANDLE)
                            ctx2.GetRtFn().vkDestroyAccelerationStructureKHR(ctx2.GetDevice(), old, nullptr);
                        if (oldBuf != VK_NULL_HANDLE)  VulkanAllocator::FreeBuffer(oldBuf, oldAlloc);
                        if (oldGeom != VK_NULL_HANDLE) VulkanAllocator::FreeBuffer(oldGeom, oldGeomAl);
                    });
                }
                m_LastResult = fresh;
            });
    }
}
