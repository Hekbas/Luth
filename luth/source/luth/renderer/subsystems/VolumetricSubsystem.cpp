#include "luthpch.h"
#include "luth/renderer/subsystems/VolumetricSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/lighting/FogVolumeGatherer.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/scene/systems/LightingSystem.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/core/FrameData.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"

#include <cstring>

namespace Luth
{
    namespace
    {
        struct InjectPC
        {
            Mat4 invView;             // 64 B — push once per dispatch; avoids per-voxel inverse(ubo.view).
            u32  volDimX, volDimY, volDimZ, _pad;  // 16 B — atlas dims, runtime-set per quality.
        };

        struct IntegratePC
        {
            Vec4 nearFarPad;          // 16 B — x = nearZ, y = farZ.
            u32  volDimX, volDimY, volDimZ, _pad;  // 16 B — atlas dims.
        };
    }

    void VolumetricSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear-clamp sampler shared by the volumetric pipelines. 3D VKTexture ctor returns a
        // null sampler so each consumer subsystem owns the sampler that matches its sampling
        // needs — Wronski wants linear-clamp on the 3D atlas, distinct from the anisotropic
        // mip-aware sampler the bindless 2D path emits.
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_Sampler);

        // Inject layout (Set 1): b0/b1 storage images (atlas density + current-frame in-scatter),
        // b2/b3/b4/b5 SSBOs (LightSSBO, ClusterGrid, LightIndex, FogVolume), b6 shadow sampler,
        // b7 history sampler (previous-frame in-scatter for temporal accumulation). b0/b1/b7
        // parity-rewrite each frame in WriteInjectPerFrame to ping-pong the in-scatter atlas
        // pair between write target and history source. UAB on every rewritten binding (b0/b1/
        // b2-b5/b7) — mirrors the cluster build / light assign layouts in LightingSubsystem.
        {
            VkDescriptorSetLayoutBinding bindings[8]{};
            for (u32 i = 0; i < 8; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          // volDensity
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          // volInScatter (current write)
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // LightSSBO
            bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // ClusterGrid
            bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // LightIndex
            bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // FogVolumeSSBO
            bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // shadowMap
            bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // volInScatterHistory

            VkDescriptorBindingFlags bindingFlags[8] = {
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                0,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 8;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.pNext        = &bindingFlagsCI;
            layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutCI.bindingCount = 8;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_InjectDescLayout);

            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(InjectPC) };

            if (auto sh = ShaderLibrary::LoadEngine("shaders/volumetric_inject.comp"))
                m_InjectSpv = sh->GetSpirV();
            if (m_InjectSpv.empty())
            {
                LH_CORE_ERROR("VolumetricSubsystem: failed to load volumetric_inject.comp!");
                return;
            }
            // Pipeline layout: Set 0 = GlobalSubsystem's (camera + CSM uniforms), Set 1 = own.
            m_InjectPipeline = std::make_unique<VKComputePipeline>(
                m_InjectSpv,
                std::vector<VkDescriptorSetLayout>{
                    pipeline.GetGlobal().GetSetLayout(),
                    m_InjectDescLayout,
                },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // Integrate layout — b0 sampled volDensity (sampler3D, READ_ONLY layout matches RG's
        // ReadStorageImage transition), b1 read+write volInScatter (storage image, GENERAL).
        // b1 parity-rewrites each frame to point at inject's current write target — UAB required.
        {
            VkDescriptorSetLayoutBinding bindings[2]{};
            for (u32 i = 0; i < 2; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;  // volDensity (sampled)
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;           // volInScatter (R/W)

            VkDescriptorBindingFlags bindingFlags[2] = { 0, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 2;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.pNext        = &bindingFlagsCI;
            layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutCI.bindingCount = 2;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_IntegrateDescLayout);

            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(IntegratePC) };

            if (auto sh = ShaderLibrary::LoadEngine("shaders/volumetric_integrate.comp"))
                m_IntegrateSpv = sh->GetSpirV();
            if (m_IntegrateSpv.empty())
            {
                LH_CORE_ERROR("VolumetricSubsystem: failed to load volumetric_integrate.comp!");
                return;
            }
            m_IntegratePipeline = std::make_unique<VKComputePipeline>(
                m_IntegrateSpv,
                std::vector<VkDescriptorSetLayout>{ m_IntegrateDescLayout },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // Composite layout (Set 1) — b0 sceneDepth, b1 volInScatter (sampler3D). All FRAGMENT.
        // b1 parity-rewrites each frame to sample whichever atlas integrate wrote to — UAB needed.
        // Descriptor set is cycled per MAX_FRAMES_IN_FLIGHT to keep rewrites disjoint from
        // in-flight prior frame's reads.
        {
            VkDescriptorSetLayoutBinding bindings[2]{};
            for (u32 i = 0; i < 2; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorBindingFlags bindingFlags[2] = { 0, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 2;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.pNext        = &bindingFlagsCI;
            layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutCI.bindingCount = 2;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_CompositeDescLayout);

            if (auto sh = ShaderLibrary::LoadEngine("shaders/fullscreen.vert"))
                m_FullscreenVertSpv = sh->GetSpirV();
            if (auto sh = ShaderLibrary::LoadEngine("shaders/volumetric_composite.frag"))
                m_CompositeFragSpv = sh->GetSpirV();
            if (m_FullscreenVertSpv.empty() || m_CompositeFragSpv.empty())
            {
                LH_CORE_ERROR("VolumetricSubsystem: failed to load composite shaders!");
                return;
            }

            PipelineConfig cfg{};
            cfg.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };  // matches SceneColor
            cfg.depthFormat  = VK_FORMAT_UNDEFINED;                 // no depth attachment
            cfg.depthTest    = false;
            cfg.depthWrite   = false;
            cfg.blendEnabled = true;                                // standard alpha — shader emits (fogColor, fogOpacity)
            cfg.cullMode     = VK_CULL_MODE_NONE;
            std::vector<VkDescriptorSetLayout> setLayouts = {
                pipeline.GetGlobal().GetSetLayout(),
                m_CompositeDescLayout,
            };
            m_CompositePipeline = std::make_unique<VKPipeline>(
                cfg, m_FullscreenVertSpv, m_CompositeFragSpv, setLayouts);
        }

        // Viz layout (Set 1) — b0 sceneDepth, b1 volDensity, b2 volInScatter. All FRAGMENT.
        // b2 parity-rewrites per frame to follow integrate's ping-pong target. Cycled set.
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            for (u32 i = 0; i < 3; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorBindingFlags bindingFlags[3] = { 0, 0, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 3;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.pNext        = &bindingFlagsCI;
            layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutCI.bindingCount = 3;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_VizDescLayout);

            if (auto sh = ShaderLibrary::LoadEngine("shaders/volumetric_viz.frag"))
                m_VizFragSpv = sh->GetSpirV();
            if (!m_VizFragSpv.empty() && !m_FullscreenVertSpv.empty())
            {
                VkPushConstantRange pc{ VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                        sizeof(u32) + sizeof(f32) + sizeof(f32) };
                PipelineConfig cfg{};
                cfg.colorFormats       = { VK_FORMAT_R8G8B8A8_UNORM };  // matches LDR
                cfg.depthFormat        = VK_FORMAT_UNDEFINED;
                cfg.depthTest          = false;
                cfg.depthWrite         = false;
                cfg.blendEnabled       = true;
                cfg.cullMode           = VK_CULL_MODE_NONE;
                cfg.pushConstantRanges = { pc };
                std::vector<VkDescriptorSetLayout> vizLayouts = {
                    pipeline.GetGlobal().GetSetLayout(),
                    m_VizDescLayout,
                };
                m_VizPipeline = std::make_unique<VKPipeline>(
                    cfg, m_FullscreenVertSpv, m_VizFragSpv, vizLayouts);
            }
        }
    }

    void VolumetricSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_InjectPipeline.reset();
        m_IntegratePipeline.reset();
        m_CompositePipeline.reset();
        m_VizPipeline.reset();
        if (m_InjectDescLayout)    vkDestroyDescriptorSetLayout(device, m_InjectDescLayout, nullptr);
        if (m_IntegrateDescLayout) vkDestroyDescriptorSetLayout(device, m_IntegrateDescLayout, nullptr);
        if (m_CompositeDescLayout) vkDestroyDescriptorSetLayout(device, m_CompositeDescLayout, nullptr);
        if (m_VizDescLayout)       vkDestroyDescriptorSetLayout(device, m_VizDescLayout, nullptr);
        if (m_Sampler)             vkDestroySampler(device, m_Sampler, nullptr);
        m_InjectDescLayout    = VK_NULL_HANDLE;
        m_IntegrateDescLayout = VK_NULL_HANDLE;
        m_CompositeDescLayout = VK_NULL_HANDLE;
        m_VizDescLayout       = VK_NULL_HANDLE;
        m_Sampler             = VK_NULL_HANDLE;
    }

    bool VolumetricSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        auto deferComp = [](std::unique_ptr<VKComputePipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if (name == "volumetric_inject.comp" && m_InjectDescLayout)
        {
            m_InjectSpv = spv;
            deferComp(m_InjectPipeline);
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(InjectPC) };
            m_InjectPipeline = std::make_unique<VKComputePipeline>(m_InjectSpv,
                std::vector<VkDescriptorSetLayout>{
                    m_Pipeline->GetGlobal().GetSetLayout(),
                    m_InjectDescLayout,
                },
                std::vector<VkPushConstantRange>{ pc });
            return true;
        }
        if (name == "volumetric_integrate.comp" && m_IntegrateDescLayout)
        {
            m_IntegrateSpv = spv;
            deferComp(m_IntegratePipeline);
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(IntegratePC) };
            m_IntegratePipeline = std::make_unique<VKComputePipeline>(m_IntegrateSpv,
                std::vector<VkDescriptorSetLayout>{ m_IntegrateDescLayout },
                std::vector<VkPushConstantRange>{ pc });
            return true;
        }
        if ((name == "volumetric_composite.frag" || name == "fullscreen.vert")
            && m_CompositeDescLayout)
        {
            if (name == "volumetric_composite.frag") m_CompositeFragSpv = spv;
            else                                     m_FullscreenVertSpv = spv;
            if (auto* raw = m_CompositePipeline.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
            PipelineConfig cfg{};
            cfg.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
            cfg.depthFormat  = VK_FORMAT_UNDEFINED;
            cfg.depthTest    = false;
            cfg.depthWrite   = false;
            cfg.blendEnabled = true;
            cfg.cullMode     = VK_CULL_MODE_NONE;
            std::vector<VkDescriptorSetLayout> setLayouts = {
                m_Pipeline->GetGlobal().GetSetLayout(),
                m_CompositeDescLayout,
            };
            m_CompositePipeline = std::make_unique<VKPipeline>(
                cfg, m_FullscreenVertSpv, m_CompositeFragSpv, setLayouts);
            return true;
        }
        if (name == "volumetric_viz.frag" && m_VizDescLayout)
        {
            m_VizFragSpv = spv;
            if (auto* raw = m_VizPipeline.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
            VkPushConstantRange pc{ VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof(u32) + sizeof(f32) + sizeof(f32) };
            PipelineConfig cfg{};
            cfg.colorFormats       = { VK_FORMAT_R8G8B8A8_UNORM };
            cfg.depthFormat        = VK_FORMAT_UNDEFINED;
            cfg.depthTest          = false;
            cfg.depthWrite         = false;
            cfg.blendEnabled       = true;
            cfg.cullMode           = VK_CULL_MODE_NONE;
            cfg.pushConstantRanges = { pc };
            std::vector<VkDescriptorSetLayout> vizLayouts = {
                m_Pipeline->GetGlobal().GetSetLayout(),
                m_VizDescLayout,
            };
            m_VizPipeline = std::make_unique<VKPipeline>(
                cfg, m_FullscreenVertSpv, m_VizFragSpv, vizLayouts);
            return true;
        }
        return false;
    }

    Memory::GPUSubRegion VolumetricSubsystem::UploadFogVolumeSSBO(const GatheredFogVolumes& volumes)
    {
        Memory::GPUSubRegion region{};
        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return region;
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        jobCtx->GpuCache.CurrentTag = frameAbs;

        auto& heap = Memory::GPUTaggedPageAllocator::Get();
        const u64 ssboSize = sizeof(FogVolumeSSBOHeader) + volumes.volumes.size() * sizeof(FogVolumeData);
        region = heap.Allocate(jobCtx->GpuCache, ssboSize, 16);
        if (!region.buffer) return {};

        auto* header = static_cast<FogVolumeSSBOHeader*>(region.mappedPtr);
        header->count = static_cast<u32>(volumes.volumes.size());
        header->_pad[0] = header->_pad[1] = header->_pad[2] = 0;
        if (!volumes.volumes.empty())
        {
            auto* dst = reinterpret_cast<FogVolumeData*>(
                static_cast<u8*>(region.mappedPtr) + sizeof(FogVolumeSSBOHeader));
            std::memcpy(dst, volumes.volumes.data(), volumes.volumes.size() * sizeof(FogVolumeData));
        }
        heap.FlushRegion(region);
        m_LastFogVolumeRegion = region;
        return region;
    }

    void VolumetricSubsystem::WriteInjectView(ViewResources& vr)
    {
        // Only b6 (shadow array sampler) is stable across frames now — b0/b1/b7 parity-rewrite
        // each frame in WriteInjectPerFrame to ping-pong the in-scatter atlases.
        if (m_InjectDescLayout == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        auto& lighting  = m_Pipeline->GetLighting();

        VkDescriptorImageInfo shadowInfo{};
        if (auto shadowTex = lighting.GetShadowMap())
        {
            shadowInfo.imageView   = std::static_pointer_cast<VKTexture>(shadowTex)->GetImageView();
            shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            shadowInfo.sampler     = lighting.GetShadowSampler();
        }
        if (!shadowInfo.sampler) return;

        VkWriteDescriptorSet writes[MAX_FRAMES_IN_FLIGHT]{};
        u32 w = 0;
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSet set = vr.volInjectDescSet[i];
            if (set == VK_NULL_HANDLE) continue;
            writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[w].dstSet          = set;
            writes[w].dstBinding      = 6;
            writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[w].descriptorCount = 1;
            writes[w].pImageInfo      = &shadowInfo;
            ++w;
        }
        vkUpdateDescriptorSets(device, w, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteInjectPerFrame(const Memory::GPUSubRegion& lightSSBORegion,
                                                  const Memory::GPUSubRegion& clusterGridRegion,
                                                  const Memory::GPUSubRegion& lightIndexRegion,
                                                  const Memory::GPUSubRegion& fogVolumeRegion,
                                                  u32 frameAbs)
    {
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
        const bool parity  = (frameAbs & 1u) != 0u;

        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!vr || vr->volInjectDescSet[slot] == VK_NULL_HANDLE) return;
        if (!vr->volDensity || !vr->volInScatter || !vr->volInScatterHistory) return;
        if (!lightSSBORegion.buffer || !clusterGridRegion.buffer ||
            !lightIndexRegion.buffer || !fogVolumeRegion.buffer)
            return;

        // Ping-pong: parity=0 writes volInScatter and reads volInScatterHistory; parity=1 swaps.
        auto vkDens  = std::static_pointer_cast<VKTexture>(vr->volDensity);
        auto vkWrite = std::static_pointer_cast<VKTexture>(
            parity ? vr->volInScatterHistory : vr->volInScatter);
        auto vkHist  = std::static_pointer_cast<VKTexture>(
            parity ? vr->volInScatter        : vr->volInScatterHistory);

        VkDescriptorImageInfo densInfo{};
        densInfo.imageView   = vkDens->GetImageView();
        densInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo writeInfo{};
        writeInfo.imageView   = vkWrite->GetImageView();
        writeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo histInfo{};
        histInfo.imageView   = vkHist->GetImageView();
        histInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        histInfo.sampler     = m_Sampler;

        VkDescriptorBufferInfo lightBi { lightSSBORegion.buffer,   lightSSBORegion.offset,   lightSSBORegion.size   };
        VkDescriptorBufferInfo gridBi  { clusterGridRegion.buffer, clusterGridRegion.offset, clusterGridRegion.size };
        VkDescriptorBufferInfo indexBi { lightIndexRegion.buffer,  lightIndexRegion.offset,  lightIndexRegion.size  };
        VkDescriptorBufferInfo fogBi   { fogVolumeRegion.buffer,   fogVolumeRegion.offset,   fogVolumeRegion.size   };

        VkWriteDescriptorSet writes[7]{};
        VkDescriptorSet set = vr->volInjectDescSet[slot];

        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &densInfo;

        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &writeInfo;

        const VkDescriptorBufferInfo* bufInfos[4] = { &lightBi, &gridBi, &indexBi, &fogBi };
        for (u32 i = 0; i < 4; ++i)
        {
            writes[2 + i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[2 + i].dstSet          = set;
            writes[2 + i].dstBinding      = 2 + i;
            writes[2 + i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2 + i].descriptorCount = 1;
            writes[2 + i].pBufferInfo     = bufInfos[i];
        }

        writes[6] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[6].dstSet          = set;
        writes[6].dstBinding      = 7;
        writes[6].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[6].descriptorCount = 1;
        writes[6].pImageInfo      = &histInfo;

        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 7, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteIntegrateView(ViewResources& vr)
    {
        // Only b0 (density sampler) is stable — b1 (in-scatter storage write target) rewrites
        // each frame in WriteIntegratePerFrame to follow inject's ping-pong parity.
        if (m_IntegrateDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volDensity) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        auto vkDens = std::static_pointer_cast<VKTexture>(vr.volDensity);

        // RG ReadStorageImage transitions volDensity to SHADER_READ_ONLY_OPTIMAL. m_Sampler is
        // unused by texelFetch but Vulkan requires a valid sampler in the descriptor.
        VkDescriptorImageInfo densInfo{};
        densInfo.imageView   = vkDens->GetImageView();
        densInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        densInfo.sampler     = m_Sampler;

        VkWriteDescriptorSet writes[MAX_FRAMES_IN_FLIGHT]{};
        u32 w = 0;
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSet set = vr.volIntegrateDescSet[i];
            if (set == VK_NULL_HANDLE) continue;
            writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[w].dstSet          = set;
            writes[w].dstBinding      = 0;
            writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[w].descriptorCount = 1;
            writes[w].pImageInfo      = &densInfo;
            ++w;
        }
        vkUpdateDescriptorSets(device, w, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteIntegratePerFrame(ViewResources& vr, u32 frameAbs)
    {
        if (m_IntegrateDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volInScatter || !vr.volInScatterHistory) return;

        const u32 slot    = frameAbs % MAX_FRAMES_IN_FLIGHT;
        const bool parity = (frameAbs & 1u) != 0u;
        if (vr.volIntegrateDescSet[slot] == VK_NULL_HANDLE) return;

        auto vkWrite = std::static_pointer_cast<VKTexture>(
            parity ? vr.volInScatterHistory : vr.volInScatter);

        VkDescriptorImageInfo scatInfo{};
        scatInfo.imageView   = vkWrite->GetImageView();
        scatInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vr.volIntegrateDescSet[slot];
        write.dstBinding      = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo      = &scatInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);
    }

    void VolumetricSubsystem::WriteCompositeView(ViewResources& vr, FrameTargets& targets)
    {
        // Only b0 (sceneDepth sampler) is stable — b1 (in-scatter sampler) rewrites each frame
        // in WriteCompositePerFrame to follow integrate's ping-pong parity.
        if (m_CompositeDescLayout == VK_NULL_HANDLE) return;

        auto sceneDepthTex = targets.GetSceneDepth();
        if (!sceneDepthTex) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        auto vkDepth = std::static_pointer_cast<VKTexture>(sceneDepthTex);

        VkDescriptorImageInfo depthInfo{};
        depthInfo.imageView   = vkDepth->GetImageView();
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depthInfo.sampler     = m_Sampler;

        VkWriteDescriptorSet writes[MAX_FRAMES_IN_FLIGHT]{};
        u32 w = 0;
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSet set = vr.volCompositeDescSet[i];
            if (set == VK_NULL_HANDLE) continue;
            writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[w].dstSet          = set;
            writes[w].dstBinding      = 0;
            writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[w].descriptorCount = 1;
            writes[w].pImageInfo      = &depthInfo;
            ++w;
        }
        vkUpdateDescriptorSets(device, w, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteCompositePerFrame(ViewResources& vr, FrameTargets& /*targets*/, u32 frameAbs)
    {
        if (m_CompositeDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volInScatter || !vr.volInScatterHistory) return;

        const u32 slot    = frameAbs % MAX_FRAMES_IN_FLIGHT;
        const bool parity = (frameAbs & 1u) != 0u;
        if (vr.volCompositeDescSet[slot] == VK_NULL_HANDLE) return;

        // Sample whichever atlas integrate wrote to this frame — same parity rule as inject's b1.
        auto vkScat = std::static_pointer_cast<VKTexture>(
            parity ? vr.volInScatterHistory : vr.volInScatter);

        VkDescriptorImageInfo scatInfo{};
        scatInfo.imageView   = vkScat->GetImageView();
        scatInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        scatInfo.sampler     = m_Sampler;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vr.volCompositeDescSet[slot];
        write.dstBinding      = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &scatInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);
    }

    RG::ResourceHandle VolumetricSubsystem::AddCompositePass(RG::RenderGraph& rg,
                                                              RG::ResourceHandle sceneColor,
                                                              RG::ResourceHandle sceneDepth,
                                                              RG::ResourceHandle inScatter)
    {
        if (!m_CompositePipeline) return sceneColor;

        struct CompositeData {
            RG::ResourceHandle color;
            RG::ResourceHandle depth;
            RG::ResourceHandle inScatter;
        };
        RG::ResourceHandle outputHandle;

        rg.AddPass<CompositeData>("VolumetricComposite",
            [&, sceneColor, sceneDepth, inScatter](CompositeData& data, RG::RenderPassBuilder& builder)
            {
                data.color = builder.Write(sceneColor,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                data.depth = builder.Read(sceneDepth);
                // Sampler-binding 1 of the composite descriptor — declaring the read makes RG emit
                // the GENERAL → SHADER_READ_ONLY transition after integrate's storage write.
                if (inScatter.IsValid())
                    data.inScatter = builder.Read(inScatter);
                outputHandle = data.color;
            },
            [this](CompositeData& /*data*/, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                const auto* view = m_Pipeline->GetCurrentView();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "VolumetricComposite",
                    "SceneColor", false,
                    { "volumetric_composite", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                if (!m_CompositePipeline || vr->volCompositeDescSet[slot] == VK_NULL_HANDLE)
                {
                    sys.GetFrameDebugger().EndCapturePass();
                    return;
                }

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_CompositePipeline->Bind(cmd);

                VkDescriptorSet sets[2] = {
                    vr->globalDescriptorSet[slot],
                    vr->volCompositeDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_CompositePipeline->GetLayout(), 0, 2, sets, 0, nullptr);

                u32 w = view->targets->GetSceneColor()->GetWidth();
                u32 h = view->targets->GetSceneColor()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("VolumetricComposite", "FullscreenTriangle",
                    "VolumetricComposite", 0, 0, dummyPC,
                    { "volumetric_composite", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                sys.GetFrameDebugger().EndCapturePass();
            });
        return outputHandle;
    }

    RG::ResourceHandle VolumetricSubsystem::AddIntegratePass(RG::RenderGraph& rg, InjectOutputs injectOut)
    {
        struct IntegrateData
        {
            RG::ResourceHandle density;
            RG::ResourceHandle inScatter;
        };
        RG::ResourceHandle outputHandle;

        rg.AddComputePass<IntegrateData>("VolumetricIntegrate", RG::QueueFamily::AsyncCompute,
            [&, injectOut](IntegrateData& data, RG::RenderPassBuilder& builder)
            {
                // Reuse inject's ResourceNodes (no fresh ImportResource — arch hazard #1). The atlases
                // are persistent VMA images shared across both passes; aliasing them onto distinct
                // nodes would diverge state tracking between the two passes' Solve walks.
                data.density   = builder.ReadStorageImage(injectOut.density);
                data.inScatter = builder.WriteStorageImage(injectOut.inScatter);
                outputHandle   = data.inScatter;
            },
            [this](IntegrateData& /*data*/, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "VolumetricIntegrate",
                    "VolInScatter", false,
                    { "volumetric_integrate", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;

                if (!m_IntegratePipeline || vr->volIntegrateDescSet[slot] == VK_NULL_HANDLE)
                {
                    sys.GetFrameDebugger().EndCapturePass();
                    return;
                }

                m_IntegratePipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_IntegratePipeline->GetLayout(), 0, 1, &vr->volIntegrateDescSet[slot], 0, nullptr);

                IntegratePC pc{};
                pc.nearFarPad = Vec4(m_Pipeline->GetCurrentView()->camera.nearZ,
                                     m_Pipeline->GetCurrentView()->camera.farZ, 0.0f, 0.0f);
                pc.volDimX = vr->volDimX; pc.volDimY = vr->volDimY; pc.volDimZ = vr->volDimZ;
                vkCmdPushConstants(cmd, m_IntegratePipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(IntegratePC), &pc);

                // 2D dispatch over (x, y); each thread walks the full Z column.
                const u32 groupX = (vr->volDimX + 7) / 8;
                const u32 groupY = (vr->volDimY + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);

                sys.GetFrameDebugger().CaptureComputeDispatch("VolumetricIntegrate",
                    "volumetric_integrate", groupX, groupY, 1);
                sys.GetFrameDebugger().EndCapturePass();
            });
        return outputHandle;
    }

    VolumetricSubsystem::InjectOutputs VolumetricSubsystem::AddInjectPass(RG::RenderGraph& rg,
        const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount])
    {
        struct InjectData
        {
            RG::ResourceHandle density;
            RG::ResourceHandle inScatter;
            RG::ResourceHandle history;
            RG::ResourceHandle shadowCascades[k_ShadowCascadeCount];
        };
        InjectOutputs output{};

        rg.AddComputePass<InjectData>("VolumetricInject", RG::QueueFamily::AsyncCompute,
            [&, this](InjectData& data, RG::RenderPassBuilder& builder)
            {
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const bool parity  = (frameAbs & 1u) != 0u;

                RG::TextureDesc descD;
                descD.name   = "VolDensity";
                descD.width  = vr->volDimX;
                descD.height = vr->volDimY;
                descD.format = RG::TextureFormat::RGBA16_Float;
                auto vkDens  = std::static_pointer_cast<VKTexture>(vr->volDensity);
                data.density = rg.ImportResource(descD,
                    (void*)vkDens->GetImage(), (void*)vkDens->GetImageView(),
                    RG::ResourceState::Undefined);
                data.density = builder.WriteStorageImage(data.density);

                // Ping-pong: parity=0 writes volInScatter and reads volInScatterHistory; parity=1
                // swaps. Both VkImages are distinct, so two ImportResource calls don't alias the
                // same node (arch hazard #1 only fires on duplicate imports of the same image).
                auto vkScatW = std::static_pointer_cast<VKTexture>(
                    parity ? vr->volInScatterHistory : vr->volInScatter);
                auto vkScatH = std::static_pointer_cast<VKTexture>(
                    parity ? vr->volInScatter        : vr->volInScatterHistory);

                RG::TextureDesc descW;
                descW.name   = parity ? "VolInScatter[history]" : "VolInScatter";
                descW.width  = vr->volDimX;
                descW.height = vr->volDimY;
                descW.format = RG::TextureFormat::RGBA16_Float;
                data.inScatter = rg.ImportResource(descW,
                    (void*)vkScatW->GetImage(), (void*)vkScatW->GetImageView(),
                    RG::ResourceState::Undefined);
                data.inScatter = builder.WriteStorageImage(data.inScatter);

                RG::TextureDesc descH;
                descH.name   = parity ? "VolInScatter" : "VolInScatter[history]";
                descH.width  = vr->volDimX;
                descH.height = vr->volDimY;
                descH.format = RG::TextureFormat::RGBA16_Float;
                data.history = rg.ImportResource(descH,
                    (void*)vkScatH->GetImage(), (void*)vkScatH->GetImageView(),
                    RG::ResourceState::Undefined);
                data.history = builder.ReadStorageImage(data.history);

                // Per-cascade read triggers DEPTH→SHADER_READ barriers — shader binding 6 samples
                // the full shadow-map array. ReadStorageImage despite the COMBINED_IMAGE_SAMPLER
                // descriptor — builder name is about queue affinity (COMPUTE_SHADER stage), not
                // descriptor type. Target layout SHADER_READ_ONLY matches sampler use.
                for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                    if (shadowHandles[i].IsValid())
                        data.shadowCascades[i] = builder.ReadStorageImage(shadowHandles[i]);

                output.density   = data.density;
                output.inScatter = data.inScatter;
            },
            [this](InjectData& /*data*/, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "VolumetricInject",
                    "VolInScatter", false,
                    { "volumetric_inject", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;

                if (!m_InjectPipeline || vr->volInjectDescSet[slot] == VK_NULL_HANDLE)
                {
                    sys.GetFrameDebugger().EndCapturePass();
                    return;
                }

                m_InjectPipeline->Bind(cmd);

                // Set 0 = Global UBO (camera + CSM + nearZ/farZ), Set 1 = volumetric (atlases + SSBOs
                // + shadow sampler). Both per-view + per-frame-cycled.
                VkDescriptorSet sets[2] = {
                    vr->globalDescriptorSet[slot],
                    vr->volInjectDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_InjectPipeline->GetLayout(), 0, 2, sets, 0, nullptr);

                // Push invView once per dispatch — avoids per-voxel inverse(ubo.view) (~40 ALU ops).
                InjectPC pc{};
                pc.invView = Math::Inverse(m_Pipeline->GetCurrentView()->camera.view);
                pc.volDimX = vr->volDimX; pc.volDimY = vr->volDimY; pc.volDimZ = vr->volDimZ;
                vkCmdPushConstants(cmd, m_InjectPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(InjectPC), &pc);

                const u32 groupX = (vr->volDimX + 7) / 8;
                const u32 groupY = (vr->volDimY + 7) / 8;
                const u32 groupZ = (vr->volDimZ + 3) / 4;
                vkCmdDispatch(cmd, groupX, groupY, groupZ);

                sys.GetFrameDebugger().CaptureComputeDispatch("VolumetricInject",
                    "volumetric_inject", groupX, groupY, groupZ);
                sys.GetFrameDebugger().EndCapturePass();
            });
        return output;
    }

    void VolumetricSubsystem::WriteVizView(ViewResources& vr, FrameTargets& targets)
    {
        // Stable: b0 (sceneDepth sampler), b1 (volDensity sampler). b2 (volInScatter) follows
        // ping-pong parity, rewritten in WriteVizPerFrame.
        if (m_VizDescLayout == VK_NULL_HANDLE) return;
        auto sceneDepthTex = targets.GetSceneDepth();
        if (!sceneDepthTex || !vr.volDensity) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        auto vkDepth = std::static_pointer_cast<VKTexture>(sceneDepthTex);
        auto vkDens  = std::static_pointer_cast<VKTexture>(vr.volDensity);

        VkDescriptorImageInfo depthInfo{};
        depthInfo.imageView   = vkDepth->GetImageView();
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depthInfo.sampler     = m_Sampler;
        VkDescriptorImageInfo densInfo{};
        densInfo.imageView   = vkDens->GetImageView();
        densInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        densInfo.sampler     = m_Sampler;

        VkWriteDescriptorSet writes[2 * MAX_FRAMES_IN_FLIGHT]{};
        u32 w = 0;
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSet set = vr.volVizDescSet[i];
            if (set == VK_NULL_HANDLE) continue;
            writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[w].dstSet          = set;
            writes[w].dstBinding      = 0;
            writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[w].descriptorCount = 1;
            writes[w].pImageInfo      = &depthInfo;
            ++w;
            writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[w].dstSet          = set;
            writes[w].dstBinding      = 1;
            writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[w].descriptorCount = 1;
            writes[w].pImageInfo      = &densInfo;
            ++w;
        }
        vkUpdateDescriptorSets(device, w, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteVizPerFrame(ViewResources& vr, u32 frameAbs)
    {
        if (m_VizDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volInScatter || !vr.volInScatterHistory) return;

        const u32 slot    = frameAbs % MAX_FRAMES_IN_FLIGHT;
        const bool parity = (frameAbs & 1u) != 0u;
        if (vr.volVizDescSet[slot] == VK_NULL_HANDLE) return;

        // Same parity rule as composite: sample whichever atlas integrate wrote to this frame.
        auto vkScat = std::static_pointer_cast<VKTexture>(
            parity ? vr.volInScatterHistory : vr.volInScatter);

        VkDescriptorImageInfo scatInfo{};
        scatInfo.imageView   = vkScat->GetImageView();
        scatInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        scatInfo.sampler     = m_Sampler;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vr.volVizDescSet[slot];
        write.dstBinding      = 2;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &scatInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);
    }

    RG::ResourceHandle VolumetricSubsystem::AddVizPass(RG::RenderGraph& rg,
                                                       RG::ResourceHandle ldrInput,
                                                       RG::ResourceHandle density,
                                                       RG::ResourceHandle inScatter,
                                                       RG::ResourceHandle sceneDepth,
                                                       u32 mode)
    {
        if (!m_VizPipeline) return ldrInput;

        struct VizData {
            RG::ResourceHandle output;
            RG::ResourceHandle depth;
            RG::ResourceHandle density;
            RG::ResourceHandle inScatter;
        };
        RG::ResourceHandle outputHandle;

        rg.AddPass<VizData>("VolumetricVizPass",
            [&, ldrInput, sceneDepth, density, inScatter](VizData& d, RG::RenderPassBuilder& builder)
            {
                VkClearValue clearVal{ { { 0.f, 0.f, 0.f, 1.f } } };
                d.output = builder.Write(ldrInput, VK_ATTACHMENT_LOAD_OP_LOAD,
                                                   VK_ATTACHMENT_STORE_OP_STORE, clearVal);
                d.depth  = builder.Read(sceneDepth);
                // Both atlases sampled via descriptors — RG MUST know so it emits the
                // GENERAL → SHADER_READ_ONLY transitions (v3.0.4 lesson, hazard #1 family).
                if (density.IsValid())   d.density   = builder.Read(density);
                if (inScatter.IsValid()) d.inScatter = builder.Read(inScatter);
                outputHandle = d.output;
            },
            [this, mode](VizData&, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                const auto* view = m_Pipeline->GetCurrentView();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "VolumetricVizPass",
                    "LDROutput", false,
                    { "volumetric_viz", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                if (!vr || vr->volVizDescSet[slot] == VK_NULL_HANDLE ||
                    vr->globalDescriptorSet[slot] == VK_NULL_HANDLE)
                {
                    sys.GetFrameDebugger().EndCapturePass();
                    return;
                }

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_VizPipeline->Bind(cmd);
                VkDescriptorSet sets[2] = {
                    vr->globalDescriptorSet[slot],
                    vr->volVizDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_VizPipeline->GetLayout(), 0, 2, sets, 0, nullptr);

                struct VizPC { u32 mode; f32 scale; f32 overlayAlpha; } pc{};
                pc.mode         = mode;
                // Density rarely exceeds 1.0; in-scatter is HDR radiance (can be >> 1). Slim default
                // scale that keeps both visually meaningful; finer tuning happens via the toggle.
                pc.scale        = (mode == 0u) ? 5.0f : 0.5f;
                pc.overlayAlpha = 0.75f;
                vkCmdPushConstants(cmd, m_VizPipeline->GetLayout(),
                                   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VizPC), &pc);

                u32 w = view->targets->GetLDROutput()->GetWidth();
                u32 h = view->targets->GetLDROutput()->GetHeight();
                VkViewport vp{}; vp.width = (f32)w; vp.height = (f32)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("VolumetricVizPass", "FullscreenTriangle",
                    "VolumetricViz", 0, 0, dummyPC,
                    { "volumetric_viz", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                sys.GetFrameDebugger().EndCapturePass();
            });
        return outputHandle;
    }
}
