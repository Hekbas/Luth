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

        struct ResolvePC
        {
            Mat4 invView;             // 64 B — current frame's view-space → world reconstruction.
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

        // Inject layout (Set 1): b0/b1 storage images (density + in-scatter scratch), b2-b5 SSBOs
        // (LightSSBO, ClusterGrid, LightIndex, FogVolume), b6 shadow sampler, b7 3D noise sampler
        // (static Worley-FBM, baked once at Init). b0/b1/b6/b7 are stable per-view; b2-b5 rewrite
        // per frame against fresh tagged-heap regions.
        {
            VkDescriptorSetLayoutBinding bindings[8]{};
            for (u32 i = 0; i < 8; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          // volDensity
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          // volInScatter (scratch)
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // LightSSBO
            bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // ClusterGrid
            bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // LightIndex
            bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // FogVolumeSSBO
            bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // shadowMap
            bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // volNoise3D

            VkDescriptorBindingFlags bindingFlags[8] = {
                0, 0,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                0, 0,
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

        // Integrate layout — b0 sampled volDensity (sampler3D, RG ReadStorageImage transitions to
        // SHADER_READ_ONLY), b1 read+write volInScatter scratch (storage, GENERAL). Stable bindings.
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

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
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

        // Resolve layout — b0 scratch sampler (this frame's post-integrate), b1 prev resolved
        // sampler (reprojected history source), b2 curr resolved storage (write). b1/b2 parity-
        // rewrite per frame to ping-pong HistA/B. Push constant: invView (64B) + atlas dims (16B).
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            for (u32 i = 0; i < 3; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // volInScatter scratch
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // prev resolved
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          // curr resolved (write)

            VkDescriptorBindingFlags bindingFlags[3] = {
                0,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 3;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.pNext        = &bindingFlagsCI;
            layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutCI.bindingCount = 3;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_ResolveDescLayout);

            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolvePC) };

            if (auto sh = ShaderLibrary::LoadEngine("shaders/volumetric_resolve.comp"))
                m_ResolveSpv = sh->GetSpirV();
            if (m_ResolveSpv.empty())
            {
                LH_CORE_ERROR("VolumetricSubsystem: failed to load volumetric_resolve.comp!");
                return;
            }
            // Set 0 = GlobalUniforms (prevViewProjection + prevViewParams + temporalAlpha).
            m_ResolvePipeline = std::make_unique<VKComputePipeline>(
                m_ResolveSpv,
                std::vector<VkDescriptorSetLayout>{
                    pipeline.GetGlobal().GetSetLayout(),
                    m_ResolveDescLayout,
                },
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

            VkPushConstantRange compPC{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Mat4) };

            PipelineConfig cfg{};
            cfg.colorFormats       = { VK_FORMAT_R16G16B16A16_SFLOAT };  // matches SceneColor
            cfg.depthFormat        = VK_FORMAT_UNDEFINED;
            cfg.depthTest          = false;
            cfg.depthWrite         = false;
            cfg.blendEnabled       = true;                               // standard alpha — shader emits (fogColor, fogOpacity)
            cfg.cullMode           = VK_CULL_MODE_NONE;
            cfg.pushConstantRanges = { compPC };
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

        // 3D noise bake — one-shot compute dispatch that fills m_NoiseTexture with Worley-FBM at
        // engine init. All ephemeral state (pool / layout / pipeline) lives inside this scope and
        // is destroyed at end. The texture itself outlives Init and is sampled by inject's b7.
        {
            constexpr u32 k_NoiseDim = 128;
            m_NoiseTexture = std::make_shared<VKTexture>(
                k_NoiseDim, k_NoiseDim, k_NoiseDim, TextureFormat::RGBA8, VK_IMAGE_USAGE_STORAGE_BIT);

            // Sampler — LINEAR + REPEAT for tileable noise. m_Sampler is CLAMP_TO_EDGE so it's not
            // reusable here.
            VkSamplerCreateInfo nsCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            nsCI.magFilter    = VK_FILTER_LINEAR;
            nsCI.minFilter    = VK_FILTER_LINEAR;
            nsCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            nsCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            nsCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            nsCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &nsCI, nullptr, &m_NoiseSampler);

            // Bake pipeline + descriptor — ephemeral, destroyed at end of this block.
            std::vector<u32> bakeSpv;
            if (auto sh = ShaderLibrary::LoadEngine("shaders/volumetric_noise_bake.comp"))
                bakeSpv = sh->GetSpirV();
            if (bakeSpv.empty())
            {
                LH_CORE_ERROR("VolumetricSubsystem: failed to load volumetric_noise_bake.comp!");
                return;
            }

            VkDescriptorSetLayoutBinding binding{};
            binding.binding         = 0;
            binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            binding.descriptorCount = 1;
            binding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo bakeLayoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            bakeLayoutCI.bindingCount = 1;
            bakeLayoutCI.pBindings    = &binding;
            VkDescriptorSetLayout bakeLayout = VK_NULL_HANDLE;
            vkCreateDescriptorSetLayout(device, &bakeLayoutCI, nullptr, &bakeLayout);

            VkDescriptorPoolSize bakePoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 };
            VkDescriptorPoolCreateInfo bakePoolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            bakePoolCI.maxSets       = 1;
            bakePoolCI.poolSizeCount = 1;
            bakePoolCI.pPoolSizes    = &bakePoolSize;
            VkDescriptorPool bakePool = VK_NULL_HANDLE;
            vkCreateDescriptorPool(device, &bakePoolCI, nullptr, &bakePool);

            VkDescriptorSet bakeSet = VK_NULL_HANDLE;
            VkDescriptorSetAllocateInfo bakeAI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            bakeAI.descriptorPool     = bakePool;
            bakeAI.descriptorSetCount = 1;
            bakeAI.pSetLayouts        = &bakeLayout;
            vkAllocateDescriptorSets(device, &bakeAI, &bakeSet);

            VkPushConstantRange bakePC{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32) };
            auto bakePipeline = std::make_unique<VKComputePipeline>(
                bakeSpv,
                std::vector<VkDescriptorSetLayout>{ bakeLayout },
                std::vector<VkPushConstantRange>{ bakePC });

            auto vkNoise = std::static_pointer_cast<VKTexture>(m_NoiseTexture);
            VkDescriptorImageInfo noiseStoreInfo{};
            noiseStoreInfo.imageView   = vkNoise->GetImageView();
            noiseStoreInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet bakeWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            bakeWrite.dstSet          = bakeSet;
            bakeWrite.dstBinding      = 0;
            bakeWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bakeWrite.descriptorCount = 1;
            bakeWrite.pImageInfo      = &noiseStoreInfo;
            vkUpdateDescriptorSets(device, 1, &bakeWrite, 0, nullptr);

            VkImage noiseImg = vkNoise->GetImage();
            VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd)
            {
                VkImageMemoryBarrier toGen{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                toGen.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                toGen.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
                toGen.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGen.image               = noiseImg;
                toGen.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                toGen.srcAccessMask       = 0;
                toGen.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &toGen);

                bakePipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    bakePipeline->GetLayout(), 0, 1, &bakeSet, 0, nullptr);
                u32 dim = k_NoiseDim;
                vkCmdPushConstants(cmd, bakePipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32), &dim);
                const u32 groups = (k_NoiseDim + 3) / 4;
                vkCmdDispatch(cmd, groups, groups, groups);

                VkImageMemoryBarrier toShader = toGen;
                toShader.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
                toShader.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toShader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &toShader);
            });

            // Tear down the bake-only state. The texture + sampler stay alive on the subsystem.
            bakePipeline.reset();
            vkDestroyDescriptorPool(device, bakePool, nullptr);
            vkDestroyDescriptorSetLayout(device, bakeLayout, nullptr);
        }
    }

    void VolumetricSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_InjectPipeline.reset();
        m_IntegratePipeline.reset();
        m_ResolvePipeline.reset();
        m_CompositePipeline.reset();
        m_VizPipeline.reset();
        if (m_InjectDescLayout)    vkDestroyDescriptorSetLayout(device, m_InjectDescLayout, nullptr);
        if (m_IntegrateDescLayout) vkDestroyDescriptorSetLayout(device, m_IntegrateDescLayout, nullptr);
        if (m_ResolveDescLayout)   vkDestroyDescriptorSetLayout(device, m_ResolveDescLayout, nullptr);
        if (m_CompositeDescLayout) vkDestroyDescriptorSetLayout(device, m_CompositeDescLayout, nullptr);
        if (m_VizDescLayout)       vkDestroyDescriptorSetLayout(device, m_VizDescLayout, nullptr);
        if (m_Sampler)             vkDestroySampler(device, m_Sampler, nullptr);
        if (m_NoiseSampler)        vkDestroySampler(device, m_NoiseSampler, nullptr);
        m_NoiseTexture.reset();
        m_InjectDescLayout    = VK_NULL_HANDLE;
        m_IntegrateDescLayout = VK_NULL_HANDLE;
        m_ResolveDescLayout   = VK_NULL_HANDLE;
        m_CompositeDescLayout = VK_NULL_HANDLE;
        m_VizDescLayout       = VK_NULL_HANDLE;
        m_Sampler             = VK_NULL_HANDLE;
        m_NoiseSampler        = VK_NULL_HANDLE;
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
        if (name == "volumetric_resolve.comp" && m_ResolveDescLayout)
        {
            m_ResolveSpv = spv;
            deferComp(m_ResolvePipeline);
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolvePC) };
            m_ResolvePipeline = std::make_unique<VKComputePipeline>(m_ResolveSpv,
                std::vector<VkDescriptorSetLayout>{
                    m_Pipeline->GetGlobal().GetSetLayout(),
                    m_ResolveDescLayout,
                },
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
            VkPushConstantRange compPC{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Mat4) };
            PipelineConfig cfg{};
            cfg.colorFormats       = { VK_FORMAT_R16G16B16A16_SFLOAT };
            cfg.depthFormat        = VK_FORMAT_UNDEFINED;
            cfg.depthTest          = false;
            cfg.depthWrite         = false;
            cfg.blendEnabled       = true;
            cfg.cullMode           = VK_CULL_MODE_NONE;
            cfg.pushConstantRanges = { compPC };
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
        // Stable across frames: b0 (volDensity), b1 (volInScatter scratch), b6 (shadow sampler),
        // b7 (3D noise sampler). SSBO bindings b2-b5 rewrite per frame in WriteInjectPerFrame.
        if (m_InjectDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volDensity || !vr.volInScatter) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        auto& lighting  = m_Pipeline->GetLighting();

        auto vkDens = std::static_pointer_cast<VKTexture>(vr.volDensity);
        auto vkScat = std::static_pointer_cast<VKTexture>(vr.volInScatter);

        VkDescriptorImageInfo densInfo{};
        densInfo.imageView   = vkDens->GetImageView();
        densInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo scatInfo{};
        scatInfo.imageView   = vkScat->GetImageView();
        scatInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo shadowInfo{};
        if (auto shadowTex = lighting.GetShadowMap())
        {
            shadowInfo.imageView   = std::static_pointer_cast<VKTexture>(shadowTex)->GetImageView();
            shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            shadowInfo.sampler     = lighting.GetShadowSampler();
        }

        VkDescriptorImageInfo noiseInfo{};
        if (m_NoiseTexture && m_NoiseSampler)
        {
            noiseInfo.imageView   = std::static_pointer_cast<VKTexture>(m_NoiseTexture)->GetImageView();
            noiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            noiseInfo.sampler     = m_NoiseSampler;
        }

        VkWriteDescriptorSet writes[4 * MAX_FRAMES_IN_FLIGHT]{};
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

            if (noiseInfo.sampler)
            {
                writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[w].dstSet          = set;
                writes[w].dstBinding      = 7;
                writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[w].descriptorCount = 1;
                writes[w].pImageInfo      = &noiseInfo;
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
            writes[i].dstBinding      = 2 + i;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo     = infos[i];
        }
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 4, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteIntegrateView(ViewResources& vr)
    {
        // Both b0 (density sampler) and b1 (in-scatter storage R/W = scratch atlas) are stable.
        // Integrate works in-place over volInScatter every frame.
        if (m_IntegrateDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volDensity || !vr.volInScatter) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        auto vkDens = std::static_pointer_cast<VKTexture>(vr.volDensity);
        auto vkScat = std::static_pointer_cast<VKTexture>(vr.volInScatter);

        // RG ReadStorageImage transitions volDensity to SHADER_READ_ONLY_OPTIMAL. m_Sampler is
        // unused by texelFetch but Vulkan requires a valid sampler in the descriptor.
        VkDescriptorImageInfo densInfo{};
        densInfo.imageView   = vkDens->GetImageView();
        densInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        densInfo.sampler     = m_Sampler;
        VkDescriptorImageInfo scatInfo{};
        scatInfo.imageView   = vkScat->GetImageView();
        scatInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2 * MAX_FRAMES_IN_FLIGHT]{};
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
            writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[w].dstSet          = set;
            writes[w].dstBinding      = 1;
            writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[w].descriptorCount = 1;
            writes[w].pImageInfo      = &scatInfo;
            ++w;
        }
        vkUpdateDescriptorSets(device, w, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteResolveView(ViewResources& vr)
    {
        // Only b0 (scratch sampler) is stable. b1 (prev resolved sampler) + b2 (curr resolved
        // storage) parity-rewrite per frame to ping-pong HistA / HistB.
        if (m_ResolveDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volInScatter) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        auto vkScratch = std::static_pointer_cast<VKTexture>(vr.volInScatter);

        VkDescriptorImageInfo scratchInfo{};
        scratchInfo.imageView   = vkScratch->GetImageView();
        scratchInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        scratchInfo.sampler     = m_Sampler;

        VkWriteDescriptorSet writes[MAX_FRAMES_IN_FLIGHT]{};
        u32 w = 0;
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSet set = vr.volResolveDescSet[i];
            if (set == VK_NULL_HANDLE) continue;
            writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[w].dstSet          = set;
            writes[w].dstBinding      = 0;
            writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[w].descriptorCount = 1;
            writes[w].pImageInfo      = &scratchInfo;
            ++w;
        }
        vkUpdateDescriptorSets(device, w, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteResolvePerFrame(ViewResources& vr, u32 frameAbs)
    {
        if (m_ResolveDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volInScatterHistA || !vr.volInScatterHistB) return;

        const u32 slot    = frameAbs % MAX_FRAMES_IN_FLIGHT;
        const bool parity = (frameAbs & 1u) != 0u;
        if (vr.volResolveDescSet[slot] == VK_NULL_HANDLE) return;

        // parity=0: read HistA as prev, write HistB as curr. parity=1: swap.
        auto vkPrev = std::static_pointer_cast<VKTexture>(
            parity ? vr.volInScatterHistB : vr.volInScatterHistA);
        auto vkCurr = std::static_pointer_cast<VKTexture>(
            parity ? vr.volInScatterHistA : vr.volInScatterHistB);

        VkDescriptorImageInfo prevInfo{};
        prevInfo.imageView   = vkPrev->GetImageView();
        prevInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        prevInfo.sampler     = m_Sampler;

        VkDescriptorImageInfo currInfo{};
        currInfo.imageView   = vkCurr->GetImageView();
        currInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr.volResolveDescSet[slot];
        writes[0].dstBinding      = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &prevInfo;
        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = vr.volResolveDescSet[slot];
        writes[1].dstBinding      = 2;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &currInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 2, writes, 0, nullptr);
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
        if (!vr.volInScatterHistA || !vr.volInScatterHistB) return;

        const u32 slot    = frameAbs % MAX_FRAMES_IN_FLIGHT;
        const bool parity = (frameAbs & 1u) != 0u;
        if (vr.volCompositeDescSet[slot] == VK_NULL_HANDLE) return;

        // Sample the resolve pass's curr-frame output (same parity rule as resolve's b2 write).
        auto vkScat = std::static_pointer_cast<VKTexture>(
            parity ? vr.volInScatterHistA : vr.volInScatterHistB);

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
                                                              RG::ResourceHandle resolvedInScatter)
    {
        if (!m_CompositePipeline) return sceneColor;

        struct CompositeData {
            RG::ResourceHandle color;
            RG::ResourceHandle depth;
            RG::ResourceHandle inScatter;
        };
        RG::ResourceHandle outputHandle;

        rg.AddPass<CompositeData>("VolumetricComposite",
            [&, sceneColor, sceneDepth, resolvedInScatter](CompositeData& data, RG::RenderPassBuilder& builder)
            {
                data.color = builder.Write(sceneColor,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                data.depth = builder.Read(sceneDepth);
                // Sampler-binding 1 of the composite descriptor — declaring the read makes RG emit
                // the GENERAL → SHADER_READ_ONLY transition after resolve's storage write.
                if (resolvedInScatter.IsValid())
                    data.inScatter = builder.Read(resolvedInScatter);
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

                // invView push constant avoids per-fragment inverse(ubo.view) at full-screen.
                Mat4 invView = Math::Inverse(view->camera.view);
                vkCmdPushConstants(cmd, m_CompositePipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Mat4), &invView);

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

    RG::ResourceHandle VolumetricSubsystem::AddResolvePass(RG::RenderGraph& rg,
                                                           RG::ResourceHandle scratchInScatter)
    {
        struct ResolveData
        {
            RG::ResourceHandle scratch;   // reads post-integrate this frame
            RG::ResourceHandle resolved;  // writes blended-with-prev result
        };
        RG::ResourceHandle outputHandle;

        rg.AddComputePass<ResolveData>("VolumetricResolve", RG::QueueFamily::AsyncCompute,
            [&, this, scratchInScatter](ResolveData& data, RG::RenderPassBuilder& builder)
            {
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const bool parity  = (frameAbs & 1u) != 0u;

                // Scratch comes from integrate's output — same ResourceNode (arch hazard #1 OK).
                data.scratch = builder.ReadStorageImage(scratchInScatter);

                // History ping-pong — two distinct VkImages, two distinct nodes per frame. Prev
                // history sampled at reprojected coord; curr history written at current voxel.
                // Separate physical atlases keep the read + write hazard-free.
                auto vkCurr = std::static_pointer_cast<VKTexture>(
                    parity ? vr->volInScatterHistA : vr->volInScatterHistB);

                RG::TextureDesc desc;
                desc.name   = parity ? "VolInScatterHistA[curr]" : "VolInScatterHistB[curr]";
                desc.width  = vr->volDimX;
                desc.height = vr->volDimY;
                desc.format = RG::TextureFormat::RGBA16_Float;
                data.resolved = rg.ImportResource(desc,
                    (void*)vkCurr->GetImage(), (void*)vkCurr->GetImageView(),
                    RG::ResourceState::Undefined);
                data.resolved = builder.WriteStorageImage(data.resolved);
                outputHandle = data.resolved;
            },
            [this](ResolveData& /*data*/, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "VolumetricResolve",
                    "VolInScatterHistA", false,
                    { "volumetric_resolve", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;

                if (!m_ResolvePipeline || vr->volResolveDescSet[slot] == VK_NULL_HANDLE)
                {
                    sys.GetFrameDebugger().EndCapturePass();
                    return;
                }

                m_ResolvePipeline->Bind(cmd);
                VkDescriptorSet sets[2] = {
                    vr->globalDescriptorSet[slot],
                    vr->volResolveDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_ResolvePipeline->GetLayout(), 0, 2, sets, 0, nullptr);

                ResolvePC pc{};
                pc.invView = Math::Inverse(m_Pipeline->GetCurrentView()->camera.view);
                pc.volDimX = vr->volDimX; pc.volDimY = vr->volDimY; pc.volDimZ = vr->volDimZ;
                vkCmdPushConstants(cmd, m_ResolvePipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolvePC), &pc);

                const u32 groupX = (vr->volDimX + 7) / 8;
                const u32 groupY = (vr->volDimY + 7) / 8;
                const u32 groupZ = (vr->volDimZ + 3) / 4;
                vkCmdDispatch(cmd, groupX, groupY, groupZ);

                sys.GetFrameDebugger().CaptureComputeDispatch("VolumetricResolve",
                    "volumetric_resolve", groupX, groupY, groupZ);
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
            RG::ResourceHandle shadowCascades[k_ShadowCascadeCount];
        };
        InjectOutputs output{};

        rg.AddComputePass<InjectData>("VolumetricInject", RG::QueueFamily::AsyncCompute,
            [&, this](InjectData& data, RG::RenderPassBuilder& builder)
            {
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

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

                // Single-atlas scratch write — temporal accumulation moved to the resolve pass so
                // inject no longer needs ping-pong or history reads.
                RG::TextureDesc descS;
                descS.name   = "VolInScatter";
                descS.width  = vr->volDimX;
                descS.height = vr->volDimY;
                descS.format = RG::TextureFormat::RGBA16_Float;
                auto vkScat    = std::static_pointer_cast<VKTexture>(vr->volInScatter);
                data.inScatter = rg.ImportResource(descS,
                    (void*)vkScat->GetImage(), (void*)vkScat->GetImageView(),
                    RG::ResourceState::Undefined);
                data.inScatter = builder.WriteStorageImage(data.inScatter);

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
        if (!vr.volInScatterHistA || !vr.volInScatterHistB) return;

        const u32 slot    = frameAbs % MAX_FRAMES_IN_FLIGHT;
        const bool parity = (frameAbs & 1u) != 0u;
        if (vr.volVizDescSet[slot] == VK_NULL_HANDLE) return;

        // Same parity rule as composite: sample the resolved atlas this frame.
        auto vkScat = std::static_pointer_cast<VKTexture>(
            parity ? vr.volInScatterHistA : vr.volInScatterHistB);

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

                // Pull live tunables from VolumetricSettings — Render panel exposes both scales.
                const auto& vs = sys.GetVolumetricSettings();
                struct VizPC { u32 mode; f32 scale; f32 overlayAlpha; } pc{};
                pc.mode         = mode;
                pc.scale        = (mode == 0u) ? vs.vizScaleDensity : vs.vizScaleInScatter;
                pc.overlayAlpha = vs.vizOpacity;
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
