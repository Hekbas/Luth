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
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/core/FrameData.h"
#include "luth/core/types/LuthMath.h"

namespace Luth
{
    namespace {
        // Sizes the reservoir allocation only; the GPU layout lives in restir_gi_common.slang's
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
            i32  gbufferScale;   // 1 = full-res; 2 = half-res GI (G-buffer reads remap to full)
            i32  dispatchW;      // GI working (dispatch) resolution
            i32  dispatchH;
            u64  geomTableBDA;   // geometry-table BDA; stays 8-aligned at offset 88
        };
        static_assert(sizeof(GiPC) == 96, "GiPC must match restir_gi_initial.slang push_constant (shade reads the 88 B prefix)");

        // Temporal-pass push constants: same 80 B footprint as GiPC (one shared pcRange), different
        // fields. mCap + maxReservoirAge bit-packed so invViewProj + 4 scalars fit 80 B.
        struct GiTemporalPC {
            Mat4 invViewProj;
            u32  mCapMaxAge;        // mCap (low 16) | maxReservoirAge (high 16)
            u32  frameSeed;
            f32  depthThreshold;
            f32  normalThreshold;
            i32  gbufferScale;
            i32  dispatchW;
            i32  dispatchH;
        };
        static_assert(sizeof(GiTemporalPC) == 92, "GiTemporalPC must match restir_gi_temporal.slang push_constant");

        // Spatial-pass push constants: 80 B (shared pcRange). neighbours+radius bit-packed.
        struct GiSpatialPC {
            Mat4 invViewProj;
            u32  neighboursRadius;   // neighbours (low 16) | radius (high 16)
            u32  frameSeed;
            f32  depthThreshold;
            f32  normalThreshold;
            i32  gbufferScale;
            i32  dispatchW;
            i32  dispatchH;
        };
        static_assert(sizeof(GiSpatialPC) == 92, "GiSpatialPC must match restir_gi_spatial.slang push_constant");

        struct GiUpscalePC {
            i32 fullW;
            i32 fullH;
            i32 halfW;
            i32 halfH;
            f32 phiDepth;
            f32 phiNormal;
        };
        static_assert(sizeof(GiUpscalePC) == 24, "GiUpscalePC must match gi_upscale.comp push_constant");
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
        LH_PROFILE_FUNCTION();
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear clamp-to-edge, same shape as the DI pass sampler for SceneDepth/SlimNormal.
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_Sampler);

        // Set 2 (pass-local): identical 7-binding shape to the DI subsystem. b0 depth sampler, b1
        // slimNormal sampler, b2 reservoir CURR (r/w SSBO), b3 GI storage image, b4 reservoir PREV
        // (read SSBO), b5 motion sampler, b6 spatial-output reservoir. b2/b4 swap each frame.
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
        // cycled slots may still be pending on the GPU; UAB satisfies VUID-vkUpdateDescriptorSets-
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

        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_gi_initial.slang"))
            m_InitialSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_gi_temporal.slang"))
            m_TemporalSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_gi_spatial.slang"))
            m_SpatialSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_gi_shade.slang"))
            m_ShadeSpv = sh->GetSpirV();
        if (m_InitialSpv.empty() || m_TemporalSpv.empty() || m_SpatialSpv.empty() || m_ShadeSpv.empty())
        {
            LH_LOG(Renderer, error, "RtRestirGiSubsystem: failed to load restir_gi_initial/temporal/spatial/shade.comp SPIR-V");
            return;
        }

        // Sets: 0 = global (UBO b0 + TLAS b6), 1 = light SSBO, 2 = pass-local. temporal/spatial/shade.
        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
        };
        // Initial adds set 3 = Material SSBO + set 4 = bindless textures (secondary-hit material fetch).
        // Only the initial pass shades L_o; the others are L_o-agnostic and keep the 3-set layout.
        const std::vector<VkDescriptorSetLayout> initialLayouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
            MaterialSystem::GetDescriptorSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
        };
        // Shared 88 B range: GiPC carries geomTableBDA; GiTemporalPC/GiSpatialPC push their 80 B subset.
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiPC) };

        m_InitialPipeline = std::make_unique<VKComputePipeline>(
            m_InitialSpv, initialLayouts, std::vector<VkPushConstantRange>{ pcRange });
        m_TemporalPipeline = std::make_unique<VKComputePipeline>(
            m_TemporalSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        m_SpatialPipeline = std::make_unique<VKComputePipeline>(
            m_SpatialSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        m_ShadePipeline = std::make_unique<VKComputePipeline>(
            m_ShadeSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });

        // Reservoir debug-viz graphics pipeline (ShadeMode::RestirGiReservoir). One set: b0 depth
        // sampler, b1 spatial-reservoir SSBO. Fullscreen triangle -> heat-map blended over LDR.
        {
            VkDescriptorSetLayoutBinding vb[2]{};
            vb[0].binding = 0; vb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; vb[0].descriptorCount = 1; vb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            vb[1].binding = 1; vb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;          vb[1].descriptorCount = 1; vb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo vci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            vci.bindingCount = 2; vci.pBindings = vb;
            vkCreateDescriptorSetLayout(device, &vci, nullptr, &m_ReservoirVizSetLayout);

            if (auto sh = ShaderLibrary::LoadEngine("shaders/fullscreen.slang"))              m_FullscreenVertSpv   = sh->GetSpirV();
            if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_gi_reservoir_viz.slang")) m_ReservoirVizFragSpv = sh->GetSpirV();
            if (!m_FullscreenVertSpv.empty() && !m_ReservoirVizFragSpv.empty())
            {
                std::vector<VkDescriptorSetLayout> vlayouts = { m_ReservoirVizSetLayout };
                VkPushConstantRange vpc{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24 };  // vec2 viewport + vec2 reservoir + mCap + ageCap
                PipelineConfig cfg;
                cfg.colorFormats       = { VK_FORMAT_R8G8B8A8_UNORM };
                cfg.depthFormat        = VK_FORMAT_UNDEFINED;
                cfg.depthTest          = false;
                cfg.depthWrite         = false;
                cfg.blendEnabled       = true;
                cfg.cullMode           = VK_CULL_MODE_NONE;
                cfg.pushConstantRanges = { vpc };
                m_ReservoirVizPipeline = std::make_unique<VKPipeline>(
                    cfg, m_FullscreenVertSpv, m_ReservoirVizFragSpv, vlayouts);
            }
        }

        // Half-res GI bilateral-upscale pipeline. Set 0 = global UBO (nearZ/farZ); Set 1 = b0 half-res GI
        // sampler, b1 depth sampler, b2 normal sampler, b3 full-res svgfGiDenoised storage.
        {
            VkDescriptorSetLayoutBinding ub[4]{};
            ub[0].binding = 0; ub[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ub[0].descriptorCount = 1; ub[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            ub[1].binding = 1; ub[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ub[1].descriptorCount = 1; ub[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            ub[2].binding = 2; ub[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ub[2].descriptorCount = 1; ub[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            ub[3].binding = 3; ub[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          ub[3].descriptorCount = 1; ub[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo uci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            uci.bindingCount = 4; uci.pBindings = ub;
            vkCreateDescriptorSetLayout(device, &uci, nullptr, &m_UpscaleSetLayout);

            if (auto sh = ShaderLibrary::LoadEngine("shaders/bilateral_upscale.slang")) m_UpscaleSpv = sh->GetSpirV();
            if (!m_UpscaleSpv.empty())
            {
                const std::vector<VkDescriptorSetLayout> ulayouts = {
                    m_Pipeline->GetGlobal().GetSetLayout(),
                    m_UpscaleSetLayout,
                };
                VkPushConstantRange upc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiUpscalePC) };
                m_UpscalePipeline = std::make_unique<VKComputePipeline>(
                    m_UpscaleSpv, ulayouts, std::vector<VkPushConstantRange>{ upc });
            }
        }
    }

    void RtRestirGiSubsystem::Shutdown()
    {
        LH_PROFILE_FUNCTION();
        VkDevice device = VulkanContext::Get().GetDevice();
        m_InitialPipeline.reset();
        m_TemporalPipeline.reset();
        m_SpatialPipeline.reset();
        m_ShadePipeline.reset();
        m_UpscalePipeline.reset();
        m_ReservoirVizPipeline.reset();
        if (m_Sampler)               vkDestroySampler(device, m_Sampler, nullptr);
        if (m_SetLayout)             vkDestroyDescriptorSetLayout(device, m_SetLayout, nullptr);
        if (m_UpscaleSetLayout)      vkDestroyDescriptorSetLayout(device, m_UpscaleSetLayout, nullptr);
        if (m_ReservoirVizSetLayout) vkDestroyDescriptorSetLayout(device, m_ReservoirVizSetLayout, nullptr);
        m_Sampler               = VK_NULL_HANDLE;
        m_SetLayout             = VK_NULL_HANDLE;
        m_UpscaleSetLayout      = VK_NULL_HANDLE;
        m_ReservoirVizSetLayout = VK_NULL_HANDLE;
        m_InitialSpv.clear();
        m_TemporalSpv.clear();
        m_SpatialSpv.clear();
        m_ShadeSpv.clear();
        m_UpscaleSpv.clear();
        m_FullscreenVertSpv.clear();
        m_ReservoirVizFragSpv.clear();
        m_Pipeline = nullptr;
    }

    bool RtRestirGiSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        LH_PROFILE_FUNCTION();
        if (m_SetLayout == VK_NULL_HANDLE || !m_Pipeline) return false;

        // Reservoir debug-viz graphics pipeline (its own frag + the shared fullscreen vert). Rebuilt
        // with the same config as Init; the old pipeline defers a frame (an in-flight frame may bind it).
        if ((name == "restir_gi_reservoir_viz.slang" || name == "fullscreen.slang") && m_ReservoirVizSetLayout != VK_NULL_HANDLE)
        {
            if (name == "restir_gi_reservoir_viz.slang") m_ReservoirVizFragSpv = spv;
            else                                        m_FullscreenVertSpv   = spv;
            if (m_ReservoirVizPipeline)
                VulkanContext::Get().PushDeletion([p = m_ReservoirVizPipeline.release()]() { delete p; });
            std::vector<VkDescriptorSetLayout> vlayouts = { m_ReservoirVizSetLayout };
            VkPushConstantRange vpc{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24 };
            PipelineConfig cfg;
            cfg.colorFormats       = { VK_FORMAT_R8G8B8A8_UNORM };
            cfg.depthFormat        = VK_FORMAT_UNDEFINED;
            cfg.depthTest          = false;
            cfg.depthWrite         = false;
            cfg.blendEnabled       = true;
            cfg.cullMode           = VK_CULL_MODE_NONE;
            cfg.pushConstantRanges = { vpc };
            m_ReservoirVizPipeline = std::make_unique<VKPipeline>(
                cfg, m_FullscreenVertSpv, m_ReservoirVizFragSpv, vlayouts);
            return true;
        }

        if (name == "bilateral_upscale.slang" && m_UpscaleSetLayout != VK_NULL_HANDLE)
        {
            m_UpscaleSpv = spv;
            if (auto* raw = m_UpscalePipeline.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
            const std::vector<VkDescriptorSetLayout> ulayouts = {
                m_Pipeline->GetGlobal().GetSetLayout(), m_UpscaleSetLayout };
            VkPushConstantRange upc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiUpscalePC) };
            m_UpscalePipeline = std::make_unique<VKComputePipeline>(
                m_UpscaleSpv, ulayouts, std::vector<VkPushConstantRange>{ upc });
            return true;
        }

        const bool isInitial  = (name == "restir_gi_initial.slang");
        const bool isTemporal = (name == "restir_gi_temporal.slang");
        const bool isSpatial  = (name == "restir_gi_spatial.slang");
        const bool isShade    = (name == "restir_gi_shade.slang");
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
            // Initial keeps its 5-set layout (Material + bindless), mirroring Init.
            const std::vector<VkDescriptorSetLayout> initialLayouts = {
                m_Pipeline->GetGlobal().GetSetLayout(),
                m_Pipeline->GetLighting().GetSetLayout(),
                m_SetLayout,
                MaterialSystem::GetDescriptorSetLayout(),
                VulkanContext::Get().GetBindlessSet().GetLayout(),
            };
            m_InitialSpv = spv;
            deferComp(m_InitialPipeline);
            m_InitialPipeline = std::make_unique<VKComputePipeline>(
                m_InitialSpv, initialLayouts, std::vector<VkPushConstantRange>{ pcRange });
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
        LH_PROFILE_FUNCTION();
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

        // b6 spatial-output reservoir: single per-view buffer, stable like b0/b1/b3/b5.
        VkDescriptorBufferInfo spatialInfo{
            vr.restirGiSpatial.buffer, vr.restirGiSpatial.offset, vr.restirGiSpatial.size };

        // Stable per-view bindings only: b0 depth, b1 normal, b3 GI, b5 motion, b6 spatial out. b2/b4
        // (reservoirs) swap each frame; WriteReservoirBindings owns them.
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
        LH_PROFILE_FUNCTION();
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
        LH_PROFILE_FUNCTION();
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
        // GI working resolution (half when RestirGiSettings::halfResolution): derive from restirGiDI's
        // extent (the alloc-time sizing source of truth). G-buffer reads remap to full res in-shader.
        auto giTex0 = std::static_pointer_cast<VKTexture>(preflightVr->restirGiDI);
        const i32 giW2    = giTex0 ? static_cast<i32>(giTex0->GetWidth())  : static_cast<i32>(preflightVr->width);
        const i32 giH2    = giTex0 ? static_cast<i32>(giTex0->GetHeight()) : static_cast<i32>(preflightVr->height);
        const i32 giScale = ((u32)giW2 == preflightVr->width && (u32)giH2 == preflightVr->height) ? 1 : 2;

        GiPC pc{};
        pc.invViewProj     = invVP;
        pc.frameSeed       = frameAbs;
        pc.secondaryAlbedo = settings.secondaryAlbedo;
        pc.maxIndirect     = settings.maxIndirect;
        pc.gbufferScale    = giScale;
        pc.dispatchW       = giW2;
        pc.dispatchH       = giH2;
        // Geometry-table BDA read at preflight, paired with the same m_LastResult that GlobalSubsystem
        // binds to Set 0 b6, so the table's instanceCustomIndex mapping matches the bound TLAS. Zero
        // until the first real TLAS build (only the empty TLAS exists -> all rays miss -> never deref'd).
        pc.geomTableBDA    = m_Pipeline->GetRt().GetGeometryTableBDA();

        GiTemporalPC tpc{};
        tpc.invViewProj     = invVP;
        tpc.mCapMaxAge      = (settings.temporalMCap & 0xFFFFu) | ((settings.maxReservoirAge & 0xFFFFu) << 16);
        tpc.frameSeed       = frameAbs;
        tpc.depthThreshold  = settings.temporalDepthThreshold;
        tpc.normalThreshold = settings.temporalNormalThreshold;
        tpc.gbufferScale    = giScale;
        tpc.dispatchW       = giW2;
        tpc.dispatchH       = giH2;

        GiSpatialPC spc{};
        spc.invViewProj      = invVP;
        spc.neighboursRadius = (settings.spatialNeighbours & 0xFFFFu) | ((settings.spatialRadius & 0xFFFFu) << 16);
        spc.frameSeed        = frameAbs;
        spc.depthThreshold   = settings.spatialDepthThreshold;
        spc.normalThreshold  = settings.spatialNormalThreshold;
        spc.gbufferScale     = giScale;
        spc.dispatchW        = giW2;
        spc.dispatchH        = giH2;

        // Initial pass: cosine-sampled 1-bounce path + single-light NEE, writes the CURR reservoir at
        // b2. The curr buffer is imported ONCE here; its handle threads into shade's ReadBuffer so the
        // RG chains the initial->shade RAW barrier.
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

                // AS-build -> AS-read barrier. dstStageMask is COMPUTE_SHADER (NOT RAY_TRACING):
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
                // Set 3 = Material SSBO (render-frame slot, same convention as the PBR pass), Set 4 =
                // bindless textures, for the secondary-hit material fetch. Both only deref'd on a
                // committed hit, so a stale/empty slot on a miss is harmless.
                VkDescriptorSet sets[5] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirGiDescSet[slot],
                    MaterialSystem::GetDescriptorSet(slot),
                    VulkanContext::Get().GetBindlessSet().GetSet(),
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_InitialPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_InitialPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiPC), &pc);

                const u32 groupX = (static_cast<u32>(pc.dispatchW) + 7) / 8;
                const u32 groupY = (static_cast<u32>(pc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Temporal pass: reprojects via motion + merges last frame's PREV reservoir into the CURR
        // candidate in-place, reweighted by the reconnection Jacobian. PREV is a SEPARATE read-only
        // ImportBuffer (last frame's curr, no within-frame producer; cross-frame like taaHistory).
        // CURR threads through reservoirHandle (read+write) so the RG inserts the initial->temporal RAW
        // barrier. No AS barrier; temporal traces no rays.
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
                // access mask. Undefined -> srcAccessMask=0 -> no availability -> the cross-frame read sees
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

                const u32 groupX = (static_cast<u32>(tpc.dispatchW) + 7) / 8;
                const u32 groupY = (static_cast<u32>(tpc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Spatial pass: merges each pixel's temporal-output reservoir (b2, read-only) with a few random
        // disk neighbours into a SEPARATE single output (b6), each reused neighbour reweighted by the
        // reconnection Jacobian + RTXDI BASIC bias correction. Reads b2 read-only (neighbour reads must
        // see un-modified values, never in-place) so the temporal ping-pong stays intact as next frame's
        // history. The curr handle ends here: ReadBuffer(reservoirHandle) is its last consumer
        // (temporal->spatial RAW barrier). The spatial buffer is imported ONCE; its handle (spatialHandle)
        // threads into shade's ReadBuffer. Undefined import is correct: the spatial buffer is fully
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

                const u32 groupX = (static_cast<u32>(spc.dispatchW) + 7) / 8;
                const u32 groupY = (static_cast<u32>(spc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Shade pass: reads the SPATIAL-output reservoir (b6, threaded via spatialHandle so the RG
        // inserts the spatial->shade barrier) + depth/normal, writes the demodulated GI image. The
        // shader's b6 is the spatial result, bound per-view by WriteView; do NOT rebind here.
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

                const u32 groupX = (static_cast<u32>(pc.dispatchW) + 7) / 8;
                const u32 groupY = (static_cast<u32>(pc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        return diHandle;
    }

    void RtRestirGiSubsystem::WriteReservoirVizView(ViewResources& vr, FrameTargets& targets)
    {
        LH_PROFILE_FUNCTION();
        if (vr.giReservoirVizDescSet == VK_NULL_HANDLE) return;
        if (!targets.GetSceneDepth() || !vr.restirGiSpatial.buffer) return;

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler     = m_Sampler;
        depthInfo.imageView   = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth())->GetImageView();
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo resInfo{
            vr.restirGiSpatial.buffer, vr.restirGiSpatial.offset, vr.restirGiSpatial.size };

        VkWriteDescriptorSet w[2]{};
        w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[0].dstSet = vr.giReservoirVizDescSet; w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].descriptorCount = 1; w[0].pImageInfo = &depthInfo;
        w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[1].dstSet = vr.giReservoirVizDescSet; w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].descriptorCount = 1; w[1].pBufferInfo = &resInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 2, w, 0, nullptr);
    }

    RG::ResourceHandle RtRestirGiSubsystem::AddReservoirVizPass(RG::RenderGraph& rg,
                                                               RG::ResourceHandle ldrInput,
                                                               RG::ResourceHandle sceneDepth)
    {
        LH_PROFILE_FUNCTION();
        if (!m_ReservoirVizPipeline) return ldrInput;
        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || preflightVr->giReservoirVizDescSet == VK_NULL_HANDLE
            || !preflightVr->restirGiSpatial.buffer) return ldrInput;

        // Settings captured by value -> stable at record time. mCap approximates the max merged M
        // (temporal cap x the spatial neighbour fan-in + the pixel's own sample).
        const RestirGiSettings& s = m_Pipeline->GetSystem().GetRestirGiSettings();

        struct VizData { RG::ResourceHandle output; RG::ResourceHandle depth; };
        RG::ResourceHandle outHandle{};
        rg.AddPass<VizData>("GiReservoirVizPass",
            [&, ldrInput, sceneDepth](VizData& d, RG::RenderPassBuilder& builder) {
                VkClearValue clearVal{ { { 0.f, 0.f, 0.f, 1.f } } };
                d.output = builder.Write(ldrInput, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, clearVal);
                if (sceneDepth.IsValid()) d.depth = builder.Read(sceneDepth);
                outHandle = d.output;
            },
            [this, s](VizData&, RG::RenderPassContext& ctx) {
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->giReservoirVizDescSet == VK_NULL_HANDLE) return;
                VkCommandBuffer cmd = ctx.commandBuffer;

                m_ReservoirVizPipeline->Bind(cmd);
                VkDescriptorSet sets[1] = { vr->giReservoirVizDescSet };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_ReservoirVizPipeline->GetLayout(), 0, 1, sets, 0, nullptr);

                struct VizPC { float vx, vy, resW, resH, mCap, ageCap; } pc{};
                pc.vx     = static_cast<float>(vr->width);
                pc.vy     = static_cast<float>(vr->height);
                auto giTex = std::static_pointer_cast<VKTexture>(vr->restirGiDI);
                pc.resW   = giTex ? static_cast<float>(giTex->GetWidth())  : static_cast<float>(vr->width);
                pc.resH   = giTex ? static_cast<float>(giTex->GetHeight()) : static_cast<float>(vr->height);
                pc.mCap   = static_cast<float>(s.temporalMCap * (s.spatialNeighbours + 1u) + 1u);
                pc.ageCap = static_cast<float>(s.maxReservoirAge);
                vkCmdPushConstants(cmd, m_ReservoirVizPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VizPC), &pc);

                VkViewport vp{}; vp.width = (float)vr->width; vp.height = (float)vr->height; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { vr->width, vr->height };
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdDraw(cmd, 3, 1, 0, 0);
            });
        return outHandle;
    }

    void RtRestirGiSubsystem::WriteUpscaleView(ViewResources& vr, FrameTargets& targets)
    {
        LH_PROFILE_FUNCTION();
        if (vr.giUpscaleDescSet == VK_NULL_HANDLE) return;
        if (!vr.svgfGiHalf || !vr.svgfGiDenoised || !targets.GetSceneDepth() || !targets.GetSlimNormal()) return;

        VkDescriptorImageInfo halfInfo{ m_Sampler,
            std::static_pointer_cast<VKTexture>(vr.svgfGiHalf)->GetImageView(), VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo depthInfo{ m_Sampler,
            std::static_pointer_cast<VKTexture>(targets.GetSceneDepth())->GetImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo normalInfo{ m_Sampler,
            std::static_pointer_cast<VKTexture>(targets.GetSlimNormal())->GetImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE,
            std::static_pointer_cast<VKTexture>(vr.svgfGiDenoised)->GetImageView(), VK_IMAGE_LAYOUT_GENERAL };

        VkWriteDescriptorSet w[4]{};
        for (u32 i = 0; i < 4; ++i)
        {
            w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[i].dstSet = vr.giUpscaleDescSet; w[i].dstBinding = i; w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &halfInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &depthInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &normalInfo;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          w[3].pImageInfo = &outInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 4, w, 0, nullptr);
    }

    RG::ResourceHandle RtRestirGiSubsystem::AddUpscalePass(RG::RenderGraph& rg, RG::ResourceHandle giHalf,
                                                           RG::ResourceHandle sceneDepth, RG::ResourceHandle slimNormal)
    {
        LH_PROFILE_FUNCTION();
        if (!m_UpscalePipeline || !giHalf.IsValid()) return giHalf;
        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || preflightVr->giUpscaleDescSet == VK_NULL_HANDLE || !preflightVr->svgfGiDenoised)
            return giHalf;

        struct UpData { RG::ResourceHandle half, depth, normal, out; };
        RG::ResourceHandle outHandle{};
        rg.AddComputePass<UpData>(
            "GiUpscale",
            RG::QueueFamily::AsyncCompute,
            [&, this](UpData& data, RG::RenderPassBuilder& builder) {
                data.half = builder.ReadStorageImageGeneral(giHalf);  // svgfGiHalf stays GENERAL (atrous imageStore)
                if (sceneDepth.IsValid()) data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid()) data.normal = builder.ReadStorageImage(slimNormal);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                auto outTex = std::static_pointer_cast<VKTexture>(vr->svgfGiDenoised);
                RG::TextureDesc desc;
                desc.name   = "SvgfGiDenoised";
                desc.width  = outTex->GetWidth();
                desc.height = outTex->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;
                data.out = rg.ImportResource(desc, (void*)outTex->GetImage(), (void*)outTex->GetImageView(),
                                             RG::ResourceState::Undefined);
                data.out  = builder.WriteStorageImage(data.out);
                outHandle = data.out;
            },
            [this](UpData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->giUpscaleDescSet == VK_NULL_HANDLE || !vr->svgfGiDenoised || !vr->svgfGiHalf) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_UpscalePipeline->Bind(cmd);
                VkDescriptorSet sets[2] = { vr->globalDescriptorSet[slot], vr->giUpscaleDescSet };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_UpscalePipeline->GetLayout(), 0, 2, sets, 0, nullptr);

                auto full = std::static_pointer_cast<VKTexture>(vr->svgfGiDenoised);
                auto half = std::static_pointer_cast<VKTexture>(vr->svgfGiHalf);
                const RestirGiSettings& s = m_Pipeline->GetSystem().GetRestirGiSettings();
                GiUpscalePC pc{};
                pc.fullW = (i32)full->GetWidth();  pc.fullH = (i32)full->GetHeight();
                pc.halfW = (i32)half->GetWidth();  pc.halfH = (i32)half->GetHeight();
                pc.phiDepth  = s.spatialDepthThreshold;
                pc.phiNormal = 32.0f;
                vkCmdPushConstants(cmd, m_UpscalePipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiUpscalePC), &pc);

                const u32 gx = (full->GetWidth()  + 7) / 8;
                const u32 gy = (full->GetHeight() + 7) / 8;
                vkCmdDispatch(cmd, gx, gy, 1);
            });
        return outHandle;
    }
}
