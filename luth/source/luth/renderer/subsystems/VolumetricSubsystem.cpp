#include "luthpch.h"
#include "luth/renderer/subsystems/VolumetricSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/lighting/FogVolumeGatherer.h"
#include "luth/renderer/material/MaterialSystem.h"
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
            u64  geomTableBDA;        // 8 B — scatter-only cutout alpha-test fetch; density ignores it.
        };
        static_assert(sizeof(InjectPC) == 88, "InjectPC: invView(64) + dims(16) + geomTableBDA(8)");

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

        // Inject density layout (Set 1): b0 volDensity storage (write), b1 FogVolume SSBO,
        // b2 volNoise3D sampler. b0/b2 stable per-view; b1 rewrites per-frame.
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            for (u32 i = 0; i < 3; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          // volDensity
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // FogVolumeSSBO
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // volNoise3D

            VkDescriptorBindingFlags bindingFlags[3] = {
                0,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                0,
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
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_InjectDensityDescLayout);

            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(InjectPC) };

            if (auto sh = ShaderLibrary::LoadEngine("shaders/volumetric_inject_density.comp"))
                m_InjectDensitySpv = sh->GetSpirV();
            if (m_InjectDensitySpv.empty())
            {
                LH_CORE_ERROR("VolumetricSubsystem: failed to load volumetric_inject_density.comp!");
                return;
            }
            // Pipeline layout: Set 0 = GlobalSubsystem's, Set 1 = density-only inject state.
            m_InjectDensityPipeline = std::make_unique<VKComputePipeline>(
                m_InjectDensitySpv,
                std::vector<VkDescriptorSetLayout>{
                    pipeline.GetGlobal().GetSetLayout(),
                    m_InjectDensityDescLayout,
                },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // Inject scatter layout (Set 1): b0 volDensity sampler3D (read; density.r + tint.gba),
        // b1 volInScatter storage (write), b2-b4 SSBOs (Light, ClusterGrid, LightIndex), b5 shadow
        // array sampler. b0/b1/b5 stable per-view; b2-b4 rewrite per-frame.
        {
            VkDescriptorSetLayoutBinding bindings[6]{};
            for (u32 i = 0; i < 6; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // volDensity sampler3D
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          // volInScatter (write)
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // LightSSBO
            bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // ClusterGrid
            bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         // LightIndex
            bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // shadowMap

            VkDescriptorBindingFlags bindingFlags[6] = {
                0, 0,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                0,
            };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 6;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.pNext        = &bindingFlagsCI;
            layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutCI.bindingCount = 6;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_InjectScatterDescLayout);

            // Empty Set 2 placeholder — geom_table.glsl pins Material to Set 3 + bindless to Set 4, but the
            // scatter pipeline has no pass-local Set 2. A 0-binding layout fills the gap; it's never bound.
            VkDescriptorSetLayoutCreateInfo emptyCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            vkCreateDescriptorSetLayout(device, &emptyCI, nullptr, &m_EmptySet2Layout);

            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(InjectPC) };

            if (auto sh = ShaderLibrary::LoadEngine("shaders/volumetric_inject_scatter.comp"))
                m_InjectScatterSpv = sh->GetSpirV();
            if (m_InjectScatterSpv.empty())
            {
                LH_CORE_ERROR("VolumetricSubsystem: failed to load volumetric_inject_scatter.comp!");
                return;
            }
            // Cutout 5-set layout: Set 3 Material + Set 4 bindless feed geom_table's RT alpha-test.
            m_InjectScatterPipeline = std::make_unique<VKComputePipeline>(
                m_InjectScatterSpv,
                std::vector<VkDescriptorSetLayout>{
                    pipeline.GetGlobal().GetSetLayout(),
                    m_InjectScatterDescLayout,
                    m_EmptySet2Layout,
                    MaterialSystem::GetDescriptorSetLayout(),
                    VulkanContext::Get().GetBindlessSet().GetLayout(),
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

        // Composite layout (Set 1) — b0 sceneDepth, b1 volInScatter (sampler3D), b2 blueNoise.
        // All FRAGMENT. b1 parity-rewrites each frame to sample whichever atlas integrate wrote to —
        // UAB needed. b2 is stable per-view (blue noise texture never changes after bake).
        // Descriptor set is cycled per MAX_FRAMES_IN_FLIGHT to keep rewrites disjoint from
        // in-flight prior frame's reads.
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            for (u32 i = 0; i < 3; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorBindingFlags bindingFlags[3] = { 0, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0 };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 3;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.pNext        = &bindingFlagsCI;
            layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutCI.bindingCount = 3;
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

        // 3D Worley-FBM noise bake (one-shot at init). Pool/layout/pipeline are scoped — the
        // texture outlives Init and is sampled by inject density's b2.
        {
            constexpr u32 k_NoiseDim = 128;
            m_NoiseTexture = std::make_shared<VKTexture>(
                k_NoiseDim, k_NoiseDim, k_NoiseDim, TextureFormat::RGBA8, VK_IMAGE_USAGE_STORAGE_BIT);

            // Tileable noise needs REPEAT; m_Sampler is CLAMP_TO_EDGE so we own a dedicated one.
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

            bakePipeline.reset();
            vkDestroyDescriptorPool(device, bakePool, nullptr);
            vkDestroyDescriptorSetLayout(device, bakeLayout, nullptr);
        }

        // 2D blue-noise dither bake (Roberts R2 quasi-random). Rationale in the header next to
        // m_BlueNoise2D; the volumetric composite consumes it via NEAREST+REPEAT sampler.
        {
            constexpr u32 k_BlueNoiseDim = 64;
            m_BlueNoise2D = std::make_shared<VKTexture>(
                k_BlueNoiseDim, k_BlueNoiseDim, TextureFormat::R8,
                /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
                VK_IMAGE_USAGE_STORAGE_BIT);

            VkSamplerCreateInfo bsCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            bsCI.magFilter    = VK_FILTER_NEAREST;
            bsCI.minFilter    = VK_FILTER_NEAREST;
            bsCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            bsCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            bsCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            bsCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &bsCI, nullptr, &m_BlueNoiseSampler);

            std::vector<u32> bakeSpv;
            if (auto sh = ShaderLibrary::LoadEngine("shaders/blue_noise_bake.comp"))
                bakeSpv = sh->GetSpirV();
            if (bakeSpv.empty())
            {
                LH_CORE_ERROR("VolumetricSubsystem: failed to load blue_noise_bake.comp!");
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

            auto vkBlue = std::static_pointer_cast<VKTexture>(m_BlueNoise2D);
            VkDescriptorImageInfo blueStoreInfo{};
            blueStoreInfo.imageView   = vkBlue->GetImageView();
            blueStoreInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet bakeWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            bakeWrite.dstSet          = bakeSet;
            bakeWrite.dstBinding      = 0;
            bakeWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bakeWrite.descriptorCount = 1;
            bakeWrite.pImageInfo      = &blueStoreInfo;
            vkUpdateDescriptorSets(device, 1, &bakeWrite, 0, nullptr);

            VkImage blueImg = vkBlue->GetImage();
            VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd)
            {
                VkImageMemoryBarrier toGen{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                toGen.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                toGen.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
                toGen.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGen.image               = blueImg;
                toGen.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                toGen.srcAccessMask       = 0;
                toGen.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &toGen);

                bakePipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    bakePipeline->GetLayout(), 0, 1, &bakeSet, 0, nullptr);
                u32 dim = k_BlueNoiseDim;
                vkCmdPushConstants(cmd, bakePipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32), &dim);
                const u32 groups = (k_BlueNoiseDim + 7) / 8;
                vkCmdDispatch(cmd, groups, groups, 1);

                VkImageMemoryBarrier toShader = toGen;
                toShader.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
                toShader.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toShader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &toShader);
            });

            bakePipeline.reset();
            vkDestroyDescriptorPool(device, bakePool, nullptr);
            vkDestroyDescriptorSetLayout(device, bakeLayout, nullptr);
        }
    }

    void VolumetricSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_InjectDensityPipeline.reset();
        m_InjectScatterPipeline.reset();
        m_IntegratePipeline.reset();
        m_ResolvePipeline.reset();
        m_CompositePipeline.reset();
        m_VizPipeline.reset();
        if (m_InjectDensityDescLayout) vkDestroyDescriptorSetLayout(device, m_InjectDensityDescLayout, nullptr);
        if (m_InjectScatterDescLayout) vkDestroyDescriptorSetLayout(device, m_InjectScatterDescLayout, nullptr);
        if (m_EmptySet2Layout)         vkDestroyDescriptorSetLayout(device, m_EmptySet2Layout, nullptr);
        if (m_IntegrateDescLayout)     vkDestroyDescriptorSetLayout(device, m_IntegrateDescLayout, nullptr);
        if (m_ResolveDescLayout)       vkDestroyDescriptorSetLayout(device, m_ResolveDescLayout, nullptr);
        if (m_CompositeDescLayout)     vkDestroyDescriptorSetLayout(device, m_CompositeDescLayout, nullptr);
        if (m_VizDescLayout)           vkDestroyDescriptorSetLayout(device, m_VizDescLayout, nullptr);
        if (m_Sampler)                 vkDestroySampler(device, m_Sampler, nullptr);
        if (m_NoiseSampler)            vkDestroySampler(device, m_NoiseSampler, nullptr);
        if (m_BlueNoiseSampler)        vkDestroySampler(device, m_BlueNoiseSampler, nullptr);
        m_NoiseTexture.reset();
        m_BlueNoise2D.reset();
        m_InjectDensityDescLayout = VK_NULL_HANDLE;
        m_InjectScatterDescLayout = VK_NULL_HANDLE;
        m_EmptySet2Layout         = VK_NULL_HANDLE;
        m_IntegrateDescLayout     = VK_NULL_HANDLE;
        m_ResolveDescLayout       = VK_NULL_HANDLE;
        m_CompositeDescLayout     = VK_NULL_HANDLE;
        m_VizDescLayout           = VK_NULL_HANDLE;
        m_Sampler                 = VK_NULL_HANDLE;
        m_NoiseSampler            = VK_NULL_HANDLE;
        m_BlueNoiseSampler        = VK_NULL_HANDLE;
    }

    bool VolumetricSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        auto deferComp = [](std::unique_ptr<VKComputePipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if (name == "volumetric_inject_density.comp" && m_InjectDensityDescLayout)
        {
            m_InjectDensitySpv = spv;
            deferComp(m_InjectDensityPipeline);
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(InjectPC) };
            m_InjectDensityPipeline = std::make_unique<VKComputePipeline>(m_InjectDensitySpv,
                std::vector<VkDescriptorSetLayout>{
                    m_Pipeline->GetGlobal().GetSetLayout(),
                    m_InjectDensityDescLayout,
                },
                std::vector<VkPushConstantRange>{ pc });
            return true;
        }
        if (name == "volumetric_inject_scatter.comp" && m_InjectScatterDescLayout)
        {
            m_InjectScatterSpv = spv;
            deferComp(m_InjectScatterPipeline);
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(InjectPC) };
            m_InjectScatterPipeline = std::make_unique<VKComputePipeline>(m_InjectScatterSpv,
                std::vector<VkDescriptorSetLayout>{
                    m_Pipeline->GetGlobal().GetSetLayout(),
                    m_InjectScatterDescLayout,
                    m_EmptySet2Layout,
                    MaterialSystem::GetDescriptorSetLayout(),
                    VulkanContext::Get().GetBindlessSet().GetLayout(),
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

    void VolumetricSubsystem::WriteInjectDensityView(ViewResources& vr)
    {
        // Stable across frames: b0 (volDensity storage write), b2 (3D noise sampler).
        // b1 (FogVolume SSBO) rewrites per frame in WriteInjectDensityPerFrame.
        if (m_InjectDensityDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volDensity) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        auto vkDens = std::static_pointer_cast<VKTexture>(vr.volDensity);

        VkDescriptorImageInfo densInfo{};
        densInfo.imageView   = vkDens->GetImageView();
        densInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo noiseInfo{};
        if (m_NoiseTexture && m_NoiseSampler)
        {
            noiseInfo.imageView   = std::static_pointer_cast<VKTexture>(m_NoiseTexture)->GetImageView();
            noiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            noiseInfo.sampler     = m_NoiseSampler;
        }

        VkWriteDescriptorSet writes[2 * MAX_FRAMES_IN_FLIGHT]{};
        u32 w = 0;
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSet set = vr.volInjectDensityDescSet[i];
            if (set == VK_NULL_HANDLE) continue;

            writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[w].dstSet          = set;
            writes[w].dstBinding      = 0;
            writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[w].descriptorCount = 1;
            writes[w].pImageInfo      = &densInfo;
            ++w;

            if (noiseInfo.sampler)
            {
                writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[w].dstSet          = set;
                writes[w].dstBinding      = 2;
                writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[w].descriptorCount = 1;
                writes[w].pImageInfo      = &noiseInfo;
                ++w;
            }
        }
        vkUpdateDescriptorSets(device, w, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteInjectDensityPerFrame(const Memory::GPUSubRegion& fogVolumeRegion)
    {
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;

        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!vr || vr->volInjectDensityDescSet[slot] == VK_NULL_HANDLE) return;
        if (!fogVolumeRegion.buffer) return;

        VkDescriptorBufferInfo fogBi{ fogVolumeRegion.buffer, fogVolumeRegion.offset, fogVolumeRegion.size };
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vr->volInjectDensityDescSet[slot];
        write.dstBinding      = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo     = &fogBi;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);
    }

    void VolumetricSubsystem::WriteInjectScatterView(ViewResources& vr)
    {
        // Stable across frames: b0 (volDensity sampler3D, read), b1 (volInScatter storage write),
        // b5 (shadow array sampler). SSBOs b2-b4 rewrite per frame in WriteInjectScatterPerFrame.
        // RG transitions volDensity to SHADER_READ_ONLY before this pass runs (it's a Read here).
        if (m_InjectScatterDescLayout == VK_NULL_HANDLE) return;
        if (!vr.volDensity || !vr.volInScatter) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        auto& lighting  = m_Pipeline->GetLighting();

        auto vkDens = std::static_pointer_cast<VKTexture>(vr.volDensity);
        auto vkScat = std::static_pointer_cast<VKTexture>(vr.volInScatter);

        VkDescriptorImageInfo densInfo{};
        densInfo.imageView   = vkDens->GetImageView();
        densInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        densInfo.sampler     = m_Sampler;
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

        VkWriteDescriptorSet writes[3 * MAX_FRAMES_IN_FLIGHT]{};
        u32 w = 0;
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorSet set = vr.volInjectScatterDescSet[i];
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

            if (shadowInfo.sampler)
            {
                writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[w].dstSet          = set;
                writes[w].dstBinding      = 5;
                writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[w].descriptorCount = 1;
                writes[w].pImageInfo      = &shadowInfo;
                ++w;
            }
        }
        vkUpdateDescriptorSets(device, w, writes, 0, nullptr);
    }

    void VolumetricSubsystem::WriteInjectScatterPerFrame(const Memory::GPUSubRegion& lightSSBORegion,
                                                         const Memory::GPUSubRegion& clusterGridRegion,
                                                         const Memory::GPUSubRegion& lightIndexRegion)
    {
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;

        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!vr || vr->volInjectScatterDescSet[slot] == VK_NULL_HANDLE) return;
        if (!lightSSBORegion.buffer || !clusterGridRegion.buffer || !lightIndexRegion.buffer) return;

        VkDescriptorBufferInfo lightBi{ lightSSBORegion.buffer,   lightSSBORegion.offset,   lightSSBORegion.size   };
        VkDescriptorBufferInfo gridBi { clusterGridRegion.buffer, clusterGridRegion.offset, clusterGridRegion.size };
        VkDescriptorBufferInfo indexBi{ lightIndexRegion.buffer,  lightIndexRegion.offset,  lightIndexRegion.size  };

        VkWriteDescriptorSet writes[3]{};
        const VkDescriptorBufferInfo* infos[3] = { &lightBi, &gridBi, &indexBi };
        for (u32 i = 0; i < 3; ++i)
        {
            writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[i].dstSet          = vr->volInjectScatterDescSet[slot];
            writes[i].dstBinding      = 2 + i;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo     = infos[i];
        }
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 3, writes, 0, nullptr);
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
        // b0 (sceneDepth) + b2 (blueNoise) are stable per-view. b1 (in-scatter sampler) rewrites
        // each frame in WriteCompositePerFrame to follow integrate's ping-pong parity.
        if (m_CompositeDescLayout == VK_NULL_HANDLE) return;

        auto sceneDepthTex = targets.GetSceneDepth();
        if (!sceneDepthTex) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        auto vkDepth = std::static_pointer_cast<VKTexture>(sceneDepthTex);

        VkDescriptorImageInfo depthInfo{};
        depthInfo.imageView   = vkDepth->GetImageView();
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depthInfo.sampler     = m_Sampler;

        VkDescriptorImageInfo blueInfo{};
        const bool haveBlue = (m_BlueNoise2D && m_BlueNoiseSampler);
        if (haveBlue)
        {
            blueInfo.imageView   = std::static_pointer_cast<VKTexture>(m_BlueNoise2D)->GetImageView();
            blueInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            blueInfo.sampler     = m_BlueNoiseSampler;
        }

        VkWriteDescriptorSet writes[2 * MAX_FRAMES_IN_FLIGHT]{};
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
            if (haveBlue)
            {
                writes[w] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[w].dstSet          = set;
                writes[w].dstBinding      = 2;
                writes[w].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[w].descriptorCount = 1;
                writes[w].pImageInfo      = &blueInfo;
                ++w;
            }
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

    RG::ResourceHandle VolumetricSubsystem::AddInjectDensityPass(RG::RenderGraph& rg)
    {
        struct DensityData
        {
            RG::ResourceHandle density;
        };
        RG::ResourceHandle outputHandle;

        rg.AddComputePass<DensityData>("VolumetricInjectDensity", RG::QueueFamily::AsyncCompute,
            [&, this](DensityData& data, RG::RenderPassBuilder& builder)
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
                outputHandle = data.density;
            },
            [this](DensityData& /*data*/, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "VolumetricInjectDensity",
                    "VolDensity", false,
                    { "volumetric_inject_density", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;

                if (!m_InjectDensityPipeline || vr->volInjectDensityDescSet[slot] == VK_NULL_HANDLE)
                {
                    sys.GetFrameDebugger().EndCapturePass();
                    return;
                }

                m_InjectDensityPipeline->Bind(cmd);
                VkDescriptorSet sets[2] = {
                    vr->globalDescriptorSet[slot],
                    vr->volInjectDensityDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_InjectDensityPipeline->GetLayout(), 0, 2, sets, 0, nullptr);

                InjectPC pc{};
                pc.invView = Math::Inverse(m_Pipeline->GetCurrentView()->camera.view);
                pc.volDimX = vr->volDimX; pc.volDimY = vr->volDimY; pc.volDimZ = vr->volDimZ;
                vkCmdPushConstants(cmd, m_InjectDensityPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(InjectPC), &pc);

                const u32 groupX = (vr->volDimX + 7) / 8;
                const u32 groupY = (vr->volDimY + 7) / 8;
                const u32 groupZ = (vr->volDimZ + 3) / 4;
                vkCmdDispatch(cmd, groupX, groupY, groupZ);

                sys.GetFrameDebugger().CaptureComputeDispatch("VolumetricInjectDensity",
                    "volumetric_inject_density", groupX, groupY, groupZ);
                sys.GetFrameDebugger().EndCapturePass();
            });
        return outputHandle;
    }

    bool VolumetricSubsystem::IsRtShadowsEnabled() const
    {
        return m_Pipeline && m_Pipeline->GetSystem().GetVolumetricSettings().rtShadows;
    }

    RG::ResourceHandle VolumetricSubsystem::AddInjectScatterPass(RG::RenderGraph& rg,
        RG::ResourceHandle density,
        const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount])
    {
        struct ScatterData
        {
            RG::ResourceHandle density;
            RG::ResourceHandle inScatter;
            RG::ResourceHandle shadowCascades[k_ShadowCascadeCount];
        };
        RG::ResourceHandle outputHandle;

        rg.AddComputePass<ScatterData>("VolumetricInjectScatter", RG::QueueFamily::AsyncCompute,
            [&, this, density](ScatterData& data, RG::RenderPassBuilder& builder)
            {
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                // Reuse the density pass's ResourceNode (hazard #1: no fresh ImportResource — the
                // RG barrier between the two compute passes only fires when both share the same
                // node). Sampling via sampler3D in the shader; RG transitions to SHADER_READ_ONLY.
                data.density = builder.ReadStorageImage(density);

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

                // Per-cascade Read triggers DEPTH→SHADER_READ barriers — shader binding 5 samples
                // the full shadow-map array. ReadStorageImage despite the COMBINED_IMAGE_SAMPLER
                // descriptor — builder name is about queue affinity (COMPUTE_SHADER stage).
                for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                    if (shadowHandles[i].IsValid())
                        data.shadowCascades[i] = builder.ReadStorageImage(shadowHandles[i]);

                outputHandle = data.inScatter;
            },
            [this](ScatterData& /*data*/, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "VolumetricInjectScatter",
                    "VolInScatter", false,
                    { "volumetric_inject_scatter", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
                const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;

                if (!m_InjectScatterPipeline || vr->volInjectScatterDescSet[slot] == VK_NULL_HANDLE)
                {
                    sys.GetFrameDebugger().EndCapturePass();
                    return;
                }

                // RT fog shadows read the TLAS via rayQuery → order the per-frame TLAS build (same
                // AsyncCompute primary, registered earlier) before this dispatch. dstStage = COMPUTE_SHADER
                // (NOT RAY_TRACING — rayQuery runs in compute; a RAY_TRACING dst here is a TDR trap). Gated
                // so the off path emits nothing.
                if (IsRtShadowsEnabled())
                {
                    VkMemoryBarrier2 asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                    asBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                    asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                    asBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                    VkDependencyInfo asDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    asDep.memoryBarrierCount = 1;
                    asDep.pMemoryBarriers    = &asBarrier;
                    vkCmdPipelineBarrier2(cmd, &asDep);
                }

                m_InjectScatterPipeline->Bind(cmd);
                // Sets 0-1 (global, scatter state) then Sets 3-4 (Material, bindless) — two binds straddle
                // the empty Set 2. Set 3/4 are statically referenced by geom_table.glsl, so they bind every
                // dispatch even when RT fog is off (validation requires bound sets for static references).
                VkDescriptorSet sets01[2] = {
                    vr->globalDescriptorSet[slot],
                    vr->volInjectScatterDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_InjectScatterPipeline->GetLayout(), 0, 2, sets01, 0, nullptr);
                VkDescriptorSet sets34[2] = {
                    MaterialSystem::GetDescriptorSet(slot),
                    VulkanContext::Get().GetBindlessSet().GetSet(),
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_InjectScatterPipeline->GetLayout(), 3, 2, sets34, 0, nullptr);

                InjectPC pc{};
                pc.invView      = Math::Inverse(m_Pipeline->GetCurrentView()->camera.view);
                pc.volDimX = vr->volDimX; pc.volDimY = vr->volDimY; pc.volDimZ = vr->volDimZ;
                pc.geomTableBDA = m_Pipeline->GetRt().GetGeometryTableBDA();
                vkCmdPushConstants(cmd, m_InjectScatterPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(InjectPC), &pc);

                const u32 groupX = (vr->volDimX + 7) / 8;
                const u32 groupY = (vr->volDimY + 7) / 8;
                const u32 groupZ = (vr->volDimZ + 3) / 4;
                vkCmdDispatch(cmd, groupX, groupY, groupZ);

                sys.GetFrameDebugger().CaptureComputeDispatch("VolumetricInjectScatter",
                    "volumetric_inject_scatter", groupX, groupY, groupZ);
                sys.GetFrameDebugger().EndCapturePass();
            });
        return outputHandle;
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
                // GENERAL → SHADER_READ_ONLY transitions (see arch/rendering-pipeline.md re-import hazard).
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
