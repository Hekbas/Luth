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
            Vec4 dirLightDirIntensity;
            Vec4 dirLightColor;
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

        // Inject layout: 2 storage images (volDensity + volInScatter).
        {
            VkDescriptorSetLayoutBinding bindings[2]{};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.bindingCount = 2;
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
            m_InjectPipeline = std::make_unique<VKComputePipeline>(
                m_InjectSpv,
                std::vector<VkDescriptorSetLayout>{ m_InjectDescLayout },
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

        VkDevice device = VulkanContext::Get().GetDevice();
        auto vkDens = std::static_pointer_cast<VKTexture>(vr.volDensity);
        auto vkScat = std::static_pointer_cast<VKTexture>(vr.volInScatter);

        VkDescriptorImageInfo densInfo{};
        densInfo.imageView   = vkDens->GetImageView();
        densInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo scatInfo{};
        scatInfo.imageView   = vkScat->GetImageView();
        scatInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // A4.7 shell: same write replicated across all cycled slots. Temporal ping-pong (A4.10)
        // will start differentiating slots by frame parity (current vs history atlas).
        VkWriteDescriptorSet writes[2 * MAX_FRAMES_IN_FLIGHT]{};
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSet set = vr.volInjectDescSet[i];
            if (set == VK_NULL_HANDLE) continue;

            writes[i * 2 + 0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i * 2 + 0].dstSet          = set;
            writes[i * 2 + 0].dstBinding      = 0;
            writes[i * 2 + 0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i * 2 + 0].descriptorCount = 1;
            writes[i * 2 + 0].pImageInfo      = &densInfo;

            writes[i * 2 + 1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i * 2 + 1].dstSet          = set;
            writes[i * 2 + 1].dstBinding      = 1;
            writes[i * 2 + 1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i * 2 + 1].descriptorCount = 1;
            writes[i * 2 + 1].pImageInfo      = &scatInfo;
        }
        vkUpdateDescriptorSets(device, 2 * MAX_FRAMES_IN_FLIGHT, writes, 0, nullptr);
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
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_InjectPipeline->GetLayout(), 0, 1, &vr->volInjectDescSet[slot], 0, nullptr);

                // Sticky dir-light snapshot. LightingSystem's gather mirrors directional state into
                // GatheredLights even when no Component::DirectionalLight exists this frame, so the
                // shell always has a sensible (color/intensity/direction).
                InjectPC pc{};
                if (auto* lighting = SystemRegistry::GetSystem<LightingSystem>())
                {
                    const auto& dl = lighting->GetLights().dirLight;
                    pc.dirLightDirIntensity = Vec4(-dl.direction, dl.intensity);
                    pc.dirLightColor        = Vec4(dl.color, 0.0f);
                }
                else
                {
                    pc.dirLightDirIntensity = Vec4(0.0f, 1.0f, 0.0f, 1.0f);
                    pc.dirLightColor        = Vec4(1.0f);
                }
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
