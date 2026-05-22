#include "luthpch.h"
#include "luth/renderer/subsystems/LightingSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/scene/systems/RenderingSystem.h"
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
            LH_CORE_ERROR("LightingSubsystem: shadow shader SPIR-V empty after asset load!");
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

            // Push constant: mat4 invProjection + vec2 viewportSize + vec2 pad + float nearZ + float farZ
            // + u32 tilesX + u32 tilesY = 64 + 8 + 8 + 4 + 4 + 4 + 4 = 96 B.
            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 96 };

            m_ClusterBuildSpv = loadSpv("shaders/cluster_build.comp");
            if (m_ClusterBuildSpv.empty())
            {
                LH_CORE_ERROR("LightingSubsystem: failed to load cluster_build.comp!");
                return;
            }
            m_ClusterBuildPipeline = std::make_unique<VKComputePipeline>(
                m_ClusterBuildSpv,
                std::vector<VkDescriptorSetLayout>{ m_ClusterBuildSetLayout },
                std::vector<VkPushConstantRange>{ pcRange });
        }
    }

    void LightingSubsystem::BuildPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        BuildShadowPipelines(geoLayouts);
        BuildSkyboxPipeline(geoLayouts);
    }

    void LightingSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        m_SkyboxPipeline.reset();
        m_SkyboxVB.reset();
        m_ShadowSkinnedPipeline.reset();
        m_ShadowPipeline.reset();

        m_IrradianceMap.reset();
        m_PrefilteredMap.reset();
        m_BRDFLut.reset();
        if (m_IBLSampler) { vkDestroySampler(device, m_IBLSampler, nullptr); m_IBLSampler = VK_NULL_HANDLE; }

        if (m_ShadowSampler) { vkDestroySampler(device, m_ShadowSampler, nullptr); m_ShadowSampler = VK_NULL_HANDLE; }
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
        {
            if (m_ShadowLayerViews[i]) vkDestroyImageView(device, m_ShadowLayerViews[i], nullptr);
            m_ShadowLayerViews[i] = VK_NULL_HANDLE;
        }
        m_ShadowMap.reset();

        if (m_LightDescPool)  { vkDestroyDescriptorPool(device, m_LightDescPool, nullptr); m_LightDescPool = VK_NULL_HANDLE; }
        if (m_LightSetLayout) { vkDestroyDescriptorSetLayout(device, m_LightSetLayout, nullptr); m_LightSetLayout = VK_NULL_HANDLE; }
        m_LightDescSet.fill(VK_NULL_HANDLE);

        m_ClusterBuildPipeline.reset();
        if (m_ClusterBuildSetLayout)
        {
            vkDestroyDescriptorSetLayout(device, m_ClusterBuildSetLayout, nullptr);
            m_ClusterBuildSetLayout = VK_NULL_HANDLE;
        }
    }

    void LightingSubsystem::ReloadSkybox(const fs::path& hdrPath, const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        vkDeviceWaitIdle(device);

        if (m_IBLSampler) { vkDestroySampler(device, m_IBLSampler, nullptr); m_IBLSampler = VK_NULL_HANDLE; }
        LoadIBL(hdrPath);

        // Skybox pipeline depends on prefiltered mip count; rebuild.
        m_SkyboxPipeline.reset();
        BuildSkyboxPipeline(geoLayouts);

        LH_CORE_INFO("Skybox reloaded from '{}'", hdrPath.string());
    }

    bool LightingSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv,
                                             const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
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

    void LightingSubsystem::UploadLightUBO(const LightUniforms& lights)
    {
        // Per-frame UBO from GPUTaggedPageAllocator. Tag = render-frame index (absolute);
        // descriptor slot = same index modulo MAX_FRAMES_IN_FLIGHT (cycles per-frame storage).
        // FreeTag(N-2) reclaims after the GPU retires the consuming frame.
        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
        jobCtx->GpuCache.CurrentTag = frameAbs;

        auto& heap   = Memory::GPUTaggedPageAllocator::Get();
        const u64 al = VulkanContext::Get().GetMinUniformBufferAlignment();
        Memory::GPUSubRegion region = heap.Allocate(jobCtx->GpuCache, sizeof(LightUniforms), al);
        if (!region.buffer) return;

        memcpy(region.mappedPtr, &lights, sizeof(LightUniforms));
        heap.FlushRegion(region);

        // Stub allocation for the newly-declared SSBO bindings b1 (cluster grid) + b2 (light index).
        // Bound to a small tagged region so validation sees an initialized descriptor; pbr.frag does
        // not read these until sub-task 3b. Same region for both — they share a single VkBuffer slice.
        Memory::GPUSubRegion stub = heap.Allocate(jobCtx->GpuCache, 16, 16);
        if (!stub.buffer) return;

        VkDescriptorBufferInfo bi{};
        bi.buffer = region.buffer;
        bi.offset = region.offset;
        bi.range  = region.size;

        VkDescriptorBufferInfo stubBi{};
        stubBi.buffer = stub.buffer;
        stubBi.offset = stub.offset;
        stubBi.range  = stub.size;

        VkWriteDescriptorSet writes[3] = {};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = m_LightDescSet[slot];
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &bi;

        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = m_LightDescSet[slot];
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo     = &stubBi;

        writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[2].dstSet          = m_LightDescSet[slot];
        writes[2].dstBinding      = 2;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo     = &stubBi;

        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 3, writes, 0, nullptr);
    }

    // ---- Internal: shadow map + Set 3 layout/pool/set ----
    void LightingSubsystem::CreateShadowResources(VkDevice device)
    {
        // Shadow map: k_ShadowResolution^2, D32_Float, k_ShadowCascadeCount-layer 2D array.
        m_ShadowMap = std::make_shared<VKTexture>(
            k_ShadowResolution, k_ShadowResolution, TextureFormat::D32_Float,
            k_ShadowCascadeCount, /*createFlags*/ 0u, /*mipLevels*/ 1u, /*extraUsage*/ 0u);

        auto shadowTexForViews = std::static_pointer_cast<VKTexture>(m_ShadowMap);
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            m_ShadowLayerViews[i] = shadowTexForViews->CreateLayerView(i);

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

        // Set 3 layout: b0 = LightUBO (becomes LightSSBO in 3b), b1 = ClusterGridSSBO,
        // b2 = LightIndexSSBO, b3 = shadow sampler. b1/b2 declared now so sub-task 3a stays a
        // pure structural move; UploadLightUBO writes stub regions to them until 3b lands.
        VkDescriptorSetLayoutBinding bindings[4] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
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

        // All four bindings rebound per render-stage against per-frame slots (cycling), so no UAB.
        VkDescriptorSetLayoutCreateInfo lightLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        lightLayoutInfo.bindingCount = 4;
        lightLayoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &lightLayoutInfo, nullptr, &m_LightSetLayout);

        VkDescriptorPoolSize poolSizes[3] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT * 2;  // b1 + b2
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[2].descriptorCount = MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_LightDescPool);

        VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) layouts[i] = m_LightSetLayout;
        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = m_LightDescPool;
        allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        allocInfo.pSetLayouts = layouts;
        vkAllocateDescriptorSets(device, &allocInfo, m_LightDescSet.data());
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            char name[48]; std::snprintf(name, sizeof(name), "Lighting.Light.Slot%u", i);
            VulkanContext::SetDebugName(m_LightDescSet[i], name);
        }

        // Initial write: stable shadow sampler at b3 propagated to every slot. b0/b1/b2 rewritten
        // per-frame in UploadLightUBO against the slot's set.
        auto vkShadowTex = std::static_pointer_cast<VKTexture>(m_ShadowMap);
        VkDescriptorImageInfo shadowImgInfo{};
        shadowImgInfo.sampler     = m_ShadowSampler;
        shadowImgInfo.imageView   = vkShadowTex->GetImageView();
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet samplerWrites[MAX_FRAMES_IN_FLIGHT] = {};
        for (u32 s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
        {
            samplerWrites[s] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            samplerWrites[s].dstSet          = m_LightDescSet[s];
            samplerWrites[s].dstBinding      = 3;
            samplerWrites[s].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            samplerWrites[s].descriptorCount = 1;
            samplerWrites[s].pImageInfo      = &shadowImgInfo;
        }
        vkUpdateDescriptorSets(device, MAX_FRAMES_IN_FLIGHT, samplerWrites, 0, nullptr);
    }

    // ---- Internal: IBL precompute (irradiance + prefiltered + BRDF LUT + skybox VB/SPVs) ----
    void LightingSubsystem::LoadIBL(const fs::path& hdrPath)
    {
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

    // ---- Internal: build shadow + skybox pipelines (extracted from PipelineFactory) ----
    void LightingSubsystem::BuildShadowPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
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
            BufferLayout skinned = {
                { ShaderDataType::Float3, "a_Position"    },
                { ShaderDataType::Float3, "a_Normal"      },
                { ShaderDataType::Float2, "a_TexCoord0"   },
                { ShaderDataType::Float2, "a_TexCoord1"   },
                { ShaderDataType::Float3, "a_Tangent"     },
                { ShaderDataType::Int4,   "a_BoneIDs"     },
                { ShaderDataType::Float4, "a_BoneWeights" }
            };
            auto skinnedBindings = skinned.GetBindingDescriptions();
            auto skinnedAttribs  = skinned.GetAttributeDescriptions();

            PipelineConfig skinnedConfig = shadowConfig;
            skinnedConfig.bindingDescriptions   = skinnedBindings;
            skinnedConfig.attributeDescriptions = skinnedAttribs;

            m_ShadowSkinnedPipeline = std::make_unique<VKPipeline>(
                skinnedConfig, m_ShadowSkinnedVertSpv, m_ShadowFragSpv, geoLayouts);
        }
    }

    void LightingSubsystem::BuildSkyboxPipeline(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
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

                if (!m_ShadowPipeline) { LH_CORE_ERROR("Shadow pipeline is null!"); sys.GetFrameDebugger().EndCapturePass(); return; }

                // Bind all 6 descriptor sets (Set 5 = GPUObjectData SSBO, owned by Geometry until sub-task C).
                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_Pipeline->GetCurrentViewResources()->globalDescriptorSet[slot],
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(slot),
                    m_LightDescSet[slot],
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

                        if (dc.isSkinned != currentSkinned)
                        {
                            currentSkinned = dc.isSkinned;
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

                        VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
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

                DrawBatch(sys.GetDrawList().opaque);
                DrawBatch(sys.GetDrawList().cutout);
                DrawBatch(sys.GetDrawList().transparent);

                sys.GetFrameDebugger().EndCapturePass();
            }
        );

        return shadowHandle;
    }

    RG::ResourceHandle LightingSubsystem::AddSkyboxPass(
        RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth)
    {
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
                    m_LightDescSet[slot],
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

    // Forward+ cluster build. Per-view tagged-heap regions for AABB + grid; descriptor set on
    // ViewResources cycled by frame; dispatched on the async-compute queue. Producer handles flow
    // back through the return value so LightAssignPass + (later) GeometryPass can read them without
    // re-ImportBuffer'ing the same VkBuffer (arch hazard 1).
    LightingSubsystem::ClusterBuildOutputs LightingSubsystem::AddClusterBuildPass(RG::RenderGraph& rg)
    {
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
        out.aabb = rg.ImportBuffer(aabbDesc, (void*)aabbR.buffer, RG::ResourceState::Undefined);
        out.grid = rg.ImportBuffer(gridDesc, (void*)gridR.buffer, RG::ResourceState::Undefined);

        struct ClusterBuildData {
            RG::BufferHandle aabb;
            RG::BufferHandle grid;
        };

        // Capture per-frame values (camera, viewport) at graph-build time. Execute lambda runs later;
        // m_Pipeline->GetCurrentView() is still valid at execute time because the executor sets it
        // before dispatching the recorders, but capturing now keeps the lambda body terse.
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

                // (4, 3, 6) groups × (4, 4, 4) local = (16, 12, 24) invocations; bounds-clamp in shader.
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
}
