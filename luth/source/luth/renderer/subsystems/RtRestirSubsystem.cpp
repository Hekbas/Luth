#include "luthpch.h"
#include "luth/renderer/subsystems/RtRestirSubsystem.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/settings/RestirSettings.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/core/FrameData.h"
#include "luth/core/types/LuthMath.h"

namespace Luth
{
    namespace {
        // Sizes the reservoir allocation only — the GPU layout lives in restir_common.glsl's
        // Reservoir struct; any field change must update both. see arch/rendering-pipeline.md
        struct GPUReservoir {
            u32 lightIndex;
            f32 W, wSum;
            u32 M;
            f32 targetPdf;
            f32 pad0, pad1, pad2;
        };
        static_assert(sizeof(GPUReservoir) == 32, "GPUReservoir must match restir_common.glsl Reservoir (32 B)");

        struct RestirPC {
            Mat4 invViewProj;
            u32  candidateCount;
            u32  frameSeed;
            u64  geomTableBDA;   // cutout alpha-test material fetch (paired with the bound TLAS); 0 = none
        };
        static_assert(sizeof(RestirPC) == 80, "RestirPC must be 80 B (matches restir_initial/shade.comp push_constant)");

        // Temporal-pass push constants. Same 80 B footprint + COMPUTE range as RestirPC, so the two
        // share the existing pcRange; the field meanings differ (M-cap + validation thresholds).
        struct RestirTemporalPC {
            Mat4 invViewProj;
            u32  mCap;
            u32  frameSeed;
            f32  depthThreshold;
            f32  normalThreshold;
        };
        static_assert(sizeof(RestirTemporalPC) == 80, "RestirTemporalPC must match restir_temporal.comp push_constant");

        // Spatial-pass push constants. Same 80 B footprint + COMPUTE range as RestirPC, so all four
        // pipelines share the existing pcRange; only the field meanings differ (neighbour disk + reject).
        struct RestirSpatialPC {
            Mat4 invViewProj;
            u32  neighbourCount;
            u32  radius;
            u32  frameSeed;
            f32  depthThreshold;
        };
        static_assert(sizeof(RestirSpatialPC) == 80, "RestirSpatialPC must match restir_spatial.comp push_constant");
    }

    bool RtRestirSubsystem::IsEnabled() const
    {
        return m_Pipeline && m_Pipeline->GetSystem().GetRestirSettings().enabled;
    }

    void RtRestirSubsystem::SetEnabled(bool e)
    {
        if (m_Pipeline) m_Pipeline->GetSystem().GetRestirSettings().enabled = e;
    }

    void RtRestirSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear clamp-to-edge — same shape as RtSubsystem's pass sampler for SceneDepth/SlimNormal.
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_Sampler);

        // Set 2 (pass-local): b0 depth sampler, b1 slimNormal sampler, b2 reservoir CURR/temporal-out
        // (r/w SSBO), b3 DI storage image, b4 reservoir PREV (read SSBO), b5 motion sampler, b6
        // spatial-output reservoir (write SSBO). initial uses b0/b1/b2; temporal uses b0/b1/b2/b4/b5;
        // spatial uses b0/b1/b2(read)/b6(write); shade uses b0/b1/b6(read)/b3. b2/b4 swap each frame.
        VkDescriptorSetLayoutBinding bindings[9]{};
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
        bindings[7].binding         = 7;   // slimRoughness sampler (#154 combined target + spec shade)
        bindings[7].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[7].descriptorCount = 1;
        bindings[7].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[8].binding         = 8;   // restirDISpec storage image (#154 demodulated specular out)
        bindings[8].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[8].descriptorCount = 1;
        bindings[8].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        // b2/b4 (curr/prev reservoirs) are rewritten per-frame by WriteReservoirBindings while other
        // cycled slots may still be pending on the GPU. UAB satisfies VUID-vkUpdateDescriptorSets-
        // None-03047 — mirrors LightingSubsystem's Set 3 b0-b2 UAB protocol. b0/b1/b3/b5/b6 are stable
        // per-view, written once at WriteView time, so they stay flag-less (b6's buffer is per-view).
        VkDescriptorBindingFlags bindingFlags[9] = {
            0,                                            // b0 depth sampler
            0,                                            // b1 normal sampler
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,  // b2 reservoir curr / temporal out
            0,                                            // b3 DI storage image
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,  // b4 reservoir prev
            0,                                            // b5 motion sampler
            0,                                            // b6 spatial output reservoir
            0,                                            // b7 slimRoughness sampler
            0,                                            // b8 restirDISpec storage image
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsCI.bindingCount  = 9;
        bindingFlagsCI.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutCI.pNext        = &bindingFlagsCI;
        layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutCI.bindingCount = 9;
        layoutCI.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_SetLayout);

        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_initial.slang"))
            m_InitialSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_temporal.comp"))
            m_TemporalSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_spatial.comp"))
            m_SpatialSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_shade.comp"))
            m_ShadeSpv = sh->GetSpirV();
        if (m_InitialSpv.empty() || m_TemporalSpv.empty() || m_SpatialSpv.empty() || m_ShadeSpv.empty())
        {
            LH_CORE_ERROR("RtRestirSubsystem: failed to load restir_initial/temporal/spatial/shade.comp SPIR-V");
            return;
        }

        // Sets: 0 = global (UBO b0 + TLAS b6), 1 = light SSBO, 2 = pass-local. The initial pass adds
        // Set 3 (Material SSBO) + Set 4 (bindless) for the cutout alpha-test (material_bindings_rt.slang); the
        // temporal/spatial/shade passes trace no rays, so they keep the 3-set layout.
        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
        };
        std::vector<VkDescriptorSetLayout> layoutsInitial = layouts;
        layoutsInitial.push_back(MaterialSystem::GetDescriptorSetLayout());
        layoutsInitial.push_back(VulkanContext::Get().GetBindlessSet().GetLayout());
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirPC) };

        m_InitialPipeline = std::make_unique<VKComputePipeline>(
            m_InitialSpv, layoutsInitial, std::vector<VkPushConstantRange>{ pcRange });
        m_TemporalPipeline = std::make_unique<VKComputePipeline>(
            m_TemporalSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        m_SpatialPipeline = std::make_unique<VKComputePipeline>(
            m_SpatialSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        m_ShadePipeline = std::make_unique<VKComputePipeline>(
            m_ShadeSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
    }

    void RtRestirSubsystem::Shutdown()
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

    bool RtRestirSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        if (m_SetLayout == VK_NULL_HANDLE || !m_Pipeline) return false;

        const bool isInitial  = (name == "restir_initial.slang");
        const bool isTemporal = (name == "restir_temporal.comp");
        const bool isSpatial  = (name == "restir_spatial.comp");
        const bool isShade    = (name == "restir_shade.comp");
        if (!isInitial && !isTemporal && !isSpatial && !isShade) return false;

        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
        };
        std::vector<VkDescriptorSetLayout> layoutsInitial = layouts;
        layoutsInitial.push_back(MaterialSystem::GetDescriptorSetLayout());
        layoutsInitial.push_back(VulkanContext::Get().GetBindlessSet().GetLayout());
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirPC) };

        auto deferComp = [](std::unique_ptr<VKComputePipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if (isInitial)
        {
            m_InitialSpv = spv;
            deferComp(m_InitialPipeline);
            m_InitialPipeline = std::make_unique<VKComputePipeline>(
                m_InitialSpv, layoutsInitial, std::vector<VkPushConstantRange>{ pcRange });
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

    void RtRestirSubsystem::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.restirDescSet[0] == VK_NULL_HANDLE) return;
        if (!targets.GetSceneDepth() || !targets.GetSlimNormal() || !targets.GetSlimMotion()
            || !targets.GetSlimRoughness() || !vr.restirDI || !vr.restirDISpec) return;
        if (!vr.restirSpatial.buffer) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        const VkImageView depthView  = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth())->GetImageView();
        const VkImageView normalView = std::static_pointer_cast<VKTexture>(targets.GetSlimNormal())->GetImageView();
        const VkImageView motionView = std::static_pointer_cast<VKTexture>(targets.GetSlimMotion())->GetImageView();
        const VkImageView diView     = std::static_pointer_cast<VKTexture>(vr.restirDI)->GetImageView();
        const VkImageView roughView  = std::static_pointer_cast<VKTexture>(targets.GetSlimRoughness())->GetImageView();
        const VkImageView specView   = std::static_pointer_cast<VKTexture>(vr.restirDISpec)->GetImageView();

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

        VkDescriptorImageInfo diInfo{};
        diInfo.imageView   = diView;
        diInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo roughInfo{};
        roughInfo.sampler     = m_Sampler;
        roughInfo.imageView   = roughView;
        roughInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo specInfo{};
        specInfo.imageView   = specView;   // restirDISpec — GENERAL (storage write from shade)
        specInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // b6 spatial-output reservoir — single per-view buffer, stable like b0/b1/b3/b5.
        VkDescriptorBufferInfo spatialInfo{
            vr.restirSpatial.buffer, vr.restirSpatial.offset, vr.restirSpatial.size };

        // Stable per-view bindings only: b0 depth, b1 normal, b3 DI, b5 motion, b6 spatial out. b2/b4
        // (reservoirs) swap each frame — WriteReservoirBindings owns them.
        VkWriteDescriptorSet writes[7 * MAX_FRAMES_IN_FLIGHT]{};
        u32 n = 0;
        for (u32 slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
        {
            VkDescriptorSet set = vr.restirDescSet[slot];
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
            writes[n].pImageInfo      = &diInfo;
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

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 7;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].descriptorCount = 1;
            writes[n].pImageInfo      = &roughInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 8;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[n].descriptorCount = 1;
            writes[n].pImageInfo      = &specInfo;
            ++n;
        }
        vkUpdateDescriptorSets(device, n, writes, 0, nullptr);
    }

    void RtRestirSubsystem::WriteReservoirBindings(ViewResources& vr)
    {
        if (!vr.restirReservoir[0].buffer || !vr.restirReservoir[1].buffer) return;

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
        if (vr.restirDescSet[slot] == VK_NULL_HANDLE) return;

        // Parity picks curr; prev is the other. Frame N writes curr (initial+temporal) and reads
        // prev (last frame's curr); N+1 swaps. Must match AddPasses' selection exactly.
        const u32 currIdx = (frameAbs & 1u);
        const u32 prevIdx = currIdx ^ 1u;

        VkDescriptorBufferInfo currInfo{
            vr.restirReservoir[currIdx].buffer, vr.restirReservoir[currIdx].offset, vr.restirReservoir[currIdx].size };
        VkDescriptorBufferInfo prevInfo{
            vr.restirReservoir[prevIdx].buffer, vr.restirReservoir[prevIdx].offset, vr.restirReservoir[prevIdx].size };

        VkWriteDescriptorSet writes[2]{};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr.restirDescSet[slot];
        writes[0].dstBinding      = 2;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &currInfo;
        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = vr.restirDescSet[slot];
        writes[1].dstBinding      = 4;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo     = &prevInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 2, writes, 0, nullptr);
    }

    RtRestirSubsystem::Outputs RtRestirSubsystem::AddPasses(RG::RenderGraph& rg,
                                                    RG::ResourceHandle sceneDepth,
                                                    RG::ResourceHandle slimNormal,
                                                    RG::ResourceHandle slimMotion,
                                                    RG::ResourceHandle slimRoughness)
    {
        const RestirSettings& settings = m_Pipeline->GetSystem().GetRestirSettings();
        if (!settings.enabled || !m_InitialPipeline || !m_TemporalPipeline || !m_SpatialPipeline || !m_ShadePipeline) return {};

        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || !preflightVr->restirDI
            || !preflightVr->restirReservoir[0].buffer || !preflightVr->restirReservoir[1].buffer
            || !preflightVr->restirSpatial.buffer) return {};
        if (m_Pipeline->GetRt().GetTlas() == VK_NULL_HANDLE) return {};

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        // Bind the cycled slot's b2/b4 to this frame's curr/prev reservoirs (parity swap). Done here,
        // before the passes record, so the dispatches see the right ping-pong halves.
        WriteReservoirBindings(*preflightVr);

        // Parity picks curr; prev is last frame's curr. Must match WriteReservoirBindings. The spatial
        // output is a single per-view buffer (no ping-pong) — fully overwritten + consumed each frame.
        const u32 currIdx = (frameAbs & 1u);
        const u32 prevIdx = currIdx ^ 1u;
        const Memory::GPUSubRegion currRes    = preflightVr->restirReservoir[currIdx];
        const Memory::GPUSubRegion prevRes    = preflightVr->restirReservoir[prevIdx];
        const Memory::GPUSubRegion spatialRes = preflightVr->restirSpatial;

        // Build invViewProj + frameSeed once; initial/shade share RestirPC, temporal + spatial each
        // use their own PC (same 80 B footprint, different field meanings).
        const Mat4 invVP = Math::Inverse(m_Pipeline->GetGlobal().GetCachedViewProj());
        RestirPC pc{};
        pc.invViewProj    = invVP;
        pc.candidateCount = settings.candidateCount;
        pc.frameSeed      = frameAbs;
        pc.geomTableBDA   = m_Pipeline->GetRt().GetGeometryTableBDA();

        RestirTemporalPC tpc{};
        tpc.invViewProj     = invVP;
        tpc.mCap            = settings.temporalMCap;
        tpc.frameSeed       = frameAbs;
        tpc.depthThreshold  = settings.temporalDepthThreshold;
        tpc.normalThreshold = settings.temporalNormalThreshold;

        RestirSpatialPC spc{};
        spc.invViewProj    = invVP;
        spc.neighbourCount = settings.spatialNeighbours;
        spc.radius         = settings.spatialRadius;
        spc.frameSeed      = frameAbs;
        spc.depthThreshold = settings.spatialDepthThreshold;

        // Initial pass — RIS over point lights + one visibility ray, writes the CURR reservoir.
        // The curr buffer is imported ONCE here; its handle threads through temporal (read+write)
        // and shade (read) so the RG chains the barriers across all three (re-importing would
        // alias distinct nodes).
        struct RestirInitialData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::ResourceHandle rough;
            RG::BufferHandle   reservoir;
        };
        RG::BufferHandle reservoirHandle{};
        rg.AddComputePass<RestirInitialData>(
            "RestirInitial",
            RG::QueueFamily::AsyncCompute,
            [&, this](RestirInitialData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid())    data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid())    data.normal = builder.ReadStorageImage(slimNormal);
                if (slimRoughness.IsValid()) data.rough  = builder.ReadStorageImage(slimRoughness);

                RG::BufferDesc bd{ "RestirReservoirCurr", currRes.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
                data.reservoir  = rg.ImportBuffer(bd, (void*)currRes.buffer, RG::ResourceState::Undefined);
                data.reservoir  = builder.WriteBuffer(data.reservoir);
                reservoirHandle = data.reservoir;
            },
            [this, pc](RestirInitialData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirDescSet[0] == VK_NULL_HANDLE) return;

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
                VkDescriptorSet sets[5] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirDescSet[slot],
                    MaterialSystem::GetDescriptorSet(slot),
                    VulkanContext::Get().GetBindlessSet().GetSet(),
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_InitialPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_InitialPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirPC), &pc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Temporal pass — reprojects via motion + merges last frame's PREV reservoir into the CURR
        // RIS candidate in-place. No AS barrier (traces no rays — visibility stays in initial). PREV
        // is a SEPARATE read-only ImportBuffer (last frame's curr, no within-frame producer — same
        // cross-frame shape as taaHistory). CURR threads through reservoirHandle as read+write so the
        // RG inserts the initial→temporal RAW barrier on the same node.
        struct RestirTemporalData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::ResourceHandle motion;
            RG::ResourceHandle rough;
            RG::BufferHandle   reservoirCurr;
            RG::BufferHandle   reservoirPrev;
        };
        rg.AddComputePass<RestirTemporalData>(
            "RestirTemporal",
            RG::QueueFamily::AsyncCompute,
            [&, this](RestirTemporalData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid())    data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid())    data.normal = builder.ReadStorageImage(slimNormal);
                if (slimMotion.IsValid())    data.motion = builder.ReadStorageImage(slimMotion);
                if (slimRoughness.IsValid()) data.rough  = builder.ReadStorageImage(slimRoughness);

                RG::BufferDesc prevBd{ "RestirReservoirPrev", prevRes.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
                // Import in its true last-left state (StorageBufferWrite), NOT Undefined: Undefined → srcAccess=0
                // → no cross-frame availability → stale temporal read (mirrors the GI prev import). see arch/rendering-pipeline.md
                data.reservoirPrev = rg.ImportBuffer(prevBd, (void*)prevRes.buffer, RG::ResourceState::StorageBufferWrite);
                data.reservoirPrev = builder.ReadBuffer(data.reservoirPrev);

                data.reservoirCurr = builder.ReadBuffer(reservoirHandle);
                data.reservoirCurr = builder.WriteBuffer(data.reservoirCurr);
                reservoirHandle    = data.reservoirCurr;
            },
            [this, tpc](RestirTemporalData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirDescSet[0] == VK_NULL_HANDLE) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_TemporalPipeline->Bind(cmd);
                VkDescriptorSet sets[3] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_TemporalPipeline->GetLayout(), 0, 3, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_TemporalPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirTemporalPC), &tpc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Spatial pass — merges each pixel's temporal-output reservoir (b2) with a few random disk
        // neighbours, rejecting dissimilar geometry, into a SEPARATE single output (b6). Reads b2
        // read-only (neighbour reads must see un-modified values — never in-place) and writes b6, so
        // the temporal ping-pong stays intact as next frame's history. The curr handle ends here:
        // ReadBuffer(reservoirHandle) is its last consumer (temporal→spatial RAW barrier). The spatial
        // buffer is imported ONCE; its handle (spatialHandle) threads into shade's ReadBuffer.
        struct RestirSpatialData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::ResourceHandle rough;
            RG::BufferHandle   reservoirIn;
            RG::BufferHandle   reservoirOut;
        };
        RG::BufferHandle spatialHandle{};
        rg.AddComputePass<RestirSpatialData>(
            "RestirSpatial",
            RG::QueueFamily::AsyncCompute,
            [&, this](RestirSpatialData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid())    data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid())    data.normal = builder.ReadStorageImage(slimNormal);
                if (slimRoughness.IsValid()) data.rough  = builder.ReadStorageImage(slimRoughness);

                data.reservoirIn = builder.ReadBuffer(reservoirHandle);

                RG::BufferDesc outBd{ "RestirReservoirSpatial", spatialRes.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
                data.reservoirOut = rg.ImportBuffer(outBd, (void*)spatialRes.buffer, RG::ResourceState::Undefined);
                data.reservoirOut = builder.WriteBuffer(data.reservoirOut);
                spatialHandle     = data.reservoirOut;
            },
            [this, spc](RestirSpatialData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirDescSet[0] == VK_NULL_HANDLE) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_SpatialPipeline->Bind(cmd);
                VkDescriptorSet sets[3] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_SpatialPipeline->GetLayout(), 0, 3, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_SpatialPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirSpatialPC), &spc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Shade pass — reads the SPATIAL-output reservoir (b6) + depth/normal, writes demodulated DI
        // image. Reads spatialHandle (not the temporal output) — the shader's b6 is the spatial result.
        struct RestirShadeData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::ResourceHandle rough;
            RG::ResourceHandle di;
            RG::ResourceHandle spec;
            RG::BufferHandle   reservoir;
        };
        RG::ResourceHandle diHandle{};
        RG::ResourceHandle specHandle{};
        rg.AddComputePass<RestirShadeData>(
            "RestirShade",
            RG::QueueFamily::AsyncCompute,
            [&, this](RestirShadeData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid())    data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid())    data.normal = builder.ReadStorageImage(slimNormal);
                if (slimRoughness.IsValid()) data.rough  = builder.ReadStorageImage(slimRoughness);
                data.reservoir = builder.ReadBuffer(spatialHandle);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                auto diTex   = std::static_pointer_cast<VKTexture>(vr->restirDI);
                auto specTex = std::static_pointer_cast<VKTexture>(vr->restirDISpec);
                RG::TextureDesc desc;
                desc.name   = "RestirDI";
                desc.width  = diTex->GetWidth();
                desc.height = diTex->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;
                data.di  = rg.ImportResource(desc,
                    (void*)diTex->GetImage(), (void*)diTex->GetImageView(),
                    RG::ResourceState::Undefined);
                data.di  = builder.WriteStorageImage(data.di);
                diHandle = data.di;

                // #154 — second output: demodulated specular (b8). Imported once here; its handle feeds
                // the DiSpecular SVGF denoiser. Mirrors the DI import above (same shape, GENERAL storage).
                RG::TextureDesc specDesc;
                specDesc.name   = "RestirDISpec";
                specDesc.width  = specTex->GetWidth();
                specDesc.height = specTex->GetHeight();
                specDesc.format = RG::TextureFormat::RGBA16_Float;
                data.spec  = rg.ImportResource(specDesc,
                    (void*)specTex->GetImage(), (void*)specTex->GetImageView(),
                    RG::ResourceState::Undefined);
                data.spec  = builder.WriteStorageImage(data.spec);
                specHandle = data.spec;
            },
            [this, pc](RestirShadeData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirDescSet[0] == VK_NULL_HANDLE) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_ShadePipeline->Bind(cmd);
                VkDescriptorSet sets[3] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_ShadePipeline->GetLayout(), 0, 3, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_ShadePipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirPC), &pc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        return { diHandle, specHandle };
    }
}
