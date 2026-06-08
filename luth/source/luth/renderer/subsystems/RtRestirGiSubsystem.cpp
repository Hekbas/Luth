#include "luthpch.h"
#include "luth/renderer/subsystems/RtRestirGiSubsystem.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/settings/RestirGiSettings.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/core/FrameData.h"
#include "luth/core/types/LuthMath.h"

namespace Luth
{
    namespace {
        // Sizes the reservoir allocation only — the GPU layout lives in restir_gi_common.glsl's
        // GIReservoir struct; any field change must update both. see arch/rendering-pipeline.md
        struct GPUGIReservoir {
            f32 samplePosX, samplePosY, samplePosZ, W;
            f32 radX, radY, radZ, wSum;
            f32 sampleNormalOctX, sampleNormalOctY; u32 M, age;
            f32 visPosX, visPosY, visPosZ; u32 visNormalPacked;
        };
        static_assert(sizeof(GPUGIReservoir) == 64, "GPUGIReservoir must match restir_gi_common.glsl GIReservoir (64 B)");

        struct GiPC {
            Mat4 invViewProj;
            u32  frameSeed;
            f32  secondaryAlbedo;
            f32  maxIndirect;
            u32  pad1;
        };
        static_assert(sizeof(GiPC) == 80, "GiPC must match restir_gi_initial/shade.comp push_constant");

        // Temporal-pass push constants — same 80 B footprint as GiPC (one shared pcRange), different
        // fields. mCap + maxReservoirAge bit-packed so invViewProj + 4 scalars fit 80 B.
        struct GiTemporalPC {
            Mat4 invViewProj;
            u32  mCapMaxAge;        // mCap (low 16) | maxReservoirAge (high 16)
            u32  frameSeed;
            f32  depthThreshold;
            f32  normalThreshold;
        };
        static_assert(sizeof(GiTemporalPC) == 80, "GiTemporalPC must match restir_gi_temporal.comp push_constant");

        // Spatial-pass push constants — 80 B (shared pcRange). neighbours+radius bit-packed.
        struct GiSpatialPC {
            Mat4 invViewProj;
            u32  neighboursRadius;   // neighbours (low 16) | radius (high 16)
            u32  frameSeed;
            f32  depthThreshold;
            f32  normalThreshold;
        };
        static_assert(sizeof(GiSpatialPC) == 80, "GiSpatialPC must match restir_gi_spatial.comp push_constant");
    }

    bool RtRestirGiSubsystem::IsEnabled() const
    {
        return m_Pipeline && m_Pipeline->GetSystem().GetRestirGiSettings().enabled;
    }

    void RtRestirGiSubsystem::SetEnabled(bool e)
    {
        if (m_Pipeline) m_Pipeline->GetSystem().GetRestirGiSettings().enabled = e;
    }

    void RtRestirGiSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear clamp-to-edge — same shape as the DI pass sampler for SceneDepth/SlimNormal.
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_Sampler);

        // Set 2 (pass-local) — identical 7-binding shape to the DI subsystem so S1/S2 drop in without a
        // layout change. b0 depth sampler, b1 slimNormal sampler, b2 reservoir CURR (r/w SSBO), b3 GI
        // storage image, b4 reservoir PREV (read SSBO), b5 motion sampler, b6 spatial-output reservoir.
        // S0 shaders only use b0/b1/b2/b3. b2/b4 swap each frame.
        VkDescriptorSetLayoutBinding bindings[7]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding         = 2;
        bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[3].binding         = 3;
        bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[4].binding         = 4;
        bindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[5].binding         = 5;
        bindings[5].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[5].descriptorCount = 1;
        bindings[5].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[6].binding         = 6;
        bindings[6].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[6].descriptorCount = 1;
        bindings[6].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        // b2/b4 (curr/prev reservoirs) are rewritten per-frame by WriteReservoirBindings while other
        // cycled slots may still be pending on the GPU — UAB satisfies VUID-vkUpdateDescriptorSets-
        // None-03047. b0/b1/b3/b5/b6 are stable per-view, written once at WriteView time.
        VkDescriptorBindingFlags bindingFlags[7] = {
            0,                                            // b0 depth sampler
            0,                                            // b1 normal sampler
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,  // b2 reservoir curr
            0,                                            // b3 GI storage image
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,  // b4 reservoir prev
            0,                                            // b5 motion sampler
            0,                                            // b6 spatial output reservoir
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsCI.bindingCount  = 7;
        bindingFlagsCI.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutCI.pNext        = &bindingFlagsCI;
        layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutCI.bindingCount = 7;
        layoutCI.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_SetLayout);

        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_gi_initial.comp"))
            m_InitialSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_gi_temporal.comp"))
            m_TemporalSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_gi_spatial.comp"))
            m_SpatialSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_gi_shade.comp"))
            m_ShadeSpv = sh->GetSpirV();
        if (m_InitialSpv.empty() || m_TemporalSpv.empty() || m_SpatialSpv.empty() || m_ShadeSpv.empty())
        {
            LH_CORE_ERROR("RtRestirGiSubsystem: failed to load restir_gi_initial/temporal/spatial/shade.comp SPIR-V");
            return;
        }

        // Sets: 0 = global (UBO b0 + TLAS b6), 1 = light SSBO, 2 = pass-local. Matches all 3 shaders.
        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
        };
        // GiPC and GiTemporalPC are both 80 B — one shared range covers all 3 pipelines.
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiPC) };

        m_InitialPipeline = std::make_unique<VKComputePipeline>(
            m_InitialSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        m_TemporalPipeline = std::make_unique<VKComputePipeline>(
            m_TemporalSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        m_SpatialPipeline = std::make_unique<VKComputePipeline>(
            m_SpatialSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        m_ShadePipeline = std::make_unique<VKComputePipeline>(
            m_ShadeSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
    }

    void RtRestirGiSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_InitialPipeline.reset();
        m_TemporalPipeline.reset();
        m_SpatialPipeline.reset();
        m_ShadePipeline.reset();
        if (m_Sampler)   vkDestroySampler(device, m_Sampler, nullptr);
        if (m_SetLayout) vkDestroyDescriptorSetLayout(device, m_SetLayout, nullptr);
        m_Sampler   = VK_NULL_HANDLE;
        m_SetLayout = VK_NULL_HANDLE;
        m_InitialSpv.clear();
        m_TemporalSpv.clear();
        m_SpatialSpv.clear();
        m_ShadeSpv.clear();
        m_Pipeline = nullptr;
    }

    bool RtRestirGiSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        if (m_SetLayout == VK_NULL_HANDLE || !m_Pipeline) return false;

        const bool isInitial  = (name == "restir_gi_initial.comp");
        const bool isTemporal = (name == "restir_gi_temporal.comp");
        const bool isSpatial  = (name == "restir_gi_spatial.comp");
        const bool isShade    = (name == "restir_gi_shade.comp");
        if (!isInitial && !isTemporal && !isSpatial && !isShade) return false;

        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
        };
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiPC) };

        auto deferComp = [](std::unique_ptr<VKComputePipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if (isInitial)
        {
            m_InitialSpv = spv;
            deferComp(m_InitialPipeline);
            m_InitialPipeline = std::make_unique<VKComputePipeline>(
                m_InitialSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        }
        else if (isTemporal)
        {
            m_TemporalSpv = spv;
            deferComp(m_TemporalPipeline);
            m_TemporalPipeline = std::make_unique<VKComputePipeline>(
                m_TemporalSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        }
        else if (isSpatial)
        {
            m_SpatialSpv = spv;
            deferComp(m_SpatialPipeline);
            m_SpatialPipeline = std::make_unique<VKComputePipeline>(
                m_SpatialSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        }
        else
        {
            m_ShadeSpv = spv;
            deferComp(m_ShadePipeline);
            m_ShadePipeline = std::make_unique<VKComputePipeline>(
                m_ShadeSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        }
        return true;
    }

    void RtRestirGiSubsystem::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.restirGiDescSet[0] == VK_NULL_HANDLE) return;
        if (!targets.GetSceneDepth() || !targets.GetSlimNormal() || !targets.GetSlimMotion() || !vr.restirGiDI) return;
        if (!vr.restirGiSpatial.buffer) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        const VkImageView depthView  = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth())->GetImageView();
        const VkImageView normalView = std::static_pointer_cast<VKTexture>(targets.GetSlimNormal())->GetImageView();
        const VkImageView motionView = std::static_pointer_cast<VKTexture>(targets.GetSlimMotion())->GetImageView();
        const VkImageView giView     = std::static_pointer_cast<VKTexture>(vr.restirGiDI)->GetImageView();

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler     = m_Sampler;
        depthInfo.imageView   = depthView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo normalInfo{};
        normalInfo.sampler     = m_Sampler;
        normalInfo.imageView   = normalView;
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo motionInfo{};
        motionInfo.sampler     = m_Sampler;
        motionInfo.imageView   = motionView;
        motionInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo giInfo{};
        giInfo.imageView   = giView;
        giInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // b6 spatial-output reservoir — single per-view buffer, stable like b0/b1/b3/b5 (S0 ignores it).
        VkDescriptorBufferInfo spatialInfo{
            vr.restirGiSpatial.buffer, vr.restirGiSpatial.offset, vr.restirGiSpatial.size };

        // Stable per-view bindings only: b0 depth, b1 normal, b3 GI, b5 motion, b6 spatial out. b2/b4
        // (reservoirs) swap each frame — WriteReservoirBindings owns them.
        VkWriteDescriptorSet writes[5 * MAX_FRAMES_IN_FLIGHT]{};
        u32 n = 0;
        for (u32 slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
        {
            VkDescriptorSet set = vr.restirGiDescSet[slot];
            if (set == VK_NULL_HANDLE) continue;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 0;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].descriptorCount = 1;
            writes[n].pImageInfo      = &depthInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 1;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].descriptorCount = 1;
            writes[n].pImageInfo      = &normalInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 3;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[n].descriptorCount = 1;
            writes[n].pImageInfo      = &giInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 5;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].descriptorCount = 1;
            writes[n].pImageInfo      = &motionInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 6;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[n].descriptorCount = 1;
            writes[n].pBufferInfo     = &spatialInfo;
            ++n;
        }
        vkUpdateDescriptorSets(device, n, writes, 0, nullptr);
    }

    void RtRestirGiSubsystem::WriteReservoirBindings(ViewResources& vr)
    {
        if (!vr.restirGiReservoir[0].buffer || !vr.restirGiReservoir[1].buffer) return;

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
        if (vr.restirGiDescSet[slot] == VK_NULL_HANDLE) return;

        // Parity picks curr; prev is the other. Must match AddPasses' selection exactly.
        const u32 currIdx = (frameAbs & 1u);
        const u32 prevIdx = currIdx ^ 1u;

        VkDescriptorBufferInfo currInfo{
            vr.restirGiReservoir[currIdx].buffer, vr.restirGiReservoir[currIdx].offset, vr.restirGiReservoir[currIdx].size };
        VkDescriptorBufferInfo prevInfo{
            vr.restirGiReservoir[prevIdx].buffer, vr.restirGiReservoir[prevIdx].offset, vr.restirGiReservoir[prevIdx].size };

        VkWriteDescriptorSet writes[2]{};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr.restirGiDescSet[slot];
        writes[0].dstBinding      = 2;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &currInfo;
        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = vr.restirGiDescSet[slot];
        writes[1].dstBinding      = 4;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo     = &prevInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 2, writes, 0, nullptr);
    }

    RG::ResourceHandle RtRestirGiSubsystem::AddPasses(RG::RenderGraph& rg,
                                                      RG::ResourceHandle sceneDepth,
                                                      RG::ResourceHandle slimNormal,
                                                      RG::ResourceHandle slimMotion)
    {
        const RestirGiSettings& settings = m_Pipeline->GetSystem().GetRestirGiSettings();
        if (!settings.enabled || !m_InitialPipeline || !m_TemporalPipeline || !m_SpatialPipeline || !m_ShadePipeline) return {};

        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || !preflightVr->restirGiDI
            || !preflightVr->restirGiReservoir[0].buffer || !preflightVr->restirGiReservoir[1].buffer) return {};
        if (m_Pipeline->GetRt().GetTlas() == VK_NULL_HANDLE) return {};

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        // Bind the cycled slot's b2/b4 to this frame's curr/prev reservoirs (parity swap), before the
        // passes record, so the dispatches see the right ping-pong halves.
        WriteReservoirBindings(*preflightVr);

        // Parity picks curr; prev is last frame's curr (read-only by temporal). Must match
        // WriteReservoirBindings.
        const u32 currIdx = (frameAbs & 1u);
        const u32 prevIdx = currIdx ^ 1u;
        const Memory::GPUSubRegion currRes = preflightVr->restirGiReservoir[currIdx];
        const Memory::GPUSubRegion prevRes = preflightVr->restirGiReservoir[prevIdx];

        // Build invViewProj + frameSeed once; initial/shade share GiPC, temporal uses GiTemporalPC
        // (same 80 B footprint / shared pcRange, different fields).
        const Mat4 invVP = Math::Inverse(m_Pipeline->GetGlobal().GetCachedViewProj());
        GiPC pc{};
        pc.invViewProj     = invVP;
        pc.frameSeed       = frameAbs;
        pc.secondaryAlbedo = settings.secondaryAlbedo;
        pc.maxIndirect     = settings.maxIndirect;

        GiTemporalPC tpc{};
        tpc.invViewProj     = invVP;
        tpc.mCapMaxAge      = (settings.temporalMCap & 0xFFFFu) | ((settings.maxReservoirAge & 0xFFFFu) << 16);
        tpc.frameSeed       = frameAbs;
        tpc.depthThreshold  = settings.temporalDepthThreshold;
        tpc.normalThreshold = settings.temporalNormalThreshold;

        GiSpatialPC spc{};
        spc.invViewProj      = invVP;
        spc.neighboursRadius = (settings.spatialNeighbours & 0xFFFFu) | ((settings.spatialRadius & 0xFFFFu) << 16);
        spc.frameSeed        = frameAbs;
        spc.depthThreshold   = settings.spatialDepthThreshold;
        spc.normalThreshold  = settings.spatialNormalThreshold;

        // Initial pass — cosine-sampled 1-bounce path + single-light NEE, writes the CURR reservoir at
        // b2. The curr buffer is imported ONCE here; its handle threads into shade's ReadBuffer so the
        // RG chains the initial→shade RAW barrier.
        struct GiInitialData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::BufferHandle   reservoir;
        };
        RG::BufferHandle reservoirHandle{};
        rg.AddComputePass<GiInitialData>(
            "GiInitial",
            RG::QueueFamily::AsyncCompute,
            [&, this](GiInitialData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid()) data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid()) data.normal = builder.ReadStorageImage(slimNormal);

                RG::BufferDesc bd{ "RestirGiReservoirCurr", currRes.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
                data.reservoir  = rg.ImportBuffer(bd, (void*)currRes.buffer, RG::ResourceState::Undefined);
                data.reservoir  = builder.WriteBuffer(data.reservoir);
                reservoirHandle = data.reservoir;
            },
            [this, pc](GiInitialData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirGiDescSet[0] == VK_NULL_HANDLE) return;

                // AS-build → AS-read barrier. dstStageMask is COMPUTE_SHADER (NOT RAY_TRACING) —
                // rayQuery executes in the compute stage; a RAY_TRACING dst here is a TDR trap.
                VkMemoryBarrier2 asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                asBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                asBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                VkDependencyInfo asDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                asDep.memoryBarrierCount = 1;
                asDep.pMemoryBarriers    = &asBarrier;
                vkCmdPipelineBarrier2(cmd, &asDep);

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_InitialPipeline->Bind(cmd);
                VkDescriptorSet sets[3] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirGiDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_InitialPipeline->GetLayout(), 0, 3, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_InitialPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiPC), &pc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Temporal pass — reprojects via motion + merges last frame's PREV reservoir into the CURR
        // candidate in-place, reweighted by the reconnection Jacobian. PREV is a SEPARATE read-only
        // ImportBuffer (last frame's curr, no within-frame producer — cross-frame like taaHistory).
        // CURR threads through reservoirHandle (read+write) so the RG inserts the initial→temporal RAW
        // barrier. No AS barrier — temporal traces no rays.
        struct GiTemporalData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::ResourceHandle motion;
            RG::BufferHandle   reservoirCurr;
            RG::BufferHandle   reservoirPrev;
        };
        rg.AddComputePass<GiTemporalData>(
            "GiTemporal",
            RG::QueueFamily::AsyncCompute,
            [&, this](GiTemporalData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid()) data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid()) data.normal = builder.ReadStorageImage(slimNormal);
                if (slimMotion.IsValid()) data.motion = builder.ReadStorageImage(slimMotion);

                RG::BufferDesc prevBd{ "RestirGiReservoirPrev", prevRes.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
                // Import in the state it was LEFT in last frame (StorageBufferWrite), NOT Undefined: the
                // RG only makes last frame's write available if the read barrier's src carries a real
                // access mask. Undefined → srcAccessMask=0 → no availability → the cross-frame read sees
                // stale/zero data and temporal never accumulates. see arch/rendering-pipeline.md
                data.reservoirPrev = rg.ImportBuffer(prevBd, (void*)prevRes.buffer, RG::ResourceState::StorageBufferWrite);
                data.reservoirPrev = builder.ReadBuffer(data.reservoirPrev);

                data.reservoirCurr = builder.ReadBuffer(reservoirHandle);
                data.reservoirCurr = builder.WriteBuffer(data.reservoirCurr);
                reservoirHandle    = data.reservoirCurr;
            },
            [this, tpc](GiTemporalData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirGiDescSet[0] == VK_NULL_HANDLE) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_TemporalPipeline->Bind(cmd);
                VkDescriptorSet sets[3] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirGiDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_TemporalPipeline->GetLayout(), 0, 3, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_TemporalPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiTemporalPC), &tpc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Spatial pass — merges each pixel's temporal-output reservoir (b2, read-only) with a few random
        // disk neighbours into a SEPARATE single output (b6), each reused neighbour reweighted by the
        // reconnection Jacobian + RTXDI BASIC bias correction. Reads b2 read-only (neighbour reads must
        // see un-modified values — never in-place) so the temporal ping-pong stays intact as next frame's
        // history. The curr handle ends here: ReadBuffer(reservoirHandle) is its last consumer
        // (temporal→spatial RAW barrier). The spatial buffer is imported ONCE; its handle (spatialHandle)
        // threads into shade's ReadBuffer. Undefined import is correct — the spatial buffer is fully
        // overwritten each frame and consumed same-frame, no cross-frame read. No AS barrier (no rays).
        struct GiSpatialData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::BufferHandle   reservoirIn;
            RG::BufferHandle   reservoirOut;
        };
        RG::BufferHandle spatialHandle{};
        rg.AddComputePass<GiSpatialData>(
            "GiSpatial",
            RG::QueueFamily::AsyncCompute,
            [&, this](GiSpatialData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid()) data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid()) data.normal = builder.ReadStorageImage(slimNormal);

                data.reservoirIn = builder.ReadBuffer(reservoirHandle);

                RG::BufferDesc outBd{ "RestirGiReservoirSpatial", preflightVr->restirGiSpatial.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
                data.reservoirOut = rg.ImportBuffer(outBd, (void*)preflightVr->restirGiSpatial.buffer, RG::ResourceState::Undefined);
                data.reservoirOut = builder.WriteBuffer(data.reservoirOut);
                spatialHandle     = data.reservoirOut;
            },
            [this, spc](GiSpatialData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirGiDescSet[0] == VK_NULL_HANDLE) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_SpatialPipeline->Bind(cmd);
                VkDescriptorSet sets[3] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirGiDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_SpatialPipeline->GetLayout(), 0, 3, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_SpatialPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiSpatialPC), &spc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Shade pass — reads the SPATIAL-output reservoir (b6, threaded via spatialHandle so the RG
        // inserts the spatial→shade barrier) + depth/normal, writes the demodulated GI image. The
        // shader's b6 is the spatial result, bound per-view by WriteView — do NOT rebind here.
        struct GiShadeData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::ResourceHandle gi;
            RG::BufferHandle   reservoir;
        };
        RG::ResourceHandle diHandle{};
        rg.AddComputePass<GiShadeData>(
            "GiShade",
            RG::QueueFamily::AsyncCompute,
            [&, this](GiShadeData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid()) data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid()) data.normal = builder.ReadStorageImage(slimNormal);
                data.reservoir = builder.ReadBuffer(spatialHandle);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                auto giTex = std::static_pointer_cast<VKTexture>(vr->restirGiDI);
                RG::TextureDesc desc;
                desc.name   = "RestirGiDI";
                desc.width  = giTex->GetWidth();
                desc.height = giTex->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;
                data.gi  = rg.ImportResource(desc,
                    (void*)giTex->GetImage(), (void*)giTex->GetImageView(),
                    RG::ResourceState::Undefined);
                data.gi  = builder.WriteStorageImage(data.gi);
                diHandle = data.gi;
            },
            [this, pc](GiShadeData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirGiDescSet[0] == VK_NULL_HANDLE) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_ShadePipeline->Bind(cmd);
                VkDescriptorSet sets[3] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirGiDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_ShadePipeline->GetLayout(), 0, 3, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_ShadePipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiPC), &pc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        return diHandle;
    }
}
