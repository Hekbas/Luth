#include "luthpch.h"
#include "luth/renderer/subsystems/LightingSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/LightingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/lighting/IBLPrecompute.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/core/FrameData.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

namespace Luth
{
    void LightingSubsystem::Init(RenderPipeline& pipeline, const fs::path& hdrPath)
    {
        LH_PROFILE_FUNCTION();
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        auto loadSpv = [](const char* relPath) -> std::vector<u32> {
            auto sh = ShaderLibrary::LoadEngine(relPath);
            return sh ? sh->GetSpirV() : std::vector<u32>{};
        };
        m_ShadowVertSpv        = loadSpv("shaders/shadowDepth.vert");
        m_ShadowFragSpv        = loadSpv("shaders/shadowDepth.frag");
        m_ShadowSkinnedVertSpv = loadSpv("shaders/shadowDepth_skinned.vert");

        if (m_ShadowVertSpv.empty() || m_ShadowFragSpv.empty() || m_ShadowSkinnedVertSpv.empty())
        {
            LH_LOG(Renderer, error, "LightingSubsystem: shadow shader SPIR-V empty after asset load!");
            return;
        }

        CreateShadowResources(device);
        LoadIBL(hdrPath);

        // Forward+ cluster build pipeline. Layout has 2 SSBO bindings (AABB write, Grid write).
        // UAB matches GTAO main's pattern — per-render-stage descriptor rewrites need it to
        // dodge validation 03047 when the previous frame's cmd buffer is still pending.
        {
            VkDescriptorSetLayoutBinding bindings[2] = {};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorBindingFlags bindingFlags[2] = {
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 2;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.pNext        = &bindingFlagsCI;
            layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutCI.bindingCount = 2;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_ClusterBuildSetLayout);

            // Push constant: invProjection + viewportSize + _pad + nearZ + farZ + uvec2 tiles = 96 B.
            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 96 };

            m_ClusterBuildSpv = loadSpv("shaders/cluster_build.comp");
            if (m_ClusterBuildSpv.empty())
            {
                LH_LOG(Renderer, error, "LightingSubsystem: failed to load cluster_build.comp!");
                return;
            }
            m_ClusterBuildPipeline = std::make_unique<VKComputePipeline>(
                m_ClusterBuildSpv,
                std::vector<VkDescriptorSetLayout>{ m_ClusterBuildSetLayout },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // Forward+ light-to-cluster assignment pipeline. 5 SSBO bindings; UAB for the per-frame
        // descriptor rewrites that happen inside RecordView.
        {
            VkDescriptorSetLayoutBinding bindings[5] = {};
            for (u32 i = 0; i < 5; ++i)
            {
                bindings[i].binding         = i;
                bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            }

            VkDescriptorBindingFlags bindingFlags[5] = {
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 5;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.pNext        = &bindingFlagsCI;
            layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutCI.bindingCount = 5;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_LightAssignSetLayout);

            // Push constant: mat4 view + u32 pointLightCount + u32 spotLightCount + u32 maxLightsPerCluster + u32 _pad = 80 B.
            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 80 };

            m_LightAssignSpv = loadSpv("shaders/light_assign.comp");
            if (m_LightAssignSpv.empty())
            {
                LH_LOG(Renderer, error, "LightingSubsystem: failed to load light_assign.comp!");
                return;
            }
            m_LightAssignPipeline = std::make_unique<VKComputePipeline>(
                m_LightAssignSpv,
                std::vector<VkDescriptorSetLayout>{ m_LightAssignSetLayout },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // Cluster debug viz pipeline. Two descriptor sets: set 0 = depth sampler (per-view stable),
        // set 1 = m_LightSetLayout (per-view × per-frame, the existing lightDescSet — only b1 read).
        {
            VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sampCI.magFilter    = VK_FILTER_NEAREST;
            sampCI.minFilter    = VK_FILTER_NEAREST;
            sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &sampCI, nullptr, &m_ClusterVizDepthSampler);

            VkDescriptorSetLayoutBinding sbinding{};
            sbinding.binding         = 0;
            sbinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sbinding.descriptorCount = 1;
            sbinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo slayoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slayoutCI.bindingCount = 1;
            slayoutCI.pBindings    = &sbinding;
            vkCreateDescriptorSetLayout(device, &slayoutCI, nullptr, &m_ClusterVizDescSetLayout);

            m_FullscreenVertSpv = loadSpv("shaders/fullscreen.vert");
            m_ClusterVizFragSpv = loadSpv("shaders/cluster_viz.frag");
            if (!m_FullscreenVertSpv.empty() && !m_ClusterVizFragSpv.empty())
            {
                std::vector<VkDescriptorSetLayout> layouts = { m_ClusterVizDescSetLayout, m_LightSetLayout };
                VkPushConstantRange pcRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };  // vec2 viewport + nearZ + farZ
                PipelineConfig cfg;
                cfg.colorFormats       = { VK_FORMAT_R8G8B8A8_UNORM };
                cfg.depthFormat        = VK_FORMAT_UNDEFINED;
                cfg.depthTest          = false;
                cfg.depthWrite         = false;
                cfg.blendEnabled       = true;
                cfg.cullMode           = VK_CULL_MODE_NONE;
                cfg.pushConstantRanges = { pcRange };
                m_ClusterVizPipeline = std::make_unique<VKPipeline>(
                    cfg, m_FullscreenVertSpv, m_ClusterVizFragSpv, layouts);
            }
        }
    }

    void LightingSubsystem::BuildPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        LH_PROFILE_FUNCTION();
        BuildShadowPipelines(geoLayouts);
        BuildSkyboxPipeline(geoLayouts);
    }

    void LightingSubsystem::Shutdown()
    {
        LH_PROFILE_FUNCTION();
        VkDevice device = VulkanContext::Get().GetDevice();

        m_SkyboxPipeline.reset();
        m_SkyboxVB.reset();
        m_ShadowSkinnedPipeline.reset();
        m_ShadowPipeline.reset();

        m_IrradianceMap.reset();
        m_PrefilteredMap.reset();
        m_BRDFLut.reset();
        if (m_IBLSampler) { vkDestroySampler(device, m_IBLSampler, nullptr); m_IBLSampler = VK_NULL_HANDLE; }

        if (m_ShadowSampler)        { vkDestroySampler(device, m_ShadowSampler, nullptr);        m_ShadowSampler        = VK_NULL_HANDLE; }
        if (m_SunShadowMaskSampler) { vkDestroySampler(device, m_SunShadowMaskSampler, nullptr); m_SunShadowMaskSampler = VK_NULL_HANDLE; }
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
        {
            if (m_ShadowLayerViews[i]) vkDestroyImageView(device, m_ShadowLayerViews[i], nullptr);
            m_ShadowLayerViews[i] = VK_NULL_HANDLE;
        }
        m_ShadowMap.reset();

        if (m_LightSetLayout) { vkDestroyDescriptorSetLayout(device, m_LightSetLayout, nullptr); m_LightSetLayout = VK_NULL_HANDLE; }

        m_ClusterBuildPipeline.reset();
        if (m_ClusterBuildSetLayout)
        {
            vkDestroyDescriptorSetLayout(device, m_ClusterBuildSetLayout, nullptr);
            m_ClusterBuildSetLayout = VK_NULL_HANDLE;
        }

        m_LightAssignPipeline.reset();
        if (m_LightAssignSetLayout)
        {
            vkDestroyDescriptorSetLayout(device, m_LightAssignSetLayout, nullptr);
            m_LightAssignSetLayout = VK_NULL_HANDLE;
        }

        m_ClusterVizPipeline.reset();
        if (m_ClusterVizDescSetLayout)
        {
            vkDestroyDescriptorSetLayout(device, m_ClusterVizDescSetLayout, nullptr);
            m_ClusterVizDescSetLayout = VK_NULL_HANDLE;
        }
        if (m_ClusterVizDepthSampler)
        {
            vkDestroySampler(device, m_ClusterVizDepthSampler, nullptr);
            m_ClusterVizDepthSampler = VK_NULL_HANDLE;
        }
    }

    void LightingSubsystem::ReloadSkybox(const fs::path& hdrPath, const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        LH_PROFILE_FUNCTION();
        VkDevice device = VulkanContext::Get().GetDevice();
        vkDeviceWaitIdle(device);

        if (m_IBLSampler) { vkDestroySampler(device, m_IBLSampler, nullptr); m_IBLSampler = VK_NULL_HANDLE; }
        LoadIBL(hdrPath);

        // Skybox pipeline depends on prefiltered mip count; rebuild.
        m_SkyboxPipeline.reset();
        BuildSkyboxPipeline(geoLayouts);

        LH_LOG(Renderer, info, "Skybox reloaded from '{}'", hdrPath.string());
    }

    bool LightingSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv,
                                             const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        LH_PROFILE_FUNCTION();
        auto deferGfx = [](std::unique_ptr<VKPipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };
        auto deferComp = [](std::unique_ptr<VKComputePipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if (name == "cluster_build.comp" && m_ClusterBuildSetLayout)
        {
            m_ClusterBuildSpv = spv;
            deferComp(m_ClusterBuildPipeline);
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 96 };
            m_ClusterBuildPipeline = std::make_unique<VKComputePipeline>(m_ClusterBuildSpv,
                std::vector<VkDescriptorSetLayout>{ m_ClusterBuildSetLayout },
                std::vector<VkPushConstantRange>{ pc });
            return true;
        }
        if (name == "light_assign.comp" && m_LightAssignSetLayout)
        {
            m_LightAssignSpv = spv;
            deferComp(m_LightAssignPipeline);
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 80 };
            m_LightAssignPipeline = std::make_unique<VKComputePipeline>(m_LightAssignSpv,
                std::vector<VkDescriptorSetLayout>{ m_LightAssignSetLayout },
                std::vector<VkPushConstantRange>{ pc });
            return true;
        }
        if ((name == "cluster_viz.frag" || name == "fullscreen.vert") && m_ClusterVizDescSetLayout)
        {
            if (name == "cluster_viz.frag") m_ClusterVizFragSpv = spv;
            else                            m_FullscreenVertSpv = spv;
            deferGfx(m_ClusterVizPipeline);
            if (!m_FullscreenVertSpv.empty() && !m_ClusterVizFragSpv.empty())
            {
                std::vector<VkDescriptorSetLayout> layouts = { m_ClusterVizDescSetLayout, m_LightSetLayout };
                VkPushConstantRange pcRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
                PipelineConfig cfg;
                cfg.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
                cfg.depthFormat  = VK_FORMAT_UNDEFINED;
                cfg.depthTest    = false; cfg.depthWrite = false;
                cfg.blendEnabled = true;
                cfg.cullMode     = VK_CULL_MODE_NONE;
                cfg.pushConstantRanges = { pcRange };
                m_ClusterVizPipeline = std::make_unique<VKPipeline>(
                    cfg, m_FullscreenVertSpv, m_ClusterVizFragSpv, layouts);
            }
            return true;
        }

        if (name == "shadowDepth.vert")           m_ShadowVertSpv        = spv;
        else if (name == "shadowDepth.frag")      m_ShadowFragSpv        = spv;
        else if (name == "shadowDepth_skinned.vert") m_ShadowSkinnedVertSpv = spv;
        else if (name == "skybox.vert")           m_SkyboxVertSpv        = spv;
        else if (name == "skybox.frag")           m_SkyboxFragSpv        = spv;
        else return false;

        if (name == "skybox.vert" || name == "skybox.frag")
        {
            deferGfx(m_SkyboxPipeline);
            BuildSkyboxPipeline(geoLayouts);
        }
        else
        {
            deferGfx(m_ShadowPipeline);
            deferGfx(m_ShadowSkinnedPipeline);
            BuildShadowPipelines(geoLayouts);
        }
        return true;
    }

    VkDescriptorSet LightingSubsystem::GetLightDescSet(u32 slot) const
    {
        // Set 3 lives on the active ViewResources. Replay paths with a null view return VK_NULL_HANDLE.
        auto* vr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!vr) return VK_NULL_HANDLE;
        return vr->lightDescSet[slot];
    }

    void LightingSubsystem::WriteShadowView(ViewResources& vr)
    {
        LH_PROFILE_FUNCTION();
        if (!m_ShadowMap || vr.lightDescSet[0] == VK_NULL_HANDLE) return;

        auto vkShadowTex = std::static_pointer_cast<VKTexture>(m_ShadowMap);
        VkDescriptorImageInfo shadowImgInfo{};
        shadowImgInfo.sampler     = m_ShadowSampler;
        shadowImgInfo.imageView   = vkShadowTex->GetImageView();
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Binding 4 (RT sun shadow mask) — per-view. The mask image view comes from
        // vr.sunShadowMask (allocated in RecreateViewTextures). pbr.frag reads it only when
        // rtShadowParams.x > 0.5 (RT mode); CSM-mode pixels take the cascade-PCF branch and
        // don't dynamically access binding 4. Layout is SHADER_READ_ONLY_OPTIMAL — the RG
        // transitions the image to this from the RT pass's GENERAL via the consumer's Read.
        VkDescriptorImageInfo maskImgInfo{};
        if (vr.sunShadowMask)
        {
            auto vkMask = std::static_pointer_cast<VKTexture>(vr.sunShadowMask);
            maskImgInfo.sampler     = m_SunShadowMaskSampler;
            maskImgInfo.imageView   = vkMask->GetImageView();
            maskImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // Binding 5 (ReSTIR DI) — per-view demodulated diffuse irradiance, post-denoise. Bound to
        // vr.svgfDenoised (the denoiser output), not vr.restirDI: the denoiser owns this slot whenever
        // ReSTIR is on (it passes the raw DI through when denoising is toggled off), so the bind is
        // static and the A/B is denoise-vs-raw with no descriptor swap. Reused mask sampler (linear
        // clamp-to-edge). pbr.frag reads it only when restirParams.x > 0.5; the denoise pass leaves the
        // image in GENERAL, the GeometryPass Read transitions it to SHADER_READ_ONLY_OPTIMAL.
        VkDescriptorImageInfo diImgInfo{};
        if (vr.svgfDenoised)
        {
            auto vkDI = std::static_pointer_cast<VKTexture>(vr.svgfDenoised);
            diImgInfo.sampler     = m_SunShadowMaskSampler;
            diImgInfo.imageView   = vkDI->GetImageView();
            diImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // Binding 6 (ReSTIR GI) — post-denoise GI irradiance. Bound to vr.svgfGiDenoised (the GI
        // denoiser owns this slot, mirroring b5/DI): the bind is static and the A/B is denoise-vs-raw
        // with no descriptor swap (the denoiser passes the raw GI through when disabled). Same reused
        // mask sampler. pbr.frag adds it only when restirParams.y > 0.5; the GeometryPass Read
        // transitions it from the denoiser's GENERAL to SHADER_READ_ONLY_OPTIMAL.
        VkDescriptorImageInfo giImgInfo{};
        if (vr.svgfGiDenoised)
        {
            auto vkGI = std::static_pointer_cast<VKTexture>(vr.svgfGiDenoised);
            giImgInfo.sampler     = m_SunShadowMaskSampler;
            giImgInfo.imageView   = vkGI->GetImageView();
            giImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // Binding 7 (RT reflections, D.1) — post-denoise specular radiance. Bound to vr.svgfSpecDenoised
        // (the specular denoiser owns the slot, mirroring b5/b6). pbr.frag composites it into the split-sum
        // specular IBL when reflParams.x > 0.5; the GeometryPass Read transitions it to SHADER_READ_ONLY.
        VkDescriptorImageInfo reflImgInfo{};
        if (vr.svgfSpecDenoised)
        {
            auto vkRefl = std::static_pointer_cast<VKTexture>(vr.svgfSpecDenoised);
            reflImgInfo.sampler     = m_SunShadowMaskSampler;
            reflImgInfo.imageView   = vkRefl->GetImageView();
            reflImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // Binding 8 (#154) — post-denoise ReSTIR-DI specular. Bound to vr.svgfDiSpecDenoised; pbr.frag
        // adds it under restirParams.z. GeometryPass's Read transitions it to SHADER_READ_ONLY.
        VkDescriptorImageInfo diSpecImgInfo{};
        if (vr.svgfDiSpecDenoised)
        {
            auto vkDiSpec = std::static_pointer_cast<VKTexture>(vr.svgfDiSpecDenoised);
            diSpecImgInfo.sampler     = m_SunShadowMaskSampler;
            diSpecImgInfo.imageView   = vkDiSpec->GetImageView();
            diSpecImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        VkDevice device = VulkanContext::Get().GetDevice();
        VkWriteDescriptorSet writes[MAX_FRAMES_IN_FLIGHT * 6] = {};
        u32 writeCount = 0;
        for (u32 s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
        {
            writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[writeCount].dstSet          = vr.lightDescSet[s];
            writes[writeCount].dstBinding      = 3;
            writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[writeCount].descriptorCount = 1;
            writes[writeCount].pImageInfo      = &shadowImgInfo;
            ++writeCount;

            if (maskImgInfo.imageView != VK_NULL_HANDLE)
            {
                writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[writeCount].dstSet          = vr.lightDescSet[s];
                writes[writeCount].dstBinding      = 4;
                writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[writeCount].descriptorCount = 1;
                writes[writeCount].pImageInfo      = &maskImgInfo;
                ++writeCount;
            }

            if (diImgInfo.imageView != VK_NULL_HANDLE)
            {
                writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[writeCount].dstSet          = vr.lightDescSet[s];
                writes[writeCount].dstBinding      = 5;
                writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[writeCount].descriptorCount = 1;
                writes[writeCount].pImageInfo      = &diImgInfo;
                ++writeCount;
            }

            if (giImgInfo.imageView != VK_NULL_HANDLE)
            {
                writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[writeCount].dstSet          = vr.lightDescSet[s];
                writes[writeCount].dstBinding      = 6;
                writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[writeCount].descriptorCount = 1;
                writes[writeCount].pImageInfo      = &giImgInfo;
                ++writeCount;
            }

            if (reflImgInfo.imageView != VK_NULL_HANDLE)
            {
                writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[writeCount].dstSet          = vr.lightDescSet[s];
                writes[writeCount].dstBinding      = 7;
                writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[writeCount].descriptorCount = 1;
                writes[writeCount].pImageInfo      = &reflImgInfo;
                ++writeCount;
            }

            if (diSpecImgInfo.imageView != VK_NULL_HANDLE)
            {
                writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[writeCount].dstSet          = vr.lightDescSet[s];
                writes[writeCount].dstBinding      = 8;
                writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[writeCount].descriptorCount = 1;
                writes[writeCount].pImageInfo      = &diSpecImgInfo;
                ++writeCount;
            }
        }
        vkUpdateDescriptorSets(device, writeCount, writes, 0, nullptr);
    }

    // Allocates LightSSBO from the tagged heap, copies the gathered header + point-light array.
    // Returns the region; the BuildGraph caller threads it through WriteSet3PerView, and
    // m_LastLightSSBORegion is cached for AddLightAssignPass's b0 binding.
    Memory::GPUSubRegion LightingSubsystem::UploadLightSSBO(const GatheredLights& lights)
    {
        LH_PROFILE_FUNCTION();
        Memory::GPUSubRegion region{};
        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return region;
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        jobCtx->GpuCache.CurrentTag = frameAbs;

        auto& heap = Memory::GPUTaggedPageAllocator::Get();
        const u64 pointBytes = lights.points.size() * sizeof(PointLightData);
        const u64 spotBytes  = lights.spots.size()  * sizeof(SpotLightData);
        const u64 ssboSize   = sizeof(LightSSBOHeader) + pointBytes + spotBytes;
        region = heap.Allocate(jobCtx->GpuCache, ssboSize, 16);
        if (!region.buffer) return {};

        // Header at offset 0; points[] at offset 48; spots[] right after points (std430 alignment).
        auto* header = static_cast<LightSSBOHeader*>(region.mappedPtr);
        header->dirLight        = lights.dirLight;
        header->pointLightCount = static_cast<u32>(lights.points.size());
        header->spotLightCount  = static_cast<u32>(lights.spots.size());
        header->_pad[0] = header->_pad[1] = 0;
        auto* base = static_cast<u8*>(region.mappedPtr) + sizeof(LightSSBOHeader);
        if (!lights.points.empty())
            std::memcpy(base, lights.points.data(), pointBytes);
        if (!lights.spots.empty())
            std::memcpy(base + pointBytes, lights.spots.data(), spotBytes);
        heap.FlushRegion(region);
        m_LastLightSSBORegion = region;
        return region;
    }

    void LightingSubsystem::WriteSet3PerView(const Memory::GPUSubRegion& lightSSBORegion,
                                             const Memory::GPUSubRegion& clusterGridRegion,
                                             const Memory::GPUSubRegion& lightIndexRegion)
    {
        LH_PROFILE_FUNCTION();
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;

        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!vr || vr->lightDescSet[slot] == VK_NULL_HANDLE) return;
        if (!lightSSBORegion.buffer || !clusterGridRegion.buffer || !lightIndexRegion.buffer) return;

        VkDescriptorBufferInfo lightBi{ lightSSBORegion.buffer,   lightSSBORegion.offset,   lightSSBORegion.size   };
        VkDescriptorBufferInfo gridBi { clusterGridRegion.buffer, clusterGridRegion.offset, clusterGridRegion.size };
        VkDescriptorBufferInfo indexBi{ lightIndexRegion.buffer,  lightIndexRegion.offset,  lightIndexRegion.size  };

        VkWriteDescriptorSet writes[3] = {};
        for (u32 i = 0; i < 3; ++i)
        {
            writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[i].dstSet          = vr->lightDescSet[slot];
            writes[i].dstBinding      = i;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
        }
        writes[0].pBufferInfo = &lightBi;
        writes[1].pBufferInfo = &gridBi;
        writes[2].pBufferInfo = &indexBi;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 3, writes, 0, nullptr);
    }

    // ---- Internal: shadow map + Set 3 layout/pool/set ----
    void LightingSubsystem::CreateShadowResources(VkDevice device)
    {
        LH_PROFILE_FUNCTION();
        // Shadow map: k_ShadowResolution^2, D32_Float, k_ShadowCascadeCount-layer 2D array.
        m_ShadowMap = std::make_shared<VKTexture>(
            k_ShadowResolution, k_ShadowResolution, TextureFormat::D32_Float,
            k_ShadowCascadeCount, /*createFlags*/ 0u, /*mipLevels*/ 1u, /*extraUsage*/ 0u);

        auto shadowTexForViews = std::static_pointer_cast<VKTexture>(m_ShadowMap);
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            m_ShadowLayerViews[i] = shadowTexForViews->CreateLayerView(i);

        // VKTexture's auto-init for depth images transitions to DEPTH_STENCIL_READ_ONLY_OPTIMAL.
        // The Set 3 binding 3 descriptor writes declare SHADER_READ_ONLY_OPTIMAL — matched in CSM
        // mode because ShadowPass writes + GeometryPass Read transitions through. With B.3's
        // RT-mode gating, ShadowPass never runs and the cascade map sits in the initial layout
        // forever, mismatching the descriptor. Override to SHADER_READ_ONLY_OPTIMAL at init so
        // both modes agree. ShadowPass first-frame write does SHADER_READ_ONLY → DSAO normally.
        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            b.srcStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
            b.srcAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            b.dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                  | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                  | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
            b.dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            b.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = shadowTexForViews->GetImage();
            b.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, k_ShadowCascadeCount };
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers    = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        });

        // Shadow sampler (PCF compare: less).
        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter     = VK_FILTER_LINEAR;
        samplerInfo.minFilter     = VK_FILTER_LINEAR;
        samplerInfo.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.borderColor   = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        samplerInfo.compareEnable = VK_TRUE;
        samplerInfo.compareOp     = VK_COMPARE_OP_LESS;
        samplerInfo.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &samplerInfo, nullptr, &m_ShadowSampler);

        // Sun shadow mask sampler (Set 3 binding 4) — linear clamp-to-edge, no compare. Matches the
        // pbr.frag::ComputeShadowRT sample: `texture(sunShadowMask, uv).r`. Clamp-to-edge means
        // off-screen UVs (cluster outside frustum) read the edge value (1.0 if the mask was written
        // with no-shadow at the borders, but in practice pbr.frag clamps uv to [0,1] via gl_FragCoord).
        VkSamplerCreateInfo maskSamplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        maskSamplerInfo.magFilter    = VK_FILTER_LINEAR;
        maskSamplerInfo.minFilter    = VK_FILTER_LINEAR;
        maskSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        maskSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        maskSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        maskSamplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &maskSamplerInfo, nullptr, &m_SunShadowMaskSampler);

        // Set 3 layout: b0 = LightSSBO (header + flexible PointLightData[]), b1 = ClusterGridSSBO,
        // b2 = LightIndexSSBO, b3 = cascade shadow sampler (sampler2DArrayShadow, PCF), b4 = RT
        // sun shadow mask (sampler2D R8, populated when ShadowingMode::RtShadows is active), b5 =
        // ReSTIR DI demodulated irradiance (sampler2D RGBA16F, sampled when restirParams.x > 0.5),
        // b6 = ReSTIR GI demodulated indirect diffuse (sampler2D RGBA16F, added when restirParams.y > 0.5).
        VkDescriptorSetLayoutBinding bindings[9] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        // COMPUTE added so the ReSTIR DI passes (restir_initial/shade.comp) + rt_sun_shadows.comp can
        // read dirLight.direction / points[] / pointLightCount when this layout binds as their Set 1.
        // Cluster grid + light index (b1, b2) intentionally stay fragment-only; neither ReSTIR nor the
        // shadow pass iterates clusters.
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[4].binding = 4;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
                               | VK_SHADER_STAGE_RAYGEN_BIT_KHR;  // raygen may also read for ReSTIR DI (C.1)
        bindings[5].binding = 5;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[5].descriptorCount = 1;
        bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;  // ReSTIR DI image — pbr.frag only
        bindings[6].binding = 6;
        bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[6].descriptorCount = 1;
        bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;  // ReSTIR GI image — pbr.frag only
        bindings[7].binding = 7;
        bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[7].descriptorCount = 1;
        bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;  // RT reflections image (D.1) — pbr.frag only
        bindings[8].binding = 8;
        bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[8].descriptorCount = 1;
        bindings[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;  // ReSTIR DI specular (#154) — pbr.frag only

        // b0/b1/b2 are SSBOs rebound per-frame and are bound by BOTH graphics passes (PBR fragment)
        // AND the AsyncCompute RT raygen (set=1 in the RT pipeline-layout). The cycled-slot protocol
        // alone is no longer sufficient — the second pending reference from the compute submission
        // means vkUpdateDescriptorSets sees the set as in-use even when writing the "next" slot.
        // UAB on the rewritten bindings satisfies VUID-vkUpdateDescriptorSets-None-03047 cleanly.
        // b3-b5 (samplers) stay flag-less — they're per-view stable, not rewritten per frame.
        VkDescriptorBindingFlags bindingFlags[9] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,  // b0 LightSSBO
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,  // b1 ClusterGrid
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,  // b2 LightIndex
            0,                                            // b3 cascade sampler
            0,                                            // b4 sun shadow mask sampler
            0,                                            // b5 ReSTIR DI sampler
            0,                                            // b6 ReSTIR GI sampler
            0,                                            // b7 RT reflections sampler
            0,                                            // b8 ReSTIR DI specular sampler (#154)
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsCI.bindingCount  = 9;
        bindingFlagsCI.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo lightLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        lightLayoutInfo.pNext        = &bindingFlagsCI;
        lightLayoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        lightLayoutInfo.bindingCount = 9;
        lightLayoutInfo.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &lightLayoutInfo, nullptr, &m_LightSetLayout);

        // Descriptor sets themselves move to ViewResources (per-view × MAX_FRAMES_IN_FLIGHT slots).
        // AllocateViewResources allocates from vr.descPool and calls WriteShadowView to populate b3.
    }

    // ---- Internal: IBL precompute (irradiance + prefiltered + BRDF LUT + skybox VB/SPVs) ----
    void LightingSubsystem::LoadIBL(const fs::path& hdrPath)
    {
        LH_PROFILE_FUNCTION();
        IBLResult ibl = IBL::Precompute(hdrPath);
        m_IrradianceMap  = ibl.irradianceMap;
        m_PrefilteredMap = ibl.prefilteredMap;
        m_BRDFLut        = ibl.brdfLut;
        m_IBLSampler     = ibl.iblSampler;
        m_SkyboxVB       = ibl.skyboxVB;
        m_SkyboxVertSpv  = std::move(ibl.skyboxVertSpv);
        m_SkyboxFragSpv  = std::move(ibl.skyboxFragSpv);
        // Per-view set rewrite is the orchestrator's job (RenderPipeline iterates m_ViewResources).
    }

    // ---- Internal: build shadow + skybox pipelines ----
    void LightingSubsystem::BuildShadowPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        LH_PROFILE_FUNCTION();
        // 4-byte VERTEX push constant carries cascadeIndex.
        VkPushConstantRange shadowCascadePC{};
        shadowCascadePC.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        shadowCascadePC.offset     = 0;
        shadowCascadePC.size       = sizeof(u32);

        // Position-only attribute with full PBR vertex stride (52 bytes) so the shadow
        // pipeline can reuse the same VB as PBR draws.
        BufferLayout posOnly = { { ShaderDataType::Float3, "a_Position" } };
        auto shadowBindingDescs = posOnly.GetBindingDescriptions();
        auto shadowAttribDescs  = posOnly.GetAttributeDescriptions();
        if (!shadowBindingDescs.empty())
            shadowBindingDescs[0].stride = sizeof(float) * (3 + 3 + 2 + 2 + 3);

        PipelineConfig shadowConfig;
        shadowConfig.colorFormats = {};
        shadowConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
        shadowConfig.depthTest = true; shadowConfig.depthWrite = true;
        shadowConfig.blendEnabled = false;
        shadowConfig.cullMode = VK_CULL_MODE_FRONT_BIT;
        shadowConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        shadowConfig.bindingDescriptions = shadowBindingDescs;
        shadowConfig.attributeDescriptions = shadowAttribDescs;
        shadowConfig.pushConstantRanges = { shadowCascadePC };

        m_ShadowPipeline = std::make_unique<VKPipeline>(shadowConfig, m_ShadowVertSpv, m_ShadowFragSpv, geoLayouts);

        if (!m_ShadowSkinnedVertSpv.empty())
        {
            // Empty vertex input — deformable VS fetch the deformed buffer by gl_VertexIndex.
            PipelineConfig skinnedConfig = shadowConfig;
            skinnedConfig.bindingDescriptions.clear();
            skinnedConfig.attributeDescriptions.clear();

            m_ShadowSkinnedPipeline = std::make_unique<VKPipeline>(
                skinnedConfig, m_ShadowSkinnedVertSpv, m_ShadowFragSpv, geoLayouts);
        }
    }

    void LightingSubsystem::BuildSkyboxPipeline(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        LH_PROFILE_FUNCTION();
        if (m_SkyboxVertSpv.empty() || m_SkyboxFragSpv.empty()) return;

        BufferLayout skyboxLayout = { { ShaderDataType::Float3, "a_Position" } };

        PipelineConfig skyboxConfig;
        skyboxConfig.colorFormats   = { VK_FORMAT_R16G16B16A16_SFLOAT };
        skyboxConfig.depthFormat    = VK_FORMAT_D32_SFLOAT;
        skyboxConfig.depthTest      = true;
        skyboxConfig.depthWrite     = false;
        skyboxConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        skyboxConfig.blendEnabled   = false;
        // Y-flipped projection reverses winding; cull back = show inside faces.
        skyboxConfig.cullMode  = VK_CULL_MODE_BACK_BIT;
        skyboxConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        skyboxConfig.bindingDescriptions   = skyboxLayout.GetBindingDescriptions();
        skyboxConfig.attributeDescriptions = skyboxLayout.GetAttributeDescriptions();

        m_SkyboxPipeline = std::make_unique<VKPipeline>(skyboxConfig, m_SkyboxVertSpv, m_SkyboxFragSpv, geoLayouts);
    }

    // ---- Render-graph passes ----
    RG::ResourceHandle LightingSubsystem::AddShadowPass(
        RG::RenderGraph& rg, RG::BufferHandle indirectBufferHandle, u32 cascadeIndex)
    {
        LH_PROFILE_FUNCTION();
        struct ShadowPassData {
            RG::ResourceHandle shadowTex;
            RG::BufferHandle   indirectBuf;
            u32                cascadeIndex;
        };

        RG::ResourceHandle shadowHandle;
        const std::string passName = "ShadowPass.C" + std::to_string(cascadeIndex);
        const std::string resName  = "ShadowMap.C" + std::to_string(cascadeIndex);

        rg.AddPass<ShadowPassData>(passName,
            [&](ShadowPassData& data, RG::RenderPassBuilder& builder)
            {
                data.cascadeIndex = cascadeIndex;

                auto vkShadowTex = std::static_pointer_cast<VKTexture>(m_ShadowMap);

                RG::TextureDesc desc;
                desc.name   = resName;
                desc.width  = k_ShadowResolution;
                desc.height = k_ShadowResolution;
                desc.format = RG::TextureFormat::D32_Float;

                // Per-layer view targets cascade `i` only. Barriers carry baseArrayLayer=cascadeIndex, layerCount=1.
                data.shadowTex = rg.ImportResource(desc,
                    (void*)vkShadowTex->GetImage(),
                    (void*)m_ShadowLayerViews[cascadeIndex],
                    RG::ResourceState::Undefined,
                    /*baseArrayLayer*/ cascadeIndex,
                    /*layerCount*/     1);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                data.shadowTex = builder.WriteDepth(data.shadowTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);

                data.indirectBuf = builder.ReadIndirectBuffer(indirectBufferHandle);

                shadowHandle = data.shadowTex;
            },
            [this, passName, resName](ShadowPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, passName, resName, true,
                    { "shadowDepth", 0, VK_CULL_MODE_FRONT_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_ShadowPipeline) { LH_LOG(Renderer, error, "Shadow pipeline is null!"); sys.GetFrameDebugger().EndCapturePass(); return; }

                // Bind all 6 descriptor sets (Set 5 = GPUObjectData SSBO, owned by Geometry).
                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_Pipeline->GetCurrentViewResources()->globalDescriptorSet[slot],
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(slot),
                    m_Pipeline->GetCurrentViewResources()->lightDescSet[slot],
                    BoneMatrixBuffer::GetDescriptorSet(slot),
                    m_Pipeline->GetGeometry().GetObjectSSBODescSet(slot)
                };

                m_ShadowPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_ShadowPipeline->GetLayout(), 0, 6, sets, 0, nullptr);

                const u32 cascadeIdxVal = data.cascadeIndex;
                vkCmdPushConstants(cmd, m_ShadowPipeline->GetLayout(),
                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(u32), &cascadeIdxVal);

                VkViewport viewport{};
                viewport.width    = (float)k_ShadowResolution;
                viewport.height   = (float)k_ShadowResolution;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { k_ShadowResolution, k_ShadowResolution };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                bool currentSkinned = false;

                auto DrawBatch = [&](const std::vector<DrawCommand>& draws)
                {
                    for (const auto& dc : draws)
                    {
                        auto mesh = dc.model->GetMesh(dc.meshIndex);
                        auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                        auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                        if (!vb || !ib) continue;
                        // Deformed draws need the empty-input pipeline; skip if absent — static binds no VB.
                        if (dc.isDeformed && !m_ShadowSkinnedPipeline) continue;

                        if (dc.isDeformed != currentSkinned)
                        {
                            currentSkinned = dc.isDeformed;
                            if (currentSkinned && m_ShadowSkinnedPipeline)
                            {
                                m_ShadowSkinnedPipeline->Bind(cmd);
                                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_ShadowSkinnedPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                                vkCmdPushConstants(cmd, m_ShadowSkinnedPipeline->GetLayout(),
                                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(u32), &cascadeIdxVal);
                            }
                            else
                            {
                                m_ShadowPipeline->Bind(cmd);
                                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_ShadowPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                                vkCmdPushConstants(cmd, m_ShadowPipeline->GetLayout(),
                                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(u32), &cascadeIdxVal);
                            }
                        }

                        // Deformable draws bind no VB — the VS fetches the deformed buffer by gl_VertexIndex.
                        if (!dc.isDeformed)
                        {
                            VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                            VkDeviceSize offsets[] = { 0 };
                            vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                        }
                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                        // Per-view region layout: [camera | C0 | C1 | C2 | C3]. View N starts at
                        // region (N * k_IndirectRegionsPerView); cascade i lives at offset (i+1).
                        const u32 viewBaseRegion = m_Pipeline->GetCurrentView()->viewIndex * RenderPipeline::k_IndirectRegionsPerView;
                        const u32 cmdIndex = (viewBaseRegion + data.cascadeIndex + 1) * RenderPipeline::k_IndirectRegionStride + dc.gpuObjectIndex;
                        const auto& indirectRegion = m_Pipeline->GetGeometry().GetIndirectRegion();
                        VkDeviceSize indirectOffset = indirectRegion.offset + cmdIndex * sizeof(VkDrawIndexedIndirectCommand);
                        vkCmdDrawIndexedIndirect(cmd, indirectRegion.buffer, indirectOffset, 1,
                            sizeof(VkDrawIndexedIndirectCommand));

                        if (sys.GetFrameDebugger().state == DebuggerState::CaptureRequested)
                        {
                            std::string entName = "Entity";
                            const auto& tags = sys.GetActiveSnapshot().tagsByEntity;
                            u32 idx = entt::to_entity(dc.entity);
                            if (idx < tags.size() && tags[idx])
                                entName = tags[idx];
                            sys.GetFrameDebugger().CaptureIndirectDraw(passName,
                                dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                                entName, dc.entityIndex, ib->GetCount(), dc.gpuObjectIndex, indirectOffset,
                                { "shadowDepth", 0, static_cast<u32>(VK_CULL_MODE_FRONT_BIT),
                                  VK_POLYGON_MODE_FILL, dc.isSkinned, true, true, false });
                        }
                    }
                };

                // Transparent casts no shadows — matches its TLAS exclusion (RT-excluded tier).
                DrawBatch(sys.GetDrawList().opaque);
                DrawBatch(sys.GetDrawList().cutout);

                sys.GetFrameDebugger().EndCapturePass();
            }
        );

        return shadowHandle;
    }

    RG::ResourceHandle LightingSubsystem::AddSkyboxPass(
        RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth)
    {
        LH_PROFILE_FUNCTION();
        struct SkyboxPassData {
            RG::ResourceHandle colorTex;
            RG::ResourceHandle depthTex;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<SkyboxPassData>("SkyboxPass",
            [&](SkyboxPassData& data, RG::RenderPassBuilder& builder)
            {
                data.colorTex = builder.Write(sceneColor,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                data.depthTex = builder.WriteDepth(sceneDepth,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE);

                outputHandle = data.colorTex;
            },
            [this](SkyboxPassData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "SkyboxPass", "SceneColor", false,
                    { "skybox", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, false, false });

                if (!m_SkyboxPipeline || !m_SkyboxVB) { sys.GetFrameDebugger().EndCapturePass(); return; }

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_SkyboxPipeline->Bind(cmd);

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_Pipeline->GetCurrentViewResources()->globalDescriptorSet[slot],
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(slot),
                    m_Pipeline->GetCurrentViewResources()->lightDescSet[slot],
                    BoneMatrixBuffer::GetDescriptorSet(slot)
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_SkyboxPipeline->GetLayout(), 0, 5, sets, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.colorTex);
                VkViewport viewport{};
                viewport.width  = (float)res->desc.width;
                viewport.height = (float)res->desc.height;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                VkBuffer vb = m_SkyboxVB->GetVulkanBuffer();
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
                vkCmdDraw(cmd, 36, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("SkyboxPass", "SkyboxCube", "Skybox", 0, 0, dummyPC,
                    { "skybox", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, false, false });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );
        return outputHandle;
    }

    // Forward+ cluster build. Async-compute; per-view tagged-heap regions for AABB + grid.
    // Returns BufferHandles so downstream LightAssignPass / GeometryPass read the same VkBuffer
    // without re-importing (see arch/rendering-pipeline.md re-import hazard).
    LightingSubsystem::ClusterBuildOutputs LightingSubsystem::AddClusterBuildPass(RG::RenderGraph& rg)
    {
        LH_PROFILE_FUNCTION();
        ClusterBuildOutputs out{};
        if (!m_ClusterBuildPipeline) return out;

        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return out;
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
        jobCtx->GpuCache.CurrentTag = frameAbs;

        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!vr || vr->clusterBuildDescSet[slot] == VK_NULL_HANDLE) return out;

        auto& heap = Memory::GPUTaggedPageAllocator::Get();
        // ClusterAABB std430: vec4 min + vec4 max = 32 B per cluster.
        const u64 aabbSize = static_cast<u64>(k_ClusterCount) * 32;
        const u64 gridSize = static_cast<u64>(k_ClusterCount) * sizeof(GPUCluster);
        Memory::GPUSubRegion aabbR = heap.Allocate(jobCtx->GpuCache, aabbSize, 16);
        Memory::GPUSubRegion gridR = heap.Allocate(jobCtx->GpuCache, gridSize, 16);
        if (!aabbR.buffer || !gridR.buffer) return out;

        VkDescriptorBufferInfo aabbBi{ aabbR.buffer, aabbR.offset, aabbR.size };
        VkDescriptorBufferInfo gridBi{ gridR.buffer, gridR.offset, gridR.size };
        VkWriteDescriptorSet writes[2] = {};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr->clusterBuildDescSet[slot];
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &aabbBi;
        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = vr->clusterBuildDescSet[slot];
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo     = &gridBi;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 2, writes, 0, nullptr);

        RG::BufferDesc aabbDesc{ "ClusterAABB", aabbSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        RG::BufferDesc gridDesc{ "ClusterGrid", gridSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        out.aabb       = rg.ImportBuffer(aabbDesc, (void*)aabbR.buffer, RG::ResourceState::Undefined);
        out.grid       = rg.ImportBuffer(gridDesc, (void*)gridR.buffer, RG::ResourceState::Undefined);
        out.aabbRegion = aabbR;
        out.gridRegion = gridR;

        struct ClusterBuildData {
            RG::BufferHandle aabb;
            RG::BufferHandle grid;
        };

        // Capture per-frame values at graph-build time so the execute lambda body stays terse.
        auto* pipeline = m_ClusterBuildPipeline.get();
        FrameDebugger* debugger = &m_Pipeline->GetSystem().GetFrameDebugger();

        rg.AddComputePass<ClusterBuildData>("ClusterBuild", RG::QueueFamily::AsyncCompute,
            [&](ClusterBuildData& d, RG::RenderPassBuilder& builder)
            {
                d.aabb = builder.WriteBuffer(out.aabb);
                d.grid = builder.WriteBuffer(out.grid);
            },
            [this, pipeline, debugger](ClusterBuildData&, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                if (debugger)
                    debugger->BeginCapturePass(ctx.passIndex, "ClusterBuild", "", false,
                        { "cluster_build", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 slotLocal = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex())
                                      % MAX_FRAMES_IN_FLIGHT;
                ViewResources* vrLocal = m_Pipeline->GetCurrentViewResources();
                if (!vrLocal || vrLocal->clusterBuildDescSet[slotLocal] == VK_NULL_HANDLE)
                {
                    if (debugger) debugger->EndCapturePass();
                    return;
                }

                pipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline->GetLayout(), 0, 1, &vrLocal->clusterBuildDescSet[slotLocal], 0, nullptr);

                const auto* view = m_Pipeline->GetCurrentView();
                Mat4 proj = view->camera.projection;
                proj[1][1] *= -1.0f;  // match the Y-flip GlobalSubsystem applies before upload
                Mat4 invProj = Math::Inverse(proj);

                struct ClusterBuildPC {
                    Mat4  invProjection;
                    Vec2  viewportSize;
                    Vec2  _pad;
                    float nearZ;
                    float farZ;
                    u32   tilesX;
                    u32   tilesY;
                } pc{};
                pc.invProjection = invProj;
                pc.viewportSize  = Vec2(static_cast<float>(vrLocal->width),
                                        static_cast<float>(vrLocal->height));
                pc.nearZ  = view->camera.nearZ;
                pc.farZ   = view->camera.farZ;
                pc.tilesX = k_ClusterTilesX;
                pc.tilesY = k_ClusterTilesY;
                vkCmdPushConstants(cmd, pipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ClusterBuildPC), &pc);

                // 4×4×4 local; group dims = ceil(tile/slice counts / 4); bounds-clamp in shader.
                const u32 groupX = (k_ClusterTilesX  + 3) / 4;
                const u32 groupY = (k_ClusterTilesY  + 3) / 4;
                const u32 groupZ = (k_ClusterSlicesZ + 3) / 4;
                vkCmdDispatch(cmd, groupX, groupY, groupZ);

                if (debugger)
                {
                    debugger->CaptureComputeDispatch("ClusterBuild", "cluster_build", groupX, groupY, groupZ);
                    debugger->EndCapturePass();
                }
            });

        return out;
    }

    // Forward+ light-to-cluster assignment. Reads LightSSBO (cached from UploadLightingResources)
    // + Cluster AABB; atomicAdd packs per-cluster light indices into LightIndex and writes
    // (offset, count) to Cluster Grid. Returns the LightIndex handle + SubRegion so the caller
    // can bind b2 of Set 3 in UploadLightingResources.
    LightingSubsystem::LightAssignOutputs LightingSubsystem::AddLightAssignPass(RG::RenderGraph& rg,
                                                                                ClusterBuildOutputs cb)
    {
        LH_PROFILE_FUNCTION();
        LightAssignOutputs out{};
        if (!m_LightAssignPipeline || !m_LastLightSSBORegion.buffer) return out;

        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return out;
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
        jobCtx->GpuCache.CurrentTag = frameAbs;

        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!vr || vr->lightAssignDescSet[slot] == VK_NULL_HANDLE) return out;

        auto& heap = Memory::GPUTaggedPageAllocator::Get();
        const u64 indexSize   = static_cast<u64>(k_ClusterCount) * k_MaxLightsPerCluster * sizeof(u32);
        Memory::GPUSubRegion indexR   = heap.Allocate(jobCtx->GpuCache, indexSize, 16);
        Memory::GPUSubRegion counterR = heap.Allocate(jobCtx->GpuCache, 16, 16);
        if (!indexR.buffer || !counterR.buffer) return out;
        // Counter zero-init host-side — tagged-heap pages are HOST_VISIBLE | MAPPED, so no barrier
        // needed before the compute pass on the async-compute queue (submit-time semaphore covers
        // the host→device dependency).
        std::memset(counterR.mappedPtr, 0, 16);
        heap.FlushRegion(counterR);

        // Reuse ClusterBuild's output buffers — same VkBuffers + offsets the producer wrote.
        VkDescriptorBufferInfo lightBi{ m_LastLightSSBORegion.buffer, m_LastLightSSBORegion.offset,
                                        m_LastLightSSBORegion.size };

        // invariant: bind via the producer's SubRegion offsets — BufferHandle only carries the
        // backing VkBuffer; offset+size live on the SubRegion. Tagged-heap bump allocations cannot
        // be re-derived, so the producer hands its regions through ClusterBuildOutputs.
        VkDescriptorBufferInfo aabbBi{ cb.aabbRegion.buffer, cb.aabbRegion.offset, cb.aabbRegion.size };
        VkDescriptorBufferInfo gridBi{ cb.gridRegion.buffer, cb.gridRegion.offset, cb.gridRegion.size };
        VkDescriptorBufferInfo indexBi{ indexR.buffer,   indexR.offset,   indexR.size };
        VkDescriptorBufferInfo counterBi{ counterR.buffer, counterR.offset, counterR.size };

        VkWriteDescriptorSet writes[5] = {};
        for (u32 i = 0; i < 5; ++i)
        {
            writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[i].dstSet          = vr->lightAssignDescSet[slot];
            writes[i].dstBinding      = i;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
        }
        writes[0].pBufferInfo = &lightBi;
        writes[1].pBufferInfo = &aabbBi;
        writes[2].pBufferInfo = &gridBi;
        writes[3].pBufferInfo = &indexBi;
        writes[4].pBufferInfo = &counterBi;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 5, writes, 0, nullptr);

        RG::BufferDesc indexDesc{ "LightIndex", indexSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        out.index       = rg.ImportBuffer(indexDesc, (void*)indexR.buffer, RG::ResourceState::Undefined);
        out.indexRegion = indexR;

        struct LightAssignData {
            RG::BufferHandle aabb;
            RG::BufferHandle grid;
            RG::BufferHandle index;
        };

        auto* pipeline = m_LightAssignPipeline.get();
        FrameDebugger* debugger = &m_Pipeline->GetSystem().GetFrameDebugger();
        // Snapshot light counts at graph-build time — LightingSystem::GetLights() is final by now.
        u32 capturedPointCount = 0;
        u32 capturedSpotCount  = 0;
        if (auto* lightingSys = SystemRegistry::GetSystem<LightingSystem>())
        {
            capturedPointCount = static_cast<u32>(lightingSys->GetLights().points.size());
            capturedSpotCount  = static_cast<u32>(lightingSys->GetLights().spots.size());
        }

        rg.AddComputePass<LightAssignData>("LightAssign", RG::QueueFamily::AsyncCompute,
            [&](LightAssignData& d, RG::RenderPassBuilder& builder)
            {
                d.aabb  = builder.ReadBuffer(cb.aabb);
                d.grid  = builder.WriteBuffer(cb.grid);
                d.index = builder.WriteBuffer(out.index);
            },
            [this, pipeline, debugger, capturedPointCount, capturedSpotCount](LightAssignData&, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                if (debugger)
                    debugger->BeginCapturePass(ctx.passIndex, "LightAssign", "", false,
                        { "light_assign", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                const u32 slotLocal = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex())
                                      % MAX_FRAMES_IN_FLIGHT;
                ViewResources* vrLoc = m_Pipeline->GetCurrentViewResources();
                if (!vrLoc || vrLoc->lightAssignDescSet[slotLocal] == VK_NULL_HANDLE)
                {
                    if (debugger) debugger->EndCapturePass();
                    return;
                }

                pipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline->GetLayout(), 0, 1, &vrLoc->lightAssignDescSet[slotLocal], 0, nullptr);

                const auto* view = m_Pipeline->GetCurrentView();
                struct LightAssignPC {
                    Mat4 view;
                    u32  pointLightCount;
                    u32  spotLightCount;
                    u32  maxLightsPerCluster;
                    u32  _pad0;
                } pc{};
                pc.view                = view->camera.view;
                pc.pointLightCount     = capturedPointCount;
                pc.spotLightCount      = capturedSpotCount;
                pc.maxLightsPerCluster = k_MaxLightsPerCluster;
                vkCmdPushConstants(cmd, pipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LightAssignPC), &pc);

                // 64-invocation workgroups; one workgroup per 64 clusters.
                const u32 groupX = (k_ClusterCount + 63) / 64;
                vkCmdDispatch(cmd, groupX, 1, 1);

                if (debugger)
                {
                    debugger->CaptureComputeDispatch("LightAssign", "light_assign", groupX, 1, 1);
                    debugger->EndCapturePass();
                }
            });

        return out;
    }

    // Per-view stable depth-sampler write for the ClusterViz set 0; called from
    // AllocateViewResources after FrameTargets exists.
    void LightingSubsystem::WriteClusterVizView(ViewResources& vr, FrameTargets& targets)
    {
        LH_PROFILE_FUNCTION();
        if (vr.clusterVizDescSet == VK_NULL_HANDLE || m_ClusterVizDepthSampler == VK_NULL_HANDLE) return;

        auto vkScnDepth = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth());
        if (!vkScnDepth) return;

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler     = m_ClusterVizDepthSampler;
        depthInfo.imageView   = vkScnDepth->GetImageView();
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vr.clusterVizDescSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &depthInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);
    }

    // Cluster density viz. Fullscreen triangle blended over LDR; samples SceneDepth to compute the
    // true per-fragment 3D cluster ID (Olsson slice from linearized depth + screen tile from UV),
    // then heat-maps the cluster's light count over the lit scene.
    RG::ResourceHandle LightingSubsystem::AddClusterVizPass(RG::RenderGraph& rg,
                                                            RG::ResourceHandle ldrInput,
                                                            RG::ResourceHandle sceneDepth)
    {
        LH_PROFILE_FUNCTION();
        if (!m_ClusterVizPipeline) return ldrInput;

        struct ClusterVizData {
            RG::ResourceHandle output;
            RG::ResourceHandle depth;
        };
        RG::ResourceHandle outputHandle;

        rg.AddPass<ClusterVizData>("ClusterVizPass",
            [&, ldrInput, sceneDepth](ClusterVizData& d, RG::RenderPassBuilder& builder)
            {
                VkClearValue clearVal{ { {0.f, 0.f, 0.f, 1.f} } };
                d.output = builder.Write(ldrInput, VK_ATTACHMENT_LOAD_OP_LOAD,
                                                   VK_ATTACHMENT_STORE_OP_STORE, clearVal);
                d.depth  = builder.Read(sceneDepth);
                outputHandle = d.output;
            },
            [this](ClusterVizData&, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                const auto* view = m_Pipeline->GetCurrentView();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->clusterVizDescSet == VK_NULL_HANDLE) return;
                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex())
                                 % MAX_FRAMES_IN_FLIGHT;
                if (vr->lightDescSet[slot] == VK_NULL_HANDLE) return;

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "ClusterVizPass", "LDROutput", false,
                    { "cluster_viz", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_ClusterVizPipeline->Bind(cmd);
                VkDescriptorSet sets[2] = { vr->clusterVizDescSet, vr->lightDescSet[slot] };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_ClusterVizPipeline->GetLayout(), 0, 2, sets, 0, nullptr);

                struct ClusterVizPC {
                    Vec2  viewport;
                    float nearZ;
                    float farZ;
                } pc{};
                pc.viewport = Vec2(static_cast<float>(vr->width), static_cast<float>(vr->height));
                pc.nearZ    = view->camera.nearZ;
                pc.farZ     = view->camera.farZ;
                vkCmdPushConstants(cmd, m_ClusterVizPipeline->GetLayout(),
                                   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ClusterVizPC), &pc);

                const u32 w = view->targets->GetLDROutput()->GetWidth();
                const u32 h = view->targets->GetLDROutput()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("ClusterVizPass", "FullscreenTriangle", "ClusterViz",
                    0, 0, dummyPC,
                    { "cluster_viz", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );
        return outputHandle;
    }
}
