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
        // Sizes the reservoir allocation only; the GPU layout lives in restir_common.glsl's
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
            i32  gbufferScale;   // 1 = full-res; 2 = half-res DI (G-buffer reads remap to full)
            i32  dispatchW;      // DI working (dispatch) resolution
            i32  dispatchH;
            f32  diSpecClamp;    // fills the pre-pointer pad (offset 84); read only by the shade pass
            u64  geomTableBDA;   // cutout alpha-test material fetch; stays 8-aligned at offset 88
        };
        static_assert(sizeof(RestirPC) == 96, "RestirPC must be 96 B (matches restir_initial.slang push_constant)");

        // Temporal-pass push constants. Same 80 B footprint + COMPUTE range as RestirPC, so the two
        // share the existing pcRange; the field meanings differ (M-cap + validation thresholds).
        struct RestirTemporalPC {
            Mat4 invViewProj;
            u32  mCap;
            u32  frameSeed;
            f32  depthThreshold;
            f32  normalThreshold;
            i32  gbufferScale;
            i32  dispatchW;
            i32  dispatchH;
        };
        static_assert(sizeof(RestirTemporalPC) == 92, "RestirTemporalPC must match restir_temporal.slang push_constant");

        // Spatial-pass push constants. Shares the fixed COMPUTE pcRange with the other three pipelines;
        // only the field meanings differ (neighbour disk + reject + final-visibility geometry table).
        struct RestirSpatialPC {
            Mat4 invViewProj;
            u32  neighbourCount;
            u32  radius;
            u32  frameSeed;
            f32  depthThreshold;
            i32  gbufferScale;
            i32  dispatchW;
            i32  dispatchH;
            f32  normalThreshold;      // min dot(neighbourN, currN)
            f32  roughnessThreshold;   // max |neighbourRough - rough| (spec reuse gate)
            f32  _pad0;                // pointer-alignment pad (Slang places geomTable at offset 104)
            u64  geomTableBDA;         // final-visibility alpha-test material fetch
        };
        static_assert(sizeof(RestirSpatialPC) == 112, "RestirSpatialPC must match restir_spatial.slang push_constant");

        // Fixed push-constant range shared by the four DI pipelines; 128 B (Vulkan min) leaves headroom
        // for the largest struct (spatial 100 B) plus later growth without touching the pipeline layout.
        constexpr u32 k_RestirPCSize = 128;

        struct UpscalePC {
            i32 fullW;
            i32 fullH;
            i32 halfW;
            i32 halfH;
            f32 phiDepth;
            f32 phiNormal;
        };
        static_assert(sizeof(UpscalePC) == 24, "UpscalePC must match bilateral_upscale.slang push_constant");
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
        LH_PROFILE_FUNCTION();
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear clamp-to-edge; same shape as RtSubsystem's pass sampler for SceneDepth/SlimNormal.
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_Sampler);

        // Set 2 (pass-local): b0 depth sampler, b1 slimNormal sampler, b2 reservoir SCRATCH
        // (initial -> temporal in-place, r/w SSBO), b3 DI storage image, b4 reservoir HISTORY = the
        // spatial buffer (read SSBO), b5 motion sampler, b6 spatial-output reservoir (write SSBO,
        // same buffer as b4). initial uses b0/b1/b2; temporal uses b0/b1/b2/b4/b5; spatial uses
        // b0/b1/b2(read)/b6(write); shade uses b0/b1/b6(read)/b3. All stable per-view.
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
        bindings[7].binding         = 7;   // slimRoughness sampler (combined diffuse+spec target + spec shade)
        bindings[7].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[7].descriptorCount = 1;
        bindings[7].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[8].binding         = 8;   // restirDISpec storage image (demodulated specular out)
        bindings[8].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[8].descriptorCount = 1;
        bindings[8].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        // b2/b4 are now stable per-view (written at WriteView time like the rest); the UAB flags stay
        // so a resize-time rewrite while older cycled slots are still in flight remains legal
        // (VUID-vkUpdateDescriptorSets-None-03047).
        VkDescriptorBindingFlags bindingFlags[9] = {
            0,                                            // b0 depth sampler
            0,                                            // b1 normal sampler
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,  // b2 reservoir scratch
            0,                                            // b3 DI storage image
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,  // b4 reservoir history (spatial buffer)
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
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_temporal.slang"))
            m_TemporalSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_spatial.slang"))
            m_SpatialSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_shade.slang"))
            m_ShadeSpv = sh->GetSpirV();
        if (m_InitialSpv.empty() || m_TemporalSpv.empty() || m_SpatialSpv.empty() || m_ShadeSpv.empty())
        {
            LH_LOG(Renderer, error, "RtRestirSubsystem: failed to load restir_initial/temporal/spatial/shade.comp SPIR-V");
            return;
        }

        // Sets: 0 = global (UBO b0 + TLAS b6), 1 = light SSBO, 2 = pass-local. The initial AND spatial
        // passes add Set 3 (Material SSBO) + Set 4 (bindless) for the cutout alpha-test on their
        // visibility rays (material_bindings_rt.slang); temporal/shade trace no rays, 3-set layout.
        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
        };
        std::vector<VkDescriptorSetLayout> layoutsInitial = layouts;
        layoutsInitial.push_back(MaterialSystem::GetDescriptorSetLayout());
        layoutsInitial.push_back(VulkanContext::Get().GetBindlessSet().GetLayout());
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, k_RestirPCSize };

        m_InitialPipeline = std::make_unique<VKComputePipeline>(
            m_InitialSpv, layoutsInitial, std::vector<VkPushConstantRange>{ pcRange });
        m_TemporalPipeline = std::make_unique<VKComputePipeline>(
            m_TemporalSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        m_SpatialPipeline = std::make_unique<VKComputePipeline>(
            m_SpatialSpv, layoutsInitial, std::vector<VkPushConstantRange>{ pcRange });
        m_ShadePipeline = std::make_unique<VKComputePipeline>(
            m_ShadeSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });

        // Half-res DI bilateral-upscale pipeline (shared bilateral_upscale.slang). Set 0 = global UBO;
        // Set 1 = b0 half-res signal sampler, b1 depth sampler, b2 normal sampler, b3 full-res storage.
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
                VkPushConstantRange upc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpscalePC) };
                m_UpscalePipeline = std::make_unique<VKComputePipeline>(
                    m_UpscaleSpv, ulayouts, std::vector<VkPushConstantRange>{ upc });
            }
        }
    }

    void RtRestirSubsystem::Shutdown()
    {
        LH_PROFILE_FUNCTION();
        VkDevice device = VulkanContext::Get().GetDevice();
        m_InitialPipeline.reset();
        m_TemporalPipeline.reset();
        m_SpatialPipeline.reset();
        m_ShadePipeline.reset();
        m_UpscalePipeline.reset();
        if (m_Sampler)          vkDestroySampler(device, m_Sampler, nullptr);
        if (m_SetLayout)        vkDestroyDescriptorSetLayout(device, m_SetLayout, nullptr);
        if (m_UpscaleSetLayout) vkDestroyDescriptorSetLayout(device, m_UpscaleSetLayout, nullptr);
        m_Sampler          = VK_NULL_HANDLE;
        m_SetLayout        = VK_NULL_HANDLE;
        m_UpscaleSetLayout = VK_NULL_HANDLE;
        m_InitialSpv.clear();
        m_TemporalSpv.clear();
        m_SpatialSpv.clear();
        m_ShadeSpv.clear();
        m_UpscaleSpv.clear();
        m_Pipeline = nullptr;
    }

    bool RtRestirSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        LH_PROFILE_FUNCTION();
        if (m_SetLayout == VK_NULL_HANDLE || !m_Pipeline) return false;

        if (name == "bilateral_upscale.slang" && m_UpscaleSetLayout != VK_NULL_HANDLE)
        {
            m_UpscaleSpv = spv;
            if (auto* raw = m_UpscalePipeline.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
            const std::vector<VkDescriptorSetLayout> ulayouts = {
                m_Pipeline->GetGlobal().GetSetLayout(), m_UpscaleSetLayout };
            VkPushConstantRange upc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpscalePC) };
            m_UpscalePipeline = std::make_unique<VKComputePipeline>(
                m_UpscaleSpv, ulayouts, std::vector<VkPushConstantRange>{ upc });
            return true;
        }

        const bool isInitial  = (name == "restir_initial.slang");
        const bool isTemporal = (name == "restir_temporal.slang");
        const bool isSpatial  = (name == "restir_spatial.slang");
        const bool isShade    = (name == "restir_shade.slang");
        if (!isInitial && !isTemporal && !isSpatial && !isShade) return false;

        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
        };
        std::vector<VkDescriptorSetLayout> layoutsInitial = layouts;
        layoutsInitial.push_back(MaterialSystem::GetDescriptorSetLayout());
        layoutsInitial.push_back(VulkanContext::Get().GetBindlessSet().GetLayout());
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, k_RestirPCSize };

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
                m_SpatialSpv, layoutsInitial, std::vector<VkPushConstantRange>{ pcRange });
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
        LH_PROFILE_FUNCTION();
        if (vr.restirDescSet[0] == VK_NULL_HANDLE) return;
        if (!targets.GetSceneDepth() || !targets.GetSlimNormal() || !targets.GetSlimMotion()
            || !targets.GetSlimRoughness() || !vr.restirDI || !vr.restirDISpec) return;
        if (!vr.restirSpatial.buffer || !vr.restirReservoir.buffer) return;

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
        specInfo.imageView   = specView;   // restirDISpec: GENERAL (storage write from shade)
        specInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // b2 scratch reservoir (initial -> temporal in-place, same-frame lifetime only) + b4 temporal
        // history + b6 spatial output. b4 and b6 alias the SAME per-view buffer: temporal reads last
        // frame's spatial result (b4) before spatial overwrites it (b6); the RG emits the WAR barrier.
        VkDescriptorBufferInfo scratchInfo{
            vr.restirReservoir.buffer, vr.restirReservoir.offset, vr.restirReservoir.size };
        VkDescriptorBufferInfo spatialInfo{
            vr.restirSpatial.buffer, vr.restirSpatial.offset, vr.restirSpatial.size };

        // All Set 2 bindings are stable per-view now (b2/b4 stopped ping-ponging with the post-spatial
        // history topology); rewritten only on view alloc/resize.
        VkWriteDescriptorSet writes[9 * MAX_FRAMES_IN_FLIGHT]{};
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
            writes[n].dstBinding      = 2;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[n].descriptorCount = 1;
            writes[n].pBufferInfo     = &scratchInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 4;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[n].descriptorCount = 1;
            writes[n].pBufferInfo     = &spatialInfo;
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

    RtRestirSubsystem::Outputs RtRestirSubsystem::AddPasses(RG::RenderGraph& rg,
                                                    RG::ResourceHandle sceneDepth,
                                                    RG::ResourceHandle slimNormal,
                                                    RG::ResourceHandle slimMotion,
                                                    RG::ResourceHandle slimRoughness)
    {
        LH_PROFILE_FUNCTION();
        const RestirSettings& settings = m_Pipeline->GetSystem().GetRestirSettings();
        if (!settings.enabled || !m_InitialPipeline || !m_TemporalPipeline || !m_SpatialPipeline || !m_ShadePipeline) return {};

        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || !preflightVr->restirDI
            || !preflightVr->restirReservoir.buffer
            || !preflightVr->restirSpatial.buffer) return {};
        if (m_Pipeline->GetRt().GetTlas() == VK_NULL_HANDLE) return {};

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());

        // Post-spatial history topology: initial + temporal share the single SCRATCH reservoir (b2,
        // same-frame lifetime); the SPATIAL buffer doubles as temporal history (read at b4) and
        // spatial output (written at b6), persisting across frames. No ping-pong, no parity swap.
        const Memory::GPUSubRegion scratchRes = preflightVr->restirReservoir;
        const Memory::GPUSubRegion spatialRes = preflightVr->restirSpatial;

        // Build invViewProj + frameSeed once; initial/shade share RestirPC, temporal + spatial each
        // use their own PC (all inside the shared fixed pcRange, different field meanings).
        const Mat4 invVP = Math::Inverse(m_Pipeline->GetGlobal().GetCachedViewProj());

        // DI working resolution (half when RestirSettings::halfResolution): derived from restirDI's extent,
        // the sizing source of truth. G-buffer reads remap to full res in-shader.
        auto diTex0 = std::static_pointer_cast<VKTexture>(preflightVr->restirDI);
        const i32 diW2    = diTex0 ? static_cast<i32>(diTex0->GetWidth())  : static_cast<i32>(preflightVr->width);
        const i32 diH2    = diTex0 ? static_cast<i32>(diTex0->GetHeight()) : static_cast<i32>(preflightVr->height);
        const i32 diScale = ((u32)diW2 == preflightVr->width && (u32)diH2 == preflightVr->height) ? 1 : 2;

        RestirPC pc{};
        pc.invViewProj    = invVP;
        pc.candidateCount = settings.candidateCount;
        pc.diSpecClamp    = settings.diSpecClamp;
        pc.frameSeed      = frameAbs;
        pc.gbufferScale   = diScale;
        pc.dispatchW      = diW2;
        pc.dispatchH      = diH2;
        pc.geomTableBDA   = m_Pipeline->GetRt().GetGeometryTableBDA();

        RestirTemporalPC tpc{};
        tpc.invViewProj     = invVP;
        tpc.mCap            = settings.temporalMCap;
        tpc.frameSeed       = frameAbs;
        tpc.depthThreshold  = settings.temporalDepthThreshold;
        tpc.normalThreshold = settings.temporalNormalThreshold;
        tpc.gbufferScale    = diScale;
        tpc.dispatchW       = diW2;
        tpc.dispatchH       = diH2;

        RestirSpatialPC spc{};
        spc.invViewProj    = invVP;
        spc.neighbourCount = settings.spatialNeighbours;
        spc.radius         = settings.spatialRadius;
        spc.frameSeed      = frameAbs;
        spc.depthThreshold = settings.spatialDepthThreshold;
        spc.normalThreshold    = settings.spatialNormalThreshold;
        spc.roughnessThreshold = settings.roughnessThreshold;
        spc.gbufferScale   = diScale;
        spc.dispatchW      = diW2;
        spc.dispatchH      = diH2;
        spc.geomTableBDA   = m_Pipeline->GetRt().GetGeometryTableBDA();

        // Initial pass: RIS over point lights + one visibility ray, writes the SCRATCH reservoir.
        // The scratch buffer is imported ONCE here; its handle threads through temporal (read+write)
        // and spatial (read) so the RG chains the barriers across all three (re-importing would
        // alias distinct nodes). Undefined import is correct: fully overwritten, no cross-frame read.
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

                RG::BufferDesc bd{ "RestirReservoirScratch", scratchRes.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
                data.reservoir  = rg.ImportBuffer(bd, (void*)scratchRes.buffer, RG::ResourceState::Undefined);
                data.reservoir  = builder.WriteBuffer(data.reservoir);
                reservoirHandle = data.reservoir;
            },
            [this, pc](RestirInitialData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirDescSet[0] == VK_NULL_HANDLE) return;

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

                const u32 groupX = (static_cast<u32>(pc.dispatchW) + 7) / 8;
                const u32 groupY = (static_cast<u32>(pc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Temporal pass: reprojects via motion + merges last frame's SPATIAL output (the history) into
        // the scratch RIS candidate in-place. No AS barrier (traces no rays). The history is the
        // per-view spatial buffer, imported ONCE here in its true last-left state (StorageBufferWrite,
        // NOT Undefined: Undefined -> srcAccess=0 -> no cross-frame availability -> stale temporal
        // read; see arch/rendering-pipeline.md); its handle threads into spatial's WriteBuffer so the
        // RG emits the temporal-read -> spatial-write WAR barrier on the same node. SCRATCH threads
        // through reservoirHandle as read+write (initial->temporal RAW barrier).
        struct RestirTemporalData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::ResourceHandle motion;
            RG::ResourceHandle rough;
            RG::BufferHandle   reservoirCurr;
            RG::BufferHandle   reservoirPrev;
        };
        RG::BufferHandle spatialHandle{};
        rg.AddComputePass<RestirTemporalData>(
            "RestirTemporal",
            RG::QueueFamily::AsyncCompute,
            [&, this](RestirTemporalData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid())    data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid())    data.normal = builder.ReadStorageImage(slimNormal);
                if (slimMotion.IsValid())    data.motion = builder.ReadStorageImage(slimMotion);
                if (slimRoughness.IsValid()) data.rough  = builder.ReadStorageImage(slimRoughness);

                RG::BufferDesc histBd{ "RestirReservoirSpatial", spatialRes.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
                spatialHandle      = rg.ImportBuffer(histBd, (void*)spatialRes.buffer, RG::ResourceState::StorageBufferWrite);
                data.reservoirPrev = builder.ReadBuffer(spatialHandle);

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

                const u32 groupX = (static_cast<u32>(tpc.dispatchW) + 7) / 8;
                const u32 groupY = (static_cast<u32>(tpc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Spatial pass: merges each pixel's temporal-output reservoir (b2) with a few random disk
        // neighbours, rejecting dissimilar geometry, into the spatial/history buffer (b6), then traces
        // one final-visibility ray on the selected sample (5-set layout: Material + Bindless for the
        // alpha test; the initial pass's AS-build -> COMPUTE barrier covers this same-queue trace).
        // Reads b2 read-only (neighbour reads must see un-modified values; never in-place). The scratch
        // handle ends here: ReadBuffer(reservoirHandle) is its last consumer (temporal->spatial RAW
        // barrier). WriteBuffer on the SAME node temporal read (spatialHandle) yields the WAR barrier;
        // the result persists as next frame's history.
        struct RestirSpatialData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::ResourceHandle rough;
            RG::BufferHandle   reservoirIn;
            RG::BufferHandle   reservoirOut;
        };
        rg.AddComputePass<RestirSpatialData>(
            "RestirSpatial",
            RG::QueueFamily::AsyncCompute,
            [&, this](RestirSpatialData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid())    data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid())    data.normal = builder.ReadStorageImage(slimNormal);
                if (slimRoughness.IsValid()) data.rough  = builder.ReadStorageImage(slimRoughness);

                data.reservoirIn  = builder.ReadBuffer(reservoirHandle);
                data.reservoirOut = builder.WriteBuffer(spatialHandle);
                spatialHandle     = data.reservoirOut;
            },
            [this, spc](RestirSpatialData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirDescSet[0] == VK_NULL_HANDLE) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_SpatialPipeline->Bind(cmd);
                VkDescriptorSet sets[5] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirDescSet[slot],
                    MaterialSystem::GetDescriptorSet(slot),
                    VulkanContext::Get().GetBindlessSet().GetSet(),
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_SpatialPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_SpatialPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirSpatialPC), &spc);

                const u32 groupX = (static_cast<u32>(spc.dispatchW) + 7) / 8;
                const u32 groupY = (static_cast<u32>(spc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Shade pass: reads the SPATIAL-output reservoir (b6) + depth/normal, writes demodulated DI
        // image. Reads spatialHandle (not the temporal output); the shader's b6 is the spatial result.
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

                // Second output: demodulated specular (b8). Imported once here; its handle feeds
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

                const u32 groupX = (static_cast<u32>(pc.dispatchW) + 7) / 8;
                const u32 groupY = (static_cast<u32>(pc.dispatchH) + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        return { diHandle, specHandle };
    }

    void RtRestirSubsystem::WriteUpscaleView(ViewResources& vr, FrameTargets& targets)
    {
        LH_PROFILE_FUNCTION();
        if (!targets.GetSceneDepth() || !targets.GetSlimNormal()) return;

        VkDescriptorImageInfo depthInfo{ m_Sampler,
            std::static_pointer_cast<VKTexture>(targets.GetSceneDepth())->GetImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo normalInfo{ m_Sampler,
            std::static_pointer_cast<VKTexture>(targets.GetSlimNormal())->GetImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        auto writeSet = [&](VkDescriptorSet set, const std::shared_ptr<Texture>& half, const std::shared_ptr<Texture>& full)
        {
            if (set == VK_NULL_HANDLE || !half || !full) return;
            VkDescriptorImageInfo halfInfo{ m_Sampler, std::static_pointer_cast<VKTexture>(half)->GetImageView(), VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, std::static_pointer_cast<VKTexture>(full)->GetImageView(), VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w[4]{};
            for (u32 i = 0; i < 4; ++i) { w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[i].dstSet = set; w[i].dstBinding = i; w[i].descriptorCount = 1; }
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &halfInfo;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &depthInfo;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &normalInfo;
            w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          w[3].pImageInfo = &outInfo;
            vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 4, w, 0, nullptr);
        };
        writeSet(vr.diUpscaleDescSet,     vr.svgfDiHalf,     vr.svgfDenoised);
        writeSet(vr.diSpecUpscaleDescSet, vr.svgfDiSpecHalf, vr.svgfDiSpecDenoised);
    }

    RG::ResourceHandle RtRestirSubsystem::AddUpscalePass(RG::RenderGraph& rg, RG::ResourceHandle half,
                                                         RG::ResourceHandle sceneDepth, RG::ResourceHandle slimNormal, bool specular)
    {
        LH_PROFILE_FUNCTION();
        if (!m_UpscalePipeline || !half.IsValid()) return half;
        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr) return half;
        const VkDescriptorSet descSet = specular ? preflightVr->diSpecUpscaleDescSet : preflightVr->diUpscaleDescSet;
        const std::shared_ptr<Texture>& outShared = specular ? preflightVr->svgfDiSpecDenoised : preflightVr->svgfDenoised;
        if (descSet == VK_NULL_HANDLE || !outShared) return half;

        struct UpData { RG::ResourceHandle half, depth, normal, out; };
        RG::ResourceHandle outHandle{};
        rg.AddComputePass<UpData>(
            specular ? "DiSpecUpscale" : "DiUpscale",
            RG::QueueFamily::AsyncCompute,
            [&, this, specular](UpData& data, RG::RenderPassBuilder& builder) {
                data.half = builder.ReadStorageImageGeneral(half);  // svgfDiHalf/svgfDiSpecHalf stay GENERAL
                if (sceneDepth.IsValid()) data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid()) data.normal = builder.ReadStorageImage(slimNormal);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                auto outTex = std::static_pointer_cast<VKTexture>(specular ? vr->svgfDiSpecDenoised : vr->svgfDenoised);
                RG::TextureDesc desc;
                desc.name   = specular ? "SvgfDiSpecDenoised" : "SvgfDenoised";
                desc.width  = outTex->GetWidth();
                desc.height = outTex->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;
                data.out = rg.ImportResource(desc, (void*)outTex->GetImage(), (void*)outTex->GetImageView(),
                                             RG::ResourceState::Undefined);
                data.out  = builder.WriteStorageImage(data.out);
                outHandle = data.out;
            },
            [this, specular](UpData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr) return;
                const VkDescriptorSet set = specular ? vr->diSpecUpscaleDescSet : vr->diUpscaleDescSet;
                auto fullShared = specular ? vr->svgfDiSpecDenoised : vr->svgfDenoised;
                auto halfShared = specular ? vr->svgfDiSpecHalf     : vr->svgfDiHalf;
                if (set == VK_NULL_HANDLE || !fullShared || !halfShared) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_UpscalePipeline->Bind(cmd);
                VkDescriptorSet sets[2] = { vr->globalDescriptorSet[slot], set };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_UpscalePipeline->GetLayout(), 0, 2, sets, 0, nullptr);

                auto full  = std::static_pointer_cast<VKTexture>(fullShared);
                auto halfT = std::static_pointer_cast<VKTexture>(halfShared);
                const RestirSettings& s = m_Pipeline->GetSystem().GetRestirSettings();
                UpscalePC pc{};
                pc.fullW = (i32)full->GetWidth();  pc.fullH = (i32)full->GetHeight();
                pc.halfW = (i32)halfT->GetWidth(); pc.halfH = (i32)halfT->GetHeight();
                pc.phiDepth  = s.spatialDepthThreshold;
                pc.phiNormal = 32.0f;
                vkCmdPushConstants(cmd, m_UpscalePipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpscalePC), &pc);

                const u32 gx = (full->GetWidth()  + 7) / 8;
                const u32 gy = (full->GetHeight() + 7) / 8;
                vkCmdDispatch(cmd, gx, gy, 1);
            });
        return outHandle;
    }
}
