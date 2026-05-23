#include "luthpch.h"
#include "luth/renderer/subsystems/VolumetricSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
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
        // Atlas dimensions — match volumetric_inject.comp's VOL_DIM constant and the 3D VKTexture
        // ctor calls in RecreateViewTextures. Local-size 8x8x4 → 20x12x32 group counts per dispatch.
        constexpr u32 k_VolDimX = 160;
        constexpr u32 k_VolDimY = 90;
        constexpr u32 k_VolDimZ = 128;

        struct InjectPC
        {
            Mat4 invView;  // Push 64 B once per dispatch; avoids per-voxel inverse(ubo.view).
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

        // Inject layout (Set 1): b0/b1 storage images, b2/b3/b4/b5 SSBOs (LightSSBO, ClusterGrid,
        // LightIndex, FogVolume), b6 shadow sampler. SSBO bindings rebind per-frame against the
        // latest tagged-heap regions — cycling guarantees disjoint slots so no UAB needed.
        {
            VkDescriptorSetLayoutBinding bindings[7]{};
            for (u32 i = 0; i < 7; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          // volDensity
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          // volInScatter
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // LightSSBO
            bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // ClusterGrid
            bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // LightIndex
            bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // FogVolumeSSBO
            bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // shadowMap

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.bindingCount = 7;
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
    }

    void VolumetricSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_InjectPipeline.reset();
        if (m_InjectDescLayout) vkDestroyDescriptorSetLayout(device, m_InjectDescLayout, nullptr);
        if (m_Sampler)          vkDestroySampler(device, m_Sampler, nullptr);
        m_InjectDescLayout = VK_NULL_HANDLE;
        m_Sampler          = VK_NULL_HANDLE;
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
                std::vector<VkDescriptorSetLayout>{ m_InjectDescLayout },
                std::vector<VkPushConstantRange>{ pc });
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
        if (m_InjectDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volDensity || !vr.volInScatter)   return;

        VkDevice device  = VulkanContext::Get().GetDevice();
        auto& lighting   = m_Pipeline->GetLighting();
        auto vkDens      = std::static_pointer_cast<VKTexture>(vr.volDensity);
        auto vkScat      = std::static_pointer_cast<VKTexture>(vr.volInScatter);

        VkDescriptorImageInfo densInfo{};
        densInfo.imageView   = vkDens->GetImageView();
        densInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo scatInfo{};
        scatInfo.imageView   = vkScat->GetImageView();
        scatInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // CSM array view + compare sampler (PCF less). Same for every slot — written here so the
        // per-frame WriteInjectPerFrame doesn't have to touch b6. Shadow map's image lives on
        // LightingSubsystem; sampler too.
        VkDescriptorImageInfo shadowInfo{};
        if (auto shadowTex = lighting.GetShadowMap())
        {
            shadowInfo.imageView   = std::static_pointer_cast<VKTexture>(shadowTex)->GetImageView();
            shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            shadowInfo.sampler     = lighting.GetShadowSampler();
        }

        constexpr u32 kStableCount = 3;  // b0, b1, b6
        VkWriteDescriptorSet writes[kStableCount * MAX_FRAMES_IN_FLIGHT]{};
        u32 w = 0;
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSet set = vr.volInjectDescSet[i];
            if (set == VK_NULL_HANDLE) continue;

            writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[w].dstSet          = set;
            writes[w].dstBinding      = 0;
            writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[w].descriptorCount = 1;
            writes[w].pImageInfo      = &densInfo;
            ++w;

            writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[w].dstSet          = set;
            writes[w].dstBinding      = 1;
            writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[w].descriptorCount = 1;
            writes[w].pImageInfo      = &scatInfo;
            ++w;

            if (shadowInfo.sampler)
            {
                writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[w].dstSet          = set;
                writes[w].dstBinding      = 6;
                writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[w].descriptorCount = 1;
                writes[w].pImageInfo      = &shadowInfo;
                ++w;
            }
        }
        vkUpdateDescriptorSets(device, w, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteInjectPerFrame(const Memory::GPUSubRegion& lightSSBORegion,
                                                  const Memory::GPUSubRegion& clusterGridRegion,
                                                  const Memory::GPUSubRegion& lightIndexRegion,
                                                  const Memory::GPUSubRegion& fogVolumeRegion)
    {
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;

        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!vr || vr->volInjectDescSet[slot] == VK_NULL_HANDLE) return;
        if (!lightSSBORegion.buffer || !clusterGridRegion.buffer ||
            !lightIndexRegion.buffer || !fogVolumeRegion.buffer)
            return;

        VkDescriptorBufferInfo lightBi { lightSSBORegion.buffer,   lightSSBORegion.offset,   lightSSBORegion.size   };
        VkDescriptorBufferInfo gridBi  { clusterGridRegion.buffer, clusterGridRegion.offset, clusterGridRegion.size };
        VkDescriptorBufferInfo indexBi { lightIndexRegion.buffer,  lightIndexRegion.offset,  lightIndexRegion.size  };
        VkDescriptorBufferInfo fogBi   { fogVolumeRegion.buffer,   fogVolumeRegion.offset,   fogVolumeRegion.size   };

        VkWriteDescriptorSet writes[4]{};
        const VkDescriptorBufferInfo* infos[4] = { &lightBi, &gridBi, &indexBi, &fogBi };
        for (u32 i = 0; i < 4; ++i)
        {
            writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[i].dstSet          = vr->volInjectDescSet[slot];
            writes[i].dstBinding      = 2 + i;  // bindings 2..5
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo     = infos[i];
        }
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 4, writes, 0, nullptr);
    }

    void VolumetricSubsystem::AddInjectPass(RG::RenderGraph& rg)
    {
        struct InjectData
        {
            RG::ResourceHandle density;
            RG::ResourceHandle inScatter;
        };

        rg.AddComputePass<InjectData>("VolumetricInject", RG::QueueFamily::AsyncCompute,
            [&](InjectData& data, RG::RenderPassBuilder& builder)
            {
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                RG::TextureDesc descD;
                descD.name   = "VolDensity";
                descD.width  = k_VolDimX;
                descD.height = k_VolDimY;
                descD.format = RG::TextureFormat::RGBA16_Float;
                auto vkDens  = std::static_pointer_cast<VKTexture>(vr->volDensity);
                data.density = rg.ImportResource(descD,
                    (void*)vkDens->GetImage(), (void*)vkDens->GetImageView(),
                    RG::ResourceState::Undefined);
                data.density = builder.WriteStorageImage(data.density);

                RG::TextureDesc descS;
                descS.name   = "VolInScatter";
                descS.width  = k_VolDimX;
                descS.height = k_VolDimY;
                descS.format = RG::TextureFormat::RGBA16_Float;
                auto vkScat    = std::static_pointer_cast<VKTexture>(vr->volInScatter);
                data.inScatter = rg.ImportResource(descS,
                    (void*)vkScat->GetImage(), (void*)vkScat->GetImageView(),
                    RG::ResourceState::Undefined);
                data.inScatter = builder.WriteStorageImage(data.inScatter);
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
                vkCmdPushConstants(cmd, m_InjectPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(InjectPC), &pc);

                const u32 groupX = (k_VolDimX + 7) / 8;
                const u32 groupY = (k_VolDimY + 7) / 8;
                const u32 groupZ = (k_VolDimZ + 3) / 4;
                vkCmdDispatch(cmd, groupX, groupY, groupZ);

                sys.GetFrameDebugger().CaptureComputeDispatch("VolumetricInject",
                    "volumetric_inject", groupX, groupY, groupZ);
                sys.GetFrameDebugger().EndCapturePass();
            });
    }
}
