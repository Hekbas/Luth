#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/animation/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanShader.h"
#include "luth/renderer/backend/vulkan/DynamicRendering.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/Buffer.h"
#include "luth/renderer/lighting/IBLPrecompute.h"
#include "luth/renderer/passes/CullPass.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"
#include "luth/core/Math.h"
#include "luth/core/Time.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Components.h"
#include "luth/scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <vma/vk_mem_alloc.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <string>
#include <vector>

namespace Luth
{
    using namespace Component;

    RenderPipeline::RenderPipeline(RenderingSystem& system)
        : m_System(system)
    {
    }

    // =========================================================================
    //  Lifecycle — Initialize / Shutdown
    // =========================================================================

    void RenderPipeline::Initialize(u32 viewportWidth, u32 viewportHeight)
    {
        if (Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan) return;

        auto& s = m_System;
        m_System.m_Targets.Allocate(viewportWidth, viewportHeight);

        InitGlobalUniforms();
        InitShadowResources();

        // Load all engine shaders through the asset pipeline. Each file is a
        // single-stage asset; pipelines combine stages at creation time.
        // LoadEngine is idempotent and registers the shader in the library
        // keyed by filename (e.g. "pbr.vert").
        auto loadSpv = [](const char* relPath) -> std::vector<u32>
        {
            auto sh = ShaderLibrary::LoadEngine(relPath);
            return sh ? sh->GetSpirV() : std::vector<u32>{};
        };

        m_PBRVertSpv                  = loadSpv("shaders/pbr.vert");
        m_PBRFragSpv                  = loadSpv("shaders/pbr.frag");
        m_ShadowVertSpv               = loadSpv("shaders/shadowDepth.vert");
        m_ShadowFragSpv               = loadSpv("shaders/shadowDepth.frag");
        m_PBRSkinnedVertSpv           = loadSpv("shaders/pbr_skinned.vert");
        m_ShadowSkinnedVertSpv        = loadSpv("shaders/shadowDepth_skinned.vert");
        m_SelectionMaskVertSpv        = loadSpv("shaders/selectionMask.vert");
        m_SelectionMaskFragSpv        = loadSpv("shaders/selectionMask.frag");
        m_SelectionMaskSkinnedVertSpv = loadSpv("shaders/selectionMask_skinned.vert");
        m_DepthPrepassVertSpv         = loadSpv("shaders/depthPrepass.vert");
        m_DepthPrepassSkinnedVertSpv  = loadSpv("shaders/depthPrepass_skinned.vert");
        m_FullscreenVertSpv           = loadSpv("shaders/fullscreen.vert");
        m_BloomExtractFragSpv         = loadSpv("shaders/bloomExtract.frag");
        m_BloomBlurFragSpv            = loadSpv("shaders/bloomBlur.frag");
        m_PostProcessFragSpv          = loadSpv("shaders/postprocess.frag");
        m_OutlineFragSpv              = loadSpv("shaders/outline.frag");
        m_GridFragSpv                 = loadSpv("shaders/grid.frag");

        if (m_PBRVertSpv.empty() || m_PBRFragSpv.empty() ||
            m_ShadowVertSpv.empty() || m_ShadowFragSpv.empty() ||
            m_PBRSkinnedVertSpv.empty() || m_ShadowSkinnedVertSpv.empty() ||
            m_SelectionMaskVertSpv.empty() || m_SelectionMaskFragSpv.empty() ||
            m_SelectionMaskSkinnedVertSpv.empty() ||
            m_DepthPrepassVertSpv.empty() || m_DepthPrepassSkinnedVertSpv.empty() ||
            m_FullscreenVertSpv.empty() || m_BloomExtractFragSpv.empty() ||
            m_BloomBlurFragSpv.empty() || m_PostProcessFragSpv.empty() ||
            m_OutlineFragSpv.empty() || m_GridFragSpv.empty())
        {
            LH_CORE_ERROR("Engine shader SPIR-V empty after asset load!");
            return;
        }

        BoneMatrixBuffer::Init();
        InitPostProcessResources();
        InitIBLResources(FileSystem::ResolveAsset("textures/environment.hdr"));
        CreatePipelines();
        InitGPUObjectBuffers();
        InitCullPipeline();
        InitAOResources();

        // Shader hot-reload callback: pulls fresh SPIR-V into the cached blob
        // and rebuilds pipelines that use it. Fires after ShaderLibrary::Reload
        // has already recompiled and re-reflected the single-stage shader.
        // Library keys are the shader filename (e.g. "pbr.vert", "gtao_main.comp").
        ShaderLibrary::SetReloadCallback([this](const std::string& name) {
            vkDeviceWaitIdle(VulkanContext::Get().GetDevice());

            auto vk = std::static_pointer_cast<VulkanShader>(ShaderLibrary::Get(name));
            if (!vk || !vk->IsValid())
            {
                LH_CORE_ERROR("Shader reload: '{}' invalid — keeping existing pipelines", name);
                return;
            }
            const auto& spv = vk->GetSpirV();

            // Pull fresh SPIR-V into the cached blob used by pipeline builders.
            if      (name == "pbr.vert")                   m_PBRVertSpv                  = spv;
            else if (name == "pbr.frag")                   m_PBRFragSpv                  = spv;
            else if (name == "pbr_skinned.vert")           m_PBRSkinnedVertSpv           = spv;
            else if (name == "shadowDepth.vert")           m_ShadowVertSpv               = spv;
            else if (name == "shadowDepth.frag")           m_ShadowFragSpv               = spv;
            else if (name == "shadowDepth_skinned.vert")   m_ShadowSkinnedVertSpv        = spv;
            else if (name == "selectionMask.vert")         m_SelectionMaskVertSpv        = spv;
            else if (name == "selectionMask.frag")         m_SelectionMaskFragSpv        = spv;
            else if (name == "selectionMask_skinned.vert") m_SelectionMaskSkinnedVertSpv = spv;
            else if (name == "depthPrepass.vert")          m_DepthPrepassVertSpv         = spv;
            else if (name == "depthPrepass_skinned.vert")  m_DepthPrepassSkinnedVertSpv  = spv;
            else if (name == "fullscreen.vert")            m_FullscreenVertSpv           = spv;
            else if (name == "bloomExtract.frag")          m_BloomExtractFragSpv         = spv;
            else if (name == "bloomBlur.frag")             m_BloomBlurFragSpv            = spv;
            else if (name == "postprocess.frag")           m_PostProcessFragSpv          = spv;
            else if (name == "outline.frag")               m_OutlineFragSpv              = spv;
            else if (name == "grid.frag")                  m_GridFragSpv                 = spv;
            else if (name == "skybox.vert")                m_SkyboxVertSpv               = spv;
            else if (name == "skybox.frag")                m_SkyboxFragSpv               = spv;
            else if (name == "gtao_depth_prefilter.comp")  m_GTAOPrefilterSpv            = spv;
            else if (name == "gtao_main.comp")             m_GTAOMainSpv                 = spv;
            else if (name == "gtao_denoise.comp")          m_GTAODenoiseSpv              = spv;
            else if (name == "debugBlit.frag")             m_System.m_FrameDebugger.blitFragSpv  = spv;
            else if (name == "debugDepth.frag")            m_System.m_FrameDebugger.depthFragSpv = spv;
            // gpu_cull.comp: pipeline rebuilt below using `spv` directly
            // IBL precompute shaders (equirect/irradiance/prefilter/brdf_lut):
            // library entries refresh, but the precomputed results don't
            // re-bake on edit — ReloadSkybox() must be invoked for that.

            // Rebuild graphics pipelines (invalidate PBR cache by its canonical key).
            const bool isPBR = (name == "pbr.vert" || name == "pbr.frag");
            if (isPBR) {
                UUID pbrKey = ShaderLibrary::Get("pbr.vert")->Handle;
                m_GeoPipelineManager.InvalidateShader(pbrKey);
                m_GeoSkinnedPipelineManager.InvalidateShader(pbrKey);
            } else {
                m_GeoPipelineManager.Clear();
                m_GeoSkinnedPipelineManager.Clear();
            }
            m_ShadowPipeline.reset();
            m_ShadowSkinnedPipeline.reset();
            m_DepthPrepassPipeline.reset();
            m_DepthPrepassSkinnedPipeline.reset();
            m_SkyboxPipeline.reset();
            m_BloomExtractPipeline.reset();
            m_BloomBlurPipeline.reset();
            m_PostProcessPipeline.reset();
            m_OutlinePipeline.reset();
            m_GridPipeline.reset();
            m_SelectionMaskPipeline.reset();
            m_SelectionMaskSkinnedPipeline.reset();
            CreatePipelines();

            // Rebuild the matching compute pipeline (descriptor layouts untouched).
            if (name == "gpu_cull.comp" && m_CullDescLayout)
            {
                VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Vec4) * 6 + sizeof(u32) * 2 };
                m_CullPipeline = std::make_unique<VKComputePipeline>(spv,
                    std::vector<VkDescriptorSetLayout>{ m_CullDescLayout },
                    std::vector<VkPushConstantRange>{ pc });
            }
            else if (name == "gtao_depth_prefilter.comp" && m_GTAOPrefilterDescLayout)
            {
                VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(i32) * 2 + sizeof(float) * 6 };
                m_GTAOPrefilterPipeline = std::make_unique<VKComputePipeline>(m_GTAOPrefilterSpv,
                    std::vector<VkDescriptorSetLayout>{ m_GTAOPrefilterDescLayout },
                    std::vector<VkPushConstantRange>{ pc });
            }
            else if (name == "gtao_main.comp" && m_GTAOMainDescLayout)
            {
                VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 4 + sizeof(u32) * 4 };
                m_GTAOMainPipeline = std::make_unique<VKComputePipeline>(m_GTAOMainSpv,
                    std::vector<VkDescriptorSetLayout>{ m_GTAOMainDescLayout },
                    std::vector<VkPushConstantRange>{ pc });
            }
            else if (name == "gtao_denoise.comp" && m_GTAODenoiseDescLayout)
            {
                m_GTAODenoisePipeline = std::make_unique<VKComputePipeline>(m_GTAODenoiseSpv,
                    std::vector<VkDescriptorSetLayout>{ m_GTAODenoiseDescLayout },
                    std::vector<VkPushConstantRange>{});
            }

            LH_CORE_INFO("Pipelines rebuilt after shader reload: {}", name);
        });

        // File watcher for shader hot-reload (fires on background thread).
        m_System.m_ShaderWatcher.AddWatch(FileSystem::EngineAssetsPath("shaders"));
        m_System.m_ShaderWatcher.SetCallback([this](const fs::path& changedFile, FileWatcher::FileStatus status) {
            if (status != FileWatcher::FileStatus::Modified) return;

            std::string ext = changedFile.extension().string();
            if (ext != ".vert" && ext != ".frag" && ext != ".comp") return;

            std::string fileName = changedFile.filename().string();
            for (const auto& [name, shader] : ShaderLibrary::GetAll())
            {
                if (shader->GetPath().filename().string() == fileName)
                {
                    std::lock_guard lock(m_System.m_ReloadMutex);
                    m_System.m_PendingReloads.insert(name);
                    return;
                }
            }
            // No library match — asset database should have scanned the file.
            // Skip: there's no valid recovery path from here.
            LH_CORE_WARN("Shader watcher: changed file '{}' not registered in ShaderLibrary", fileName);
        });
        m_System.m_ShaderWatcher.Start(true);

        // Capacity covers worst-case current frame (5 cull + 4 shadow cascades +
        // geometry + selection + skybox + 3 bloom + grid + post-process + outline
        // + ImGui ≈ 19 passes) with headroom for future passes (GTAO etc.).
        m_GPUTimers.Init(64);
        RegisterNamedTextures();
    }

    void RenderPipeline::Shutdown()
    {
        auto& s = m_System;

        m_System.m_ShaderWatcher.Stop();
        ShaderLibrary::SetReloadCallback(nullptr);
        m_GPUTimers.Shutdown();

        BoneMatrixBuffer::Shutdown();

        VkDevice device = VulkanContext::Get().GetDevice();

        DestroyPerDrawPreviewTexture();
        DestroyDepthPreviewTexture();
        m_System.m_FrameDebugger.Shutdown(device);

        if (m_OutlineSampler)       vkDestroySampler(device, m_OutlineSampler, nullptr);
        if (m_OutlineDescSetLayout) vkDestroyDescriptorSetLayout(device, m_OutlineDescSetLayout, nullptr);
        if (m_OutlineDescPool)      vkDestroyDescriptorPool(device, m_OutlineDescPool, nullptr);
        if (m_GridDepthSampler)     vkDestroySampler(device, m_GridDepthSampler, nullptr);
        if (m_GridDescSetLayout)    vkDestroyDescriptorSetLayout(device, m_GridDescSetLayout, nullptr);
        if (m_GridDescPool)         vkDestroyDescriptorPool(device, m_GridDescPool, nullptr);
        if (m_PPSampler)            vkDestroySampler(device, m_PPSampler, nullptr);
        if (m_PPDescSetLayout)      vkDestroyDescriptorSetLayout(device, m_PPDescSetLayout, nullptr);
        if (m_PPDescPool)           vkDestroyDescriptorPool(device, m_PPDescPool, nullptr);
        if (m_IBLSampler)           vkDestroySampler(device, m_IBLSampler, nullptr);
        if (m_ShadowSampler)        vkDestroySampler(device, m_ShadowSampler, nullptr);
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i) {
            if (m_ShadowLayerViews[i]) vkDestroyImageView(device, m_ShadowLayerViews[i], nullptr);
        }
        if (m_LightSetLayout)  vkDestroyDescriptorSetLayout(device, m_LightSetLayout, nullptr);
        if (m_LightDescPool)   vkDestroyDescriptorPool(device, m_LightDescPool, nullptr);
        if (m_GlobalSetLayout) vkDestroyDescriptorSetLayout(device, m_GlobalSetLayout, nullptr);

        if (m_ObjectSSBO) {
            VulkanAllocator::Unmap(m_ObjectSSBOAlloc);
            VulkanAllocator::FreeBuffer(m_ObjectSSBO, m_ObjectSSBOAlloc);
        }
        if (m_IndirectBuffer) {
            VulkanAllocator::Unmap(m_IndirectBufferAlloc);
            VulkanAllocator::FreeBuffer(m_IndirectBuffer, m_IndirectBufferAlloc);
        }
        if (m_ObjectSSBODescPool)   vkDestroyDescriptorPool(device, m_ObjectSSBODescPool, nullptr);
        if (m_ObjectSSBODescLayout) vkDestroyDescriptorSetLayout(device, m_ObjectSSBODescLayout, nullptr);
        if (m_CullDescLayout)       vkDestroyDescriptorSetLayout(device, m_CullDescLayout, nullptr);
        m_CullPipeline.reset();

        // GTAO resources (epic #58).
        m_GTAOPrefilterPipeline.reset();
        m_GTAOMainPipeline.reset();
        m_GTAODenoisePipeline.reset();
        if (m_GTAOSampler)            vkDestroySampler(device, m_GTAOSampler, nullptr);
        if (m_GTAODescPool)           vkDestroyDescriptorPool(device, m_GTAODescPool, nullptr);
        if (m_GTAOPrefilterDescLayout) vkDestroyDescriptorSetLayout(device, m_GTAOPrefilterDescLayout, nullptr);
        if (m_GTAOMainDescLayout)      vkDestroyDescriptorSetLayout(device, m_GTAOMainDescLayout, nullptr);
        if (m_GTAODenoiseDescLayout)   vkDestroyDescriptorSetLayout(device, m_GTAODenoiseDescLayout, nullptr);
        // Textures + UBO destroyed automatically via shared_ptr reset when RenderingSystem dies.
    }

    void RenderPipeline::OnResize(u32 width, u32 height)
    {
        auto& s = m_System;

        m_BloomA = Texture::Create(std::max(width / 2, 1u), std::max(height / 2, 1u), TextureFormat::RGBA16F);
        m_BloomB = Texture::Create(std::max(width / 2, 1u), std::max(height / 2, 1u), TextureFormat::RGBA16F);
        UpdatePostProcessDescriptors();

        // GTAO half-res storage textures (recreated on resize; descriptors
        // refreshed below because they cache image-view pointers).
        {
            const u32 halfW = std::max(width  / 2, 1u);
            const u32 halfH = std::max(height / 2, 1u);
            auto makeStorage = [&](TextureFormat fmt) {
                return std::make_shared<VKTexture>(halfW, halfH, fmt, 1u, 0u, 1u, VK_IMAGE_USAGE_STORAGE_BIT);
            };
            m_GTAOLinearDepth = makeStorage(TextureFormat::R32_Float);
            m_GTAORawAO       = makeStorage(TextureFormat::R8);
            m_GTAOEdges       = makeStorage(TextureFormat::R8);
            m_GTAOFinal       = makeStorage(TextureFormat::R8);
        }
        UpdateAODescriptors();

        // Update outline descriptors — mask + depth buffer views changed.
        if (m_OutlineDescSet && m_OutlineSampler)
        {
            auto vkMask       = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSelectionMask());
            auto vkSelDepth   = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSelectionDepth());
            auto vkSceneDepth = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneDepth());

            VkDescriptorImageInfo maskImgInfo{};
            maskImgInfo.sampler     = m_OutlineSampler;
            maskImgInfo.imageView   = vkMask->GetImageView();
            maskImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo selDepthImgInfo{};
            selDepthImgInfo.sampler     = m_OutlineSampler;
            selDepthImgInfo.imageView   = vkSelDepth->GetImageView();
            selDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo sceneDepthImgInfo{};
            sceneDepthImgInfo.sampler     = m_OutlineSampler;
            sceneDepthImgInfo.imageView   = vkSceneDepth->GetImageView();
            sceneDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3] = {};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_OutlineDescSet;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &maskImgInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = m_OutlineDescSet;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &selDepthImgInfo;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = m_OutlineDescSet;
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &sceneDepthImgInfo;

            vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 3, writes, 0, nullptr);
        }

        // Update grid descriptor set: scene depth view changed on resize.
        if (m_GridDescSet && m_GridDepthSampler)
        {
            auto vkSceneDepth = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneDepth());

            VkDescriptorImageInfo gridDepthImgInfo{};
            gridDepthImgInfo.sampler     = m_GridDepthSampler;
            gridDepthImgInfo.imageView   = vkSceneDepth->GetImageView();
            gridDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet gridWrite{};
            gridWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gridWrite.dstSet = m_GridDescSet;
            gridWrite.dstBinding = 1;
            gridWrite.descriptorCount = 1;
            gridWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gridWrite.pImageInfo = &gridDepthImgInfo;

            vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &gridWrite, 0, nullptr);
        }

        RegisterNamedTextures();
    }

    void RenderPipeline::ExecuteMinimal()
    {
        auto& s = m_System;
        RG::RenderGraph rg(*m_System.m_FrameAllocator);
        AddImGuiPass(rg, RG::ResourceHandle{}); // invalid → ImGuiPass skips the optional Read
        rg.Compile();
        Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex(), nullptr);
    }

    void RenderPipeline::Execute(entt::registry& registry)
    {
        auto& s = m_System;

        RG::RenderGraph rg(*m_System.m_FrameAllocator);

        // Import persistent buffers into the render graph for barrier tracking.
        RG::BufferDesc objDesc {
            "ObjectSSBO",
            RenderPipeline::k_MaxGPUObjects * sizeof(GPUObjectData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        };
        RG::BufferDesc indDesc {
            "IndirectBuffer",
            RenderPipeline::k_IndirectRegionCount * RenderPipeline::k_IndirectRegionStride * sizeof(VkDrawIndexedIndirectCommand),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
        };
        RG::BufferHandle hObjectBuf   = rg.ImportBuffer(objDesc, (void*)m_ObjectSSBO,    RG::ResourceState::Undefined);
        RG::BufferHandle hIndirectBuf = rg.ImportBuffer(indDesc, (void*)m_IndirectBuffer, RG::ResourceState::Undefined);

        // Frustum cull — 5 dispatches: camera region + 4 shadow cascade regions.
        // Each cascade uses its own light-space viewProj frustum so shadow casters
        // outside the camera frustum but inside the cascade still get rendered.
        {
            Frustum camFrustum = CreateFrustumFromCamera(m_CachedViewProj);
            AddCullComputePass(rg, hObjectBuf, hIndirectBuf,
                m_CullPipeline.get(), m_CullDescSet, camFrustum.planes, m_GPUObjectCount,
                /*destOffset*/ 0, "FrustumCull.Cam", &m_System.m_FrameDebugger);

            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            {
                Frustum cascadeFrustum = CreateFrustumFromCamera(m_System.m_Cascades.lightSpaceMatrix[i]);
                const u32 destOffset = (i + 1) * RenderPipeline::k_IndirectRegionStride;
                const std::string name = "FrustumCull.C" + std::to_string(i);
                AddCullComputePass(rg, hObjectBuf, hIndirectBuf,
                    m_CullPipeline.get(), m_CullDescSet, cascadeFrustum.planes, m_GPUObjectCount,
                    destOffset, name.c_str(), &m_System.m_FrameDebugger);
            }
        }

        RG::ResourceHandle shadowHandles[k_ShadowCascadeCount];
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            shadowHandles[i] = AddShadowPass(rg, registry, hIndirectBuf, i);

        // Z-prepass produces SceneDepth before forward shading. The render
        // graph can schedule it in parallel with the shadow cascades.
        RG::ResourceHandle prepassDepth = AddDepthPrepass(rg, registry, hIndirectBuf);

        // GTAO chain runs every frame so the Set 0 binding-4 sampler sees
        // a valid SHADER_READ_ONLY layout (the `gtao.enabled` flag in the
        // UBO is what disables the modulation inside pbr.frag). ~0.3-1 ms
        // on a mid-range GPU at 1080p; can be gated later if a cheaper
        // bypass path is worth the complexity.
        RG::ResourceHandle gtaoLinearDepth = AddGTAODepthPrefilterPass(rg, prepassDepth);
        RG::ResourceHandle gtaoRawAO       = AddGTAOMainPass(rg, gtaoLinearDepth);
        RG::ResourceHandle gtaoFinalAO     = AddGTAODenoisePass(rg, gtaoRawAO, gtaoLinearDepth);

        auto geoOutput                 = AddGeometryPass(rg, registry, shadowHandles, hIndirectBuf, prepassDepth);
        auto maskOutput                = AddSelectionMaskPass(rg, registry);
        RG::ResourceHandle skyboxColor = AddSkyboxPass(rg, geoOutput.color, geoOutput.depth);
        RG::ResourceHandle bloomResult = AddBloomPasses(rg, skyboxColor); // bloom reads PRE-grid color so grid lines don't bloom
        RG::ResourceHandle gridColor   = m_System.m_GridVisible
                                         ? AddGridPass(rg, skyboxColor, geoOutput.depth)
                                         : skyboxColor;
        RG::ResourceHandle ldrOutput   = AddPostProcessPass(rg, gridColor, bloomResult);
        RG::ResourceHandle finalOutput = AddOutlinePass(rg, ldrOutput, maskOutput, geoOutput.depth);
        AddImGuiPass(rg, finalOutput);

        rg.Compile();

        // Capture render graph snapshot for Frame Debugger panel
        m_GraphSnapshot = CaptureSnapshot(rg);

        // Read GPU timing from completed frames and fill snapshot
        std::vector<float> gpuTimes;
        u32 nonCulledCount = 0;
        for (auto& p : m_GraphSnapshot.passes)
            if (!p.culled) nonCulledCount++;

        m_GPUTimers.ReadResults(nonCulledCount, gpuTimes);
        float totalMs = 0.0f;
        u32 timerIdx = 0;
        for (auto& p : m_GraphSnapshot.passes)
        {
            if (p.culled) continue;
            if (timerIdx < (u32)gpuTimes.size())
            {
                p.gpuTimeMs = gpuTimes[timerIdx];
                if (gpuTimes[timerIdx] > 0.0f) totalMs += gpuTimes[timerIdx];
            }
            timerIdx++;
        }
        m_GraphSnapshot.totalGpuTimeMs = totalMs;

        // --- Phase 14B — Wire archive sink for the capture frame ---
        // The sink will copy each tracked render target into a fresh ArchivedImage
        // after each pass that writes it. Keep the tracked-RT set tight to bound
        // memory (~50 MB at 1080p for the v1 set). The sink is a no-op when state
        // != CaptureRequested, so re-checking here is sufficient.
        if (m_System.m_FrameDebugger.state == DebuggerState::CaptureRequested)
        {
            // Phase 14D — ensure the debug sampler exists for ImGui archive previews.
            // Idempotent: returns immediately once blitPipeline is set.
            InitDebugBlitResources();

            // Phase 14E — invalidate the per-draw replay cache. The cache
            // is keyed by (passIdx, drawIdx) which can collide across
            // captures even though the underlying scene state has changed
            // (camera moved → recapture → same passIdx/drawIdx but new
            // GPUObjectData / IndirectBuffer contents). Without this reset,
            // re-clicking the same draw after recapture would hit the
            // stale cached preview.
            m_PerDrawPreviewKey = UINT64_MAX;
            // Same for the Phase 14F depth-preview blit cache — keyed by
            // (archiveIdx, layer+1); recapture rebuilds archives at the
            // same indices, so without this reset, re-selecting a depth
            // pass would skip the re-blit and show the previous frame.
            m_DepthPreviewKey = UINT64_MAX;

            m_System.m_FrameDebugger.BeginCapture(VulkanContext::Get().GetDevice(),
                                            VulkanContext::Get().GetAllocator());
            m_System.m_FrameDebugger.RegisterTrackedRT("SceneColor");
            m_System.m_FrameDebugger.RegisterTrackedRT("SceneDepth");
            // Phase 13 ShadowPass imports per-cascade resources named
            // "ShadowMap.C<i>" (one per cascade, single-layer view onto
            // the shared 4-layer array). Track each variant so the sink
            // archives them — without this, cascade nodes have no
            // primary output and the panel shows "no output preview".
            for (u32 ci = 0; ci < k_ShadowCascadeCount; ++ci)
                m_System.m_FrameDebugger.RegisterTrackedRT("ShadowMap.C" + std::to_string(ci));
            m_System.m_FrameDebugger.RegisterTrackedRT("LDROutput");
            m_System.m_FrameDebugger.RegisterTrackedRT("EntityID");
            m_System.m_FrameDebugger.RegisterTrackedRT("BloomAFinal");
            m_System.m_FrameDebugger.RegisterTrackedRT("GTAOLinearDepth");
            m_System.m_FrameDebugger.RegisterTrackedRT("GTAORawAO");
            m_System.m_FrameDebugger.RegisterTrackedRT("GTAOFinal");
            rg.SetArchiveSink(&m_System.m_FrameDebugger);
        }

        Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex(), &m_GPUTimers);

        // --- Frame Debugger: Finalize capture and enter frozen state ---
        if (m_System.m_FrameDebugger.state == DebuggerState::CaptureRequested)
        {
            // Phase 14C — captured*Draws / drawLimit removed.
            // Per-draw replay (Phase 14E) re-derives draw inputs from the
            // CapturedDrawCall records + frozen indirect/object SSBOs.

            // Copy resource and timing info from the graph snapshot
            m_System.m_FrameDebugger.capturedFrame.resources      = m_GraphSnapshot.resources;
            m_System.m_FrameDebugger.capturedFrame.totalGpuTimeMs = m_GraphSnapshot.totalGpuTimeMs;

            // Copy per-pass GPU times into captured passes
            {
                u32 capturedIdx = 0;
                for (auto& ps : m_GraphSnapshot.passes)
                {
                    if (ps.culled) continue;
                    if (capturedIdx < m_System.m_FrameDebugger.capturedFrame.passes.size())
                        m_System.m_FrameDebugger.capturedFrame.passes[capturedIdx].gpuTimeMs = ps.gpuTimeMs;
                    capturedIdx++;
                }
            }

            // Snapshot capture-time camera viewProj for the Frozen-state
            // auto-recapture comparison (see top of Update).
            m_System.m_FrameDebugger.FinalizeCapture(m_CachedViewProj);

            // Phase 14F — stamp CSM state into the captured frame so the
            // cascade detail panel can show GPU-true values from the moment
            // of capture, even if the user later twiddles light settings.
            m_System.m_FrameDebugger.capturedFrame.cascadeSplitsViewZ = m_System.m_Cascades.splitsViewZ;
            m_System.m_FrameDebugger.capturedFrame.shadowBias         = m_System.m_ShadowParams.shadowBias;
            m_System.m_FrameDebugger.capturedFrame.shadowNormalBias   = m_System.m_ShadowParams.shadowNormalBias;
            m_System.m_FrameDebugger.capturedFrame.cascadeTexelSize   = m_System.m_Cascades.texelSize;
            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                m_System.m_FrameDebugger.capturedFrame.lightSpaceMatrix[i] = m_System.m_Cascades.lightSpaceMatrix[i];

            m_System.m_FrameDebugger.capturedFrame.valid = true;
            m_System.m_FrameDebugger.state               = DebuggerState::Frozen;
        }
    }

    RG::RenderGraphSnapshot RenderPipeline::CaptureSnapshot(const RG::RenderGraph& rg)
    {
        auto& s = m_System;

        RG::RenderGraphSnapshot snapshot;

        // Snapshot resources
        auto& resources = const_cast<RG::RenderGraph&>(rg).GetResources();
        snapshot.resources.reserve(resources.size());
        for (auto& res : resources)
        {
            RG::ResourceSnapshot rs;
            rs.name        = res.desc.name;
            rs.width       = res.desc.width;
            rs.height      = res.desc.height;
            rs.format      = res.desc.format;
            rs.isExternal  = res.external;
            rs.isTransient = res.isTransient;
            snapshot.resources.push_back(std::move(rs));
        }

        // Snapshot passes
        auto& passes = rg.GetPasses();
        snapshot.passes.reserve(passes.size());
        for (auto& pass : passes)
        {
            RG::PassSnapshot ps;
            ps.name                = pass.name;
            ps.culled              = pass.culled;
            ps.numColorAttachments = (u32)pass.colorAttachments.size();
            ps.hasDepth            = pass.hasDepth;

            for (auto& r : pass.reads)
            {
                RG::PassSnapshotResource sr;
                sr.index = r.index;
                sr.name  = (r.index > 0 && r.index <= resources.size()) ? resources[r.index - 1].desc.name : "?";
                ps.reads.push_back(std::move(sr));
            }

            for (auto& w : pass.writes)
            {
                RG::PassSnapshotResource sw;
                sw.index = w.index;
                sw.name  = (w.index > 0 && w.index <= resources.size()) ? resources[w.index - 1].desc.name : "?";
                ps.writes.push_back(std::move(sw));
            }

            // Compute primaryOutputIndex from first color write, or depth if depth-only pass
            if (!pass.colorAttachments.empty())
            {
                u32 idx = pass.colorAttachments[0].handle.index;
                if (idx > 0 && idx <= resources.size())
                    ps.primaryOutputIndex = (int)(idx - 1);
            }
            else if (pass.hasDepth)
            {
                u32 idx = pass.depthAttachment.handle.index;
                if (idx > 0 && idx <= resources.size())
                    ps.primaryOutputIndex = (int)(idx - 1);
            }

            snapshot.passes.push_back(std::move(ps));
        }

        // Compute geometry stats from the current DrawList (built before pass dispatch)
        u32 totalDraws = (u32)(m_System.m_DrawList.opaque.size() + m_System.m_DrawList.cutout.size() + m_System.m_DrawList.transparent.size());
        u32 totalIndices = 0;
        auto sumIndices = [&](const std::vector<DrawCommand>& draws) {
            for (auto& dc : draws)
            {
                if (!dc.model) continue;
                auto mesh = dc.model->GetMesh(dc.meshIndex);
                if (mesh && mesh->GetIndexBuffer())
                    totalIndices += mesh->GetIndexBuffer()->GetCount();
            }
        };
        sumIndices(m_System.m_DrawList.opaque);
        sumIndices(m_System.m_DrawList.cutout);
        sumIndices(m_System.m_DrawList.transparent);

        // Enrich per-pass pipeline state (known at RenderingSystem level, not RenderGraph)
        for (auto& ps : snapshot.passes)
        {
            if (ps.culled) continue;

            if (ps.name == "ShadowPass")
            {
                ps.depthTest = true; ps.depthWrite = true;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_FRONT_BIT;
                ps.shaderName = "shadowDepth";
                ps.drawCalls = totalDraws;
                ps.indices = totalIndices;
            }
            else if (ps.name == "GeometryPass")
            {
                ps.depthTest = true; ps.depthWrite = true;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_BACK_BIT;
                ps.shaderName = "pbr";
                ps.drawCalls = totalDraws;
                ps.indices = totalIndices;
            }
            else if (ps.name == "SkyboxPass")
            {
                ps.depthTest = true; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_BACK_BIT;
                ps.shaderName = "skybox";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "BloomExtract")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "bloomExtract";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "BloomBlurH" || ps.name == "BloomBlurV")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "bloomBlur";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "PostProcess")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "postprocess";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "ImGuiPass")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = true;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "imgui";
                ps.drawCalls = 0; ps.indices = 0; // ImGui manages its own draws
            }
        }

        return snapshot;
    }
    // =========================================================================
    // Initialization
    // =========================================================================

    void RenderPipeline::InitGlobalUniforms()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        m_GlobalUniformBuffer = std::make_shared<VKUniformBuffer>(sizeof(GlobalUniforms));

        // Set 0 layout: 0 = GlobalUBO, 1-3 = IBL samplers, 4 = GTAO sampler, 5 = GTAO UBO
        VkDescriptorSetLayoutBinding bindings[6] = {};

        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        // GTAO final-AO sampler (epic #58). Written by UpdateGlobalIBLSetDescriptors
        // once m_GTAOFinal is allocated (InitAOResources runs after InitGlobalUniforms).
        bindings[4].binding = 4;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[5].binding = 5;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[5].descriptorCount = 1;
        bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 6;
        layoutInfo.pBindings = bindings;

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_GlobalSetLayout);

        VulkanContext::Get().GetDescriptorAllocator().Allocate(m_GlobalSetLayout, m_GlobalDescriptorSet);

        // Write binding 0 (UBO) immediately; bindings 1-3 written after IBL init
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_GlobalUniformBuffer->GetVulkanBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(GlobalUniforms);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_GlobalDescriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    void RenderPipeline::InitShadowResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // --- Shadow map: k_ShadowResolution^2, D32_Float, k_ShadowCascadeCount-layer 2D array (Phase 13) ---
        m_ShadowMap = std::make_shared<VKTexture>(
            k_ShadowResolution, k_ShadowResolution, TextureFormat::D32_Float,
            k_ShadowCascadeCount, /*createFlags*/ 0u, /*mipLevels*/ 1u, /*extraUsage*/ 0u);

        // Per-layer 2D views for ShadowPass.Ci depth attachments (Phase 13C).
        auto shadowTexForViews = std::static_pointer_cast<VKTexture>(m_ShadowMap);
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            m_ShadowLayerViews[i] = shadowTexForViews->CreateLayerView(i);

        // --- Shadow sampler (PCF compare: less) ---
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        samplerInfo.compareEnable = VK_TRUE;
        samplerInfo.compareOp = VK_COMPARE_OP_LESS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &samplerInfo, nullptr, &m_ShadowSampler);

        // --- Light UBO ---
        m_LightUniformBuffer = std::make_shared<VKUniformBuffer>(sizeof(LightUniforms));

        // --- Set 3 descriptor layout: binding 0 = LightUBO, binding 1 = shadow sampler ---
        VkDescriptorSetLayoutBinding bindings[2] = {};

        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo lightLayoutInfo{};
        lightLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lightLayoutInfo.bindingCount = 2;
        lightLayoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &lightLayoutInfo, nullptr, &m_LightSetLayout);

        // --- Pool ---
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = 1;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_LightDescPool);

        // --- Allocate set ---
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_LightDescPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_LightSetLayout;
        vkAllocateDescriptorSets(device, &allocInfo, &m_LightDescSet);

        // --- Write descriptors ---
        VkDescriptorBufferInfo lightBufInfo{};
        lightBufInfo.buffer = m_LightUniformBuffer->GetVulkanBuffer();
        lightBufInfo.offset = 0;
        lightBufInfo.range = sizeof(LightUniforms);

        auto vkShadowTex = std::static_pointer_cast<VKTexture>(m_ShadowMap);
        VkDescriptorImageInfo shadowImgInfo{};
        shadowImgInfo.sampler     = m_ShadowSampler;
        shadowImgInfo.imageView   = vkShadowTex->GetImageView();
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_LightDescSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &lightBufInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_LightDescSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &shadowImgInfo;

        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    void RenderPipeline::InitPostProcessResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        u32 w = m_System.m_Targets.GetSceneColor()->GetWidth();
        u32 h = m_System.m_Targets.GetSceneColor()->GetHeight();

        // Bloom textures (half-res)
        m_BloomA = Texture::Create(std::max(w / 2, 1u), std::max(h / 2, 1u), TextureFormat::RGBA16F);
        m_BloomB = Texture::Create(std::max(w / 2, 1u), std::max(h / 2, 1u), TextureFormat::RGBA16F);

        // Post-process UBO
        m_PostProcessUBOBuffer = std::make_shared<VKUniformBuffer>(sizeof(PostProcessUBO));

        // Linear clamp sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        vkCreateSampler(device, &samplerInfo, nullptr, &m_PPSampler);

        // Descriptor set layout: [sampler2D, sampler2D, UBO]
        VkDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_PPDescSetLayout);

        // Descriptor pool: 4 sets × (2 samplers + 1 UBO)
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 8; // 4 sets × 2 sampler bindings
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[1].descriptorCount = 4; // 4 sets × 1 UBO binding

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 4;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_PPDescPool);

        // Allocate 4 descriptor sets
        VkDescriptorSetLayout setLayouts[4] = { m_PPDescSetLayout, m_PPDescSetLayout, m_PPDescSetLayout, m_PPDescSetLayout };
        VkDescriptorSet sets[4];
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_PPDescPool;
        allocInfo.descriptorSetCount = 4;
        allocInfo.pSetLayouts = setLayouts;
        vkAllocateDescriptorSets(device, &allocInfo, sets);

        m_BloomExtractDescSet = sets[0];
        m_BloomBlurHDescSet   = sets[1];
        m_BloomBlurVDescSet   = sets[2];
        m_CompositeDescSet    = sets[3];

        UpdatePostProcessDescriptors();

        // ---- Outline pass resources ----
        {
            // Nearest-neighbor sampler for mask and depth textures
            VkSamplerCreateInfo outlineSamplerInfo{};
            outlineSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            outlineSamplerInfo.magFilter = VK_FILTER_NEAREST;
            outlineSamplerInfo.minFilter = VK_FILTER_NEAREST;
            outlineSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &outlineSamplerInfo, nullptr, &m_OutlineSampler);

            // Descriptor set layout: 3 sampler bindings
            VkDescriptorSetLayoutBinding bindings[3] = {};
            // Binding 0: sampler2D (selection mask)
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            // Binding 1: sampler2D (selection depth)
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            // Binding 2: sampler2D (scene depth)
            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo outlineLayoutInfo{};
            outlineLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            outlineLayoutInfo.bindingCount = 3;
            outlineLayoutInfo.pBindings = bindings;
            vkCreateDescriptorSetLayout(device, &outlineLayoutInfo, nullptr, &m_OutlineDescSetLayout);

            // Descriptor pool: 1 set, 3 combined image samplers
            VkDescriptorPoolSize outlinePoolSize{};
            outlinePoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            outlinePoolSize.descriptorCount = 3;

            VkDescriptorPoolCreateInfo outlinePoolInfo{};
            outlinePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            outlinePoolInfo.maxSets = 1;
            outlinePoolInfo.poolSizeCount = 1;
            outlinePoolInfo.pPoolSizes = &outlinePoolSize;
            vkCreateDescriptorPool(device, &outlinePoolInfo, nullptr, &m_OutlineDescPool);

            // Allocate descriptor set
            VkDescriptorSetAllocateInfo outlineAllocInfo{};
            outlineAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            outlineAllocInfo.descriptorPool = m_OutlineDescPool;
            outlineAllocInfo.descriptorSetCount = 1;
            outlineAllocInfo.pSetLayouts = &m_OutlineDescSetLayout;
            vkAllocateDescriptorSets(device, &outlineAllocInfo, &m_OutlineDescSet);

            // Write all 3 descriptors: selection mask, selection depth, scene depth
            auto vkMask      = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSelectionMask());
            auto vkSelDepth  = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSelectionDepth());
            auto vkScnDepth  = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneDepth());

            VkDescriptorImageInfo maskImgInfo{};
            maskImgInfo.sampler     = m_OutlineSampler;
            maskImgInfo.imageView   = vkMask->GetImageView();
            maskImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo selDepthImgInfo{};
            selDepthImgInfo.sampler     = m_OutlineSampler;
            selDepthImgInfo.imageView   = vkSelDepth->GetImageView();
            selDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo scnDepthImgInfo{};
            scnDepthImgInfo.sampler     = m_OutlineSampler;
            scnDepthImgInfo.imageView   = vkScnDepth->GetImageView();
            scnDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3] = {};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_OutlineDescSet;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &maskImgInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = m_OutlineDescSet;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &selDepthImgInfo;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = m_OutlineDescSet;
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &scnDepthImgInfo;

            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }

        // ---- Grid pass resources ----
        {
            // Linear sampler for scene depth read (matching behaviour used elsewhere for depth texture reads)
            VkSamplerCreateInfo gridSamplerInfo{};
            gridSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            gridSamplerInfo.magFilter = VK_FILTER_NEAREST;
            gridSamplerInfo.minFilter = VK_FILTER_NEAREST;
            gridSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &gridSamplerInfo, nullptr, &m_GridDepthSampler);

            // Descriptor set layout: binding 0 = GlobalUBO (camera), binding 1 = scene depth sampler
            VkDescriptorSetLayoutBinding gridBindings[2] = {};
            gridBindings[0].binding = 0;
            gridBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            gridBindings[0].descriptorCount = 1;
            gridBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            gridBindings[1].binding = 1;
            gridBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gridBindings[1].descriptorCount = 1;
            gridBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo gridLayoutInfo{};
            gridLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            gridLayoutInfo.bindingCount = 2;
            gridLayoutInfo.pBindings = gridBindings;
            vkCreateDescriptorSetLayout(device, &gridLayoutInfo, nullptr, &m_GridDescSetLayout);

            // Descriptor pool: 1 set (UBO + sampler)
            VkDescriptorPoolSize gridPoolSizes[2] = {};
            gridPoolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            gridPoolSizes[0].descriptorCount = 1;
            gridPoolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gridPoolSizes[1].descriptorCount = 1;

            VkDescriptorPoolCreateInfo gridPoolInfo{};
            gridPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            gridPoolInfo.maxSets = 1;
            gridPoolInfo.poolSizeCount = 2;
            gridPoolInfo.pPoolSizes = gridPoolSizes;
            vkCreateDescriptorPool(device, &gridPoolInfo, nullptr, &m_GridDescPool);

            VkDescriptorSetAllocateInfo gridAllocInfo{};
            gridAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            gridAllocInfo.descriptorPool = m_GridDescPool;
            gridAllocInfo.descriptorSetCount = 1;
            gridAllocInfo.pSetLayouts = &m_GridDescSetLayout;
            vkAllocateDescriptorSets(device, &gridAllocInfo, &m_GridDescSet);

            // Write: global UBO + scene depth
            VkDescriptorBufferInfo gridUBOInfo{};
            gridUBOInfo.buffer = m_GlobalUniformBuffer->GetVulkanBuffer();
            gridUBOInfo.offset = 0;
            gridUBOInfo.range  = sizeof(GlobalUniforms);

            auto vkScnDepthGrid = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneDepth());
            VkDescriptorImageInfo gridDepthImgInfo{};
            gridDepthImgInfo.sampler     = m_GridDepthSampler;
            gridDepthImgInfo.imageView   = vkScnDepthGrid->GetImageView();
            gridDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet gridWrites[2] = {};
            gridWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gridWrites[0].dstSet = m_GridDescSet;
            gridWrites[0].dstBinding = 0;
            gridWrites[0].descriptorCount = 1;
            gridWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            gridWrites[0].pBufferInfo = &gridUBOInfo;

            gridWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gridWrites[1].dstSet = m_GridDescSet;
            gridWrites[1].dstBinding = 1;
            gridWrites[1].descriptorCount = 1;
            gridWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gridWrites[1].pImageInfo = &gridDepthImgInfo;

            vkUpdateDescriptorSets(device, 2, gridWrites, 0, nullptr);
        }
    }

    void RenderPipeline::UpdatePostProcessDescriptors()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        auto sceneVk  = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneColor());
        auto bloomAVk = std::static_pointer_cast<VKTexture>(m_BloomA);
        auto bloomBVk = std::static_pointer_cast<VKTexture>(m_BloomB);

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = m_PostProcessUBOBuffer->GetVulkanBuffer();
        uboInfo.offset = 0;
        uboInfo.range  = sizeof(PostProcessUBO);

        // Helper: write a combined image sampler descriptor
        auto MakeImageInfo = [this](VkImageView view) -> VkDescriptorImageInfo {
            VkDescriptorImageInfo info{};
            info.sampler     = m_PPSampler;
            info.imageView   = view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return info;
        };

        // BloomExtract: binding 0 = SceneColor, binding 1 = unused (BloomA as placeholder), binding 2 = UBO
        VkDescriptorImageInfo bloomExtractImg0 = MakeImageInfo(sceneVk->GetImageView());
        VkDescriptorImageInfo bloomExtractImg1 = MakeImageInfo(bloomAVk->GetImageView());

        // BloomBlurH: binding 0 = BloomA, binding 1 = unused, binding 2 = UBO
        VkDescriptorImageInfo blurHImg0 = MakeImageInfo(bloomAVk->GetImageView());
        VkDescriptorImageInfo blurHImg1 = MakeImageInfo(bloomBVk->GetImageView());

        // BloomBlurV: binding 0 = BloomB, binding 1 = unused, binding 2 = UBO
        VkDescriptorImageInfo blurVImg0 = MakeImageInfo(bloomBVk->GetImageView());
        VkDescriptorImageInfo blurVImg1 = MakeImageInfo(bloomAVk->GetImageView());

        // Composite: binding 0 = SceneColor (HDR), binding 1 = BloomA (blurred), binding 2 = UBO
        VkDescriptorImageInfo compImg0 = MakeImageInfo(sceneVk->GetImageView());
        VkDescriptorImageInfo compImg1 = MakeImageInfo(bloomAVk->GetImageView());

        // Write all 4 sets (3 writes each = 12 total)
        VkWriteDescriptorSet writes[12] = {};
        int idx = 0;

        auto AddWrite = [&](VkDescriptorSet set, u32 binding, VkDescriptorType type,
                           VkDescriptorImageInfo* imgInfo, VkDescriptorBufferInfo* bufInfo) {
            writes[idx].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[idx].dstSet = set;
            writes[idx].dstBinding = binding;
            writes[idx].descriptorCount = 1;
            writes[idx].descriptorType = type;
            writes[idx].pImageInfo = imgInfo;
            writes[idx].pBufferInfo = bufInfo;
            idx++;
        };

        // BloomExtract set
        AddWrite(m_BloomExtractDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bloomExtractImg0, nullptr);
        AddWrite(m_BloomExtractDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bloomExtractImg1, nullptr);
        AddWrite(m_BloomExtractDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // BloomBlurH set
        AddWrite(m_BloomBlurHDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurHImg0, nullptr);
        AddWrite(m_BloomBlurHDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurHImg1, nullptr);
        AddWrite(m_BloomBlurHDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // BloomBlurV set
        AddWrite(m_BloomBlurVDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurVImg0, nullptr);
        AddWrite(m_BloomBlurVDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurVImg1, nullptr);
        AddWrite(m_BloomBlurVDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // Composite set
        AddWrite(m_CompositeDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &compImg0, nullptr);
        AddWrite(m_CompositeDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &compImg1, nullptr);
        AddWrite(m_CompositeDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        vkUpdateDescriptorSets(device, idx, writes, 0, nullptr);
    }

    void RenderPipeline::InitIBLResources(const fs::path& hdrPath)
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Run precomputation (equirect -> cubemap -> irradiance -> prefilter -> BRDF LUT)
        IBLResult ibl = IBL::Precompute(hdrPath);

        m_IrradianceMap  = ibl.irradianceMap;
        m_PrefilteredMap = ibl.prefilteredMap;
        m_BRDFLut        = ibl.brdfLut;
        m_IBLSampler     = ibl.iblSampler;
        m_SkyboxVB       = ibl.skyboxVB;
        m_SkyboxVertSpv  = std::move(ibl.skyboxVertSpv);
        m_SkyboxFragSpv  = std::move(ibl.skyboxFragSpv);

        // Write IBL descriptors to Set 0 (bindings 1-3)
        {
            auto vkIrr = std::static_pointer_cast<VKTexture>(m_IrradianceMap);
            auto vkPf  = std::static_pointer_cast<VKTexture>(m_PrefilteredMap);
            auto vkLut = std::static_pointer_cast<VKTexture>(m_BRDFLut);

            VkDescriptorImageInfo irrInfo{};
            irrInfo.sampler = m_IBLSampler;
            irrInfo.imageView = vkIrr->GetImageView();
            irrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo pfInfo{};
            pfInfo.sampler = m_IBLSampler;
            pfInfo.imageView = vkPf->GetImageView();
            pfInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo lutInfo{};
            lutInfo.sampler = m_IBLSampler;
            lutInfo.imageView = vkLut->GetImageView();
            lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3] = {};
            writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[0].dstSet = m_GlobalDescriptorSet;
            writes[0].dstBinding = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &irrInfo;

            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[1].dstSet = m_GlobalDescriptorSet;
            writes[1].dstBinding = 2;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &pfInfo;

            writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[2].dstSet = m_GlobalDescriptorSet;
            writes[2].dstBinding = 3;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].descriptorCount = 1;
            writes[2].pImageInfo = &lutInfo;

            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }
    }

    void RenderPipeline::ReloadSkybox(const fs::path& hdrPath)
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        vkDeviceWaitIdle(device);

        // Destroy old IBL sampler (textures freed by shared_ptr reset in InitIBLResources)
        if (m_IBLSampler) {
            vkDestroySampler(device, m_IBLSampler, nullptr);
            m_IBLSampler = VK_NULL_HANDLE;
        }

        InitIBLResources(hdrPath);

        // Rebuild skybox pipeline (new prefiltered map may have different mip count)
        m_SkyboxPipeline.reset();
        CreatePipelines();

        LH_CORE_INFO("Skybox reloaded from '{}'", hdrPath.string());
    }

    // =========================================================================
    // Pipeline creation
    // =========================================================================

    void RenderPipeline::InitObjectSSBODescriptorLayout()
    {
        if (m_ObjectSSBODescLayout != VK_NULL_HANDLE)
            return;

        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &binding;

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_ObjectSSBODescLayout);
    }

    void RenderPipeline::CreatePipelines()
    {
        // Ensure Set 5 descriptor layout exists (idempotent — safe to call on hot-reload)
        InitObjectSSBODescriptorLayout();

        // Full vertex layout (matches Model::ProcessMeshData)
        BufferLayout vertexLayout = {
            { ShaderDataType::Float3, "a_Position"  },
            { ShaderDataType::Float3, "a_Normal"    },
            { ShaderDataType::Float2, "a_TexCoord0" },
            { ShaderDataType::Float2, "a_TexCoord1" },
            { ShaderDataType::Float3, "a_Tangent"   }
        };

        auto bindingDescs = vertexLayout.GetBindingDescriptions();
        auto attribDescs  = vertexLayout.GetAttributeDescriptions();

        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(ObjectPushConstants);

        // 5-set layout for shadow / selection-mask / skybox pipelines (push constants remain)
        std::vector<VkDescriptorSetLayout> layouts = {
            m_GlobalSetLayout,                                    // Set 0
            VulkanContext::Get().GetBindlessSet().GetLayout(),   // Set 1
            MaterialSystem::GetDescriptorSetLayout(),            // Set 2
            m_LightSetLayout,                                    // Set 3
            BoneMatrixBuffer::GetDescriptorSetLayout()           // Set 4
        };

        // 6-set layout for geometry pipelines (adds Set 5 = GPUObjectData SSBO, no push constants)
        std::vector<VkDescriptorSetLayout> geoLayouts = {
            m_GlobalSetLayout,                                    // Set 0
            VulkanContext::Get().GetBindlessSet().GetLayout(),   // Set 1
            MaterialSystem::GetDescriptorSetLayout(),            // Set 2
            m_LightSetLayout,                                    // Set 3
            BoneMatrixBuffer::GetDescriptorSetLayout(),          // Set 4
            m_ObjectSSBODescLayout                               // Set 5
        };

        // ---- PBR geometry pipeline manager (lazy creation keyed by {shaderUUID, renderMode}) ----
        m_GeoPipelineManager.Init(geoLayouts,
            [bindingDescs, attribDescs](Material::RenderMode mode, Material::CullMode cullMode, VkPolygonMode polygonMode) -> PipelineConfig
            {
                PipelineConfig config;
                config.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_UINT };
                config.depthFormat = VK_FORMAT_D32_SFLOAT;
                config.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                config.bindingDescriptions = bindingDescs;
                config.attributeDescriptions = attribDescs;
                // No push constants — per-object data comes from GPUObjectData SSBO (Set 5)
                config.polygonMode = polygonMode;
                // LESS_OR_EQUAL so opaques pass the DepthPrepass values (written with LESS)
                // exactly, while cutouts/transparents still Z-test against the prepass depth.
                config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

                switch (mode)
                {
                    case Material::RenderMode::Opaque:
                        config.depthTest = true; config.depthWrite = true;
                        config.blendEnabled = false;
                        break;
                    case Material::RenderMode::Cutout:
                        config.depthTest = true; config.depthWrite = true;
                        config.blendEnabled = false;
                        break;
                    case Material::RenderMode::Transparent:
                    case Material::RenderMode::Fade:
                        config.depthTest = true; config.depthWrite = false;
                        config.blendEnabled = true;
                        break;
                }

                // Apply material cull mode
                switch (cullMode)
                {
                    case Material::CullMode::Back:  config.cullMode = VK_CULL_MODE_BACK_BIT;  break;
                    case Material::CullMode::Front: config.cullMode = VK_CULL_MODE_FRONT_BIT; break;
                    case Material::CullMode::None:  config.cullMode = VK_CULL_MODE_NONE;      break;
                }

                return config;
            });

        // ---- Shadow pipeline (depth-only, position attribute only) ----
        // Same stride as full vertex, but only declare the position attribute.
        BufferLayout shadowVertexLayout = {
            { ShaderDataType::Float3, "a_Position" }
        };
        // Override stride to match the full vertex size so the buffer reads are correct.
        auto shadowBindingDescs = shadowVertexLayout.GetBindingDescriptions();
        auto shadowAttribDescs  = shadowVertexLayout.GetAttributeDescriptions();
        // Fix stride: the real vertex buffer has stride = sizeof(Vertex) = 52 bytes.
        // BufferLayout calculates stride from declared attributes only.
        // We need to manually set the stride to 52 to match the actual buffer.
        if (!shadowBindingDescs.empty())
            shadowBindingDescs[0].stride = sizeof(float) * (3 + 3 + 2 + 2 + 3); // 52 bytes

        // 4-byte VERTEX push constant carries cascadeIndex (Phase 13C).
        VkPushConstantRange shadowCascadePC{};
        shadowCascadePC.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        shadowCascadePC.offset     = 0;
        shadowCascadePC.size       = sizeof(u32);

        PipelineConfig shadowConfig;
        shadowConfig.colorFormats = {};  // depth-only
        shadowConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
        shadowConfig.depthTest = true; shadowConfig.depthWrite = true;
        shadowConfig.blendEnabled = false;
        shadowConfig.cullMode = VK_CULL_MODE_FRONT_BIT; // front-face culling reduces shadow acne
        shadowConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        shadowConfig.bindingDescriptions = shadowBindingDescs;
        shadowConfig.attributeDescriptions = shadowAttribDescs;
        shadowConfig.pushConstantRanges = { shadowCascadePC };

        m_ShadowPipeline = std::make_unique<VKPipeline>(shadowConfig, m_ShadowVertSpv, m_ShadowFragSpv, geoLayouts);

        // ---- Skinned geometry pipeline manager ----
        BufferLayout skinnedVertexLayout = {
            { ShaderDataType::Float3, "a_Position"    },
            { ShaderDataType::Float3, "a_Normal"      },
            { ShaderDataType::Float2, "a_TexCoord0"   },
            { ShaderDataType::Float2, "a_TexCoord1"   },
            { ShaderDataType::Float3, "a_Tangent"     },
            { ShaderDataType::Int4,   "a_BoneIDs"     },
            { ShaderDataType::Float4, "a_BoneWeights" }
        };

        auto skinnedBindingDescs = skinnedVertexLayout.GetBindingDescriptions();
        auto skinnedAttribDescs  = skinnedVertexLayout.GetAttributeDescriptions();

        m_GeoSkinnedPipelineManager.Init(geoLayouts,
            [skinnedBindingDescs, skinnedAttribDescs](Material::RenderMode mode, Material::CullMode cullMode, VkPolygonMode polygonMode) -> PipelineConfig
            {
                PipelineConfig config;
                config.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_UINT };
                config.depthFormat = VK_FORMAT_D32_SFLOAT;
                config.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                config.bindingDescriptions = skinnedBindingDescs;
                config.attributeDescriptions = skinnedAttribDescs;
                // No push constants — per-object data comes from GPUObjectData SSBO (Set 5)
                config.polygonMode = polygonMode;
                config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // matches DepthPrepass output

                switch (mode)
                {
                    case Material::RenderMode::Opaque:
                        config.depthTest = true; config.depthWrite = true;
                        config.blendEnabled = false;
                        break;
                    case Material::RenderMode::Cutout:
                        config.depthTest = true; config.depthWrite = true;
                        config.blendEnabled = false;
                        break;
                    case Material::RenderMode::Transparent:
                    case Material::RenderMode::Fade:
                        config.depthTest = true; config.depthWrite = false;
                        config.blendEnabled = true;
                        break;
                }

                switch (cullMode)
                {
                    case Material::CullMode::Back:  config.cullMode = VK_CULL_MODE_BACK_BIT;  break;
                    case Material::CullMode::Front: config.cullMode = VK_CULL_MODE_FRONT_BIT; break;
                    case Material::CullMode::None:  config.cullMode = VK_CULL_MODE_NONE;      break;
                }

                return config;
            });

        // ---- Skinned shadow pipeline (depth-only, full skinned vertex stride) ----
        if (!m_ShadowSkinnedVertSpv.empty())
        {
            // Full skinned vertex layout — all 7 attributes declared so stride = 84 bytes
            PipelineConfig shadowSkinnedConfig;
            shadowSkinnedConfig.colorFormats = {};
            shadowSkinnedConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
            shadowSkinnedConfig.depthTest = true; shadowSkinnedConfig.depthWrite = true;
            shadowSkinnedConfig.blendEnabled = false;
            shadowSkinnedConfig.cullMode = VK_CULL_MODE_FRONT_BIT;
            shadowSkinnedConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            shadowSkinnedConfig.bindingDescriptions = skinnedBindingDescs;
            shadowSkinnedConfig.attributeDescriptions = skinnedAttribDescs;
            shadowSkinnedConfig.pushConstantRanges = { shadowCascadePC };

            m_ShadowSkinnedPipeline = std::make_unique<VKPipeline>(
                shadowSkinnedConfig, m_ShadowSkinnedVertSpv, m_ShadowFragSpv, geoLayouts);
        }

        // ---- Depth prepass pipeline (camera-space, depth-only, position only) ----
        // Reuses the shadow frag SPIR-V (empty `void main(){}`) as the null fragment.
        // Rigid variant uses the position-only binding/attribs (shadowVertexLayout).
        if (!m_DepthPrepassVertSpv.empty() && !m_ShadowFragSpv.empty())
        {
            PipelineConfig depthPrepassConfig;
            depthPrepassConfig.colorFormats = {}; // depth-only
            depthPrepassConfig.depthFormat  = VK_FORMAT_D32_SFLOAT;
            depthPrepassConfig.depthTest    = true;
            depthPrepassConfig.depthWrite   = true;
            depthPrepassConfig.depthCompareOp = VK_COMPARE_OP_LESS;
            depthPrepassConfig.blendEnabled = false;
            depthPrepassConfig.cullMode     = VK_CULL_MODE_BACK_BIT;
            depthPrepassConfig.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            depthPrepassConfig.bindingDescriptions   = shadowBindingDescs; // position-only + full-vertex stride
            depthPrepassConfig.attributeDescriptions = shadowAttribDescs;

            m_DepthPrepassPipeline = std::make_unique<VKPipeline>(
                depthPrepassConfig, m_DepthPrepassVertSpv, m_ShadowFragSpv, geoLayouts);
        }

        if (!m_DepthPrepassSkinnedVertSpv.empty() && !m_ShadowFragSpv.empty())
        {
            PipelineConfig depthPrepassSkinnedConfig;
            depthPrepassSkinnedConfig.colorFormats = {};
            depthPrepassSkinnedConfig.depthFormat  = VK_FORMAT_D32_SFLOAT;
            depthPrepassSkinnedConfig.depthTest    = true;
            depthPrepassSkinnedConfig.depthWrite   = true;
            depthPrepassSkinnedConfig.depthCompareOp = VK_COMPARE_OP_LESS;
            depthPrepassSkinnedConfig.blendEnabled = false;
            depthPrepassSkinnedConfig.cullMode     = VK_CULL_MODE_BACK_BIT;
            depthPrepassSkinnedConfig.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            depthPrepassSkinnedConfig.bindingDescriptions   = skinnedBindingDescs;
            depthPrepassSkinnedConfig.attributeDescriptions = skinnedAttribDescs;

            m_DepthPrepassSkinnedPipeline = std::make_unique<VKPipeline>(
                depthPrepassSkinnedConfig, m_DepthPrepassSkinnedVertSpv, m_ShadowFragSpv, geoLayouts);
        }

        // ---- Selection mask pipeline (static) ----
        if (!m_SelectionMaskVertSpv.empty() && !m_SelectionMaskFragSpv.empty())
        {
            PipelineConfig maskConfig;
            maskConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
            maskConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
            maskConfig.depthTest = true; maskConfig.depthWrite = true;
            maskConfig.blendEnabled = false;
            maskConfig.cullMode = VK_CULL_MODE_BACK_BIT;
            maskConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            maskConfig.bindingDescriptions = shadowBindingDescs;
            maskConfig.attributeDescriptions = shadowAttribDescs;
            maskConfig.pushConstantRanges = { pushConstantRange };

            m_SelectionMaskPipeline = std::make_unique<VKPipeline>(
                maskConfig, m_SelectionMaskVertSpv, m_SelectionMaskFragSpv, layouts);
        }

        // ---- Selection mask pipeline (skinned) ----
        if (!m_SelectionMaskSkinnedVertSpv.empty() && !m_SelectionMaskFragSpv.empty())
        {
            PipelineConfig maskSkinnedConfig;
            maskSkinnedConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
            maskSkinnedConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
            maskSkinnedConfig.depthTest = true; maskSkinnedConfig.depthWrite = true;
            maskSkinnedConfig.blendEnabled = false;
            maskSkinnedConfig.cullMode = VK_CULL_MODE_BACK_BIT;
            maskSkinnedConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            maskSkinnedConfig.bindingDescriptions = skinnedBindingDescs;
            maskSkinnedConfig.attributeDescriptions = skinnedAttribDescs;
            maskSkinnedConfig.pushConstantRanges = { pushConstantRange };

            m_SelectionMaskSkinnedPipeline = std::make_unique<VKPipeline>(
                maskSkinnedConfig, m_SelectionMaskSkinnedVertSpv, m_SelectionMaskFragSpv, layouts);
        }

        // ---- Skybox pipeline ----
        if (!m_SkyboxVertSpv.empty() && !m_SkyboxFragSpv.empty())
        {
            BufferLayout skyboxVertexLayout = {
                { ShaderDataType::Float3, "a_Position" }
            };

            PipelineConfig skyboxConfig;
            skyboxConfig.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
            skyboxConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
            skyboxConfig.depthTest = true;
            skyboxConfig.depthWrite = false;
            skyboxConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            skyboxConfig.blendEnabled = false;
            skyboxConfig.cullMode = VK_CULL_MODE_BACK_BIT; // Y-flipped projection reverses winding; cull back = show inside faces
            skyboxConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            skyboxConfig.bindingDescriptions = skyboxVertexLayout.GetBindingDescriptions();
            skyboxConfig.attributeDescriptions = skyboxVertexLayout.GetAttributeDescriptions();

            m_SkyboxPipeline = std::make_unique<VKPipeline>(skyboxConfig, m_SkyboxVertSpv, m_SkyboxFragSpv, layouts);
        }

        // ---- Post-process pipelines ----
        if (!m_FullscreenVertSpv.empty() && m_PPDescSetLayout != VK_NULL_HANDLE)
        {
            std::vector<VkDescriptorSetLayout> ppLayouts = { m_PPDescSetLayout };

            // Bloom extract: push constant = float threshold + pad
            if (!m_BloomExtractFragSpv.empty())
            {
                VkPushConstantRange bloomExtractPC{};
                bloomExtractPC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
                bloomExtractPC.offset = 0;
                bloomExtractPC.size = sizeof(float) * 4; // threshold + pad[3]

                PipelineConfig bloomExtractConfig;
                bloomExtractConfig.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
                bloomExtractConfig.depthFormat = VK_FORMAT_UNDEFINED;
                bloomExtractConfig.depthTest = false; bloomExtractConfig.depthWrite = false;
                bloomExtractConfig.blendEnabled = false;
                bloomExtractConfig.cullMode = VK_CULL_MODE_NONE;
                bloomExtractConfig.pushConstantRanges = { bloomExtractPC };
                m_BloomExtractPipeline = std::make_unique<VKPipeline>(
                    bloomExtractConfig, m_FullscreenVertSpv, m_BloomExtractFragSpv, ppLayouts);
            }

            // Bloom blur: push constant = vec2 direction + pad
            if (!m_BloomBlurFragSpv.empty())
            {
                VkPushConstantRange bloomBlurPC{};
                bloomBlurPC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
                bloomBlurPC.offset = 0;
                bloomBlurPC.size = sizeof(float) * 4; // direction.xy + pad[2]

                PipelineConfig bloomBlurConfig;
                bloomBlurConfig.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
                bloomBlurConfig.depthFormat = VK_FORMAT_UNDEFINED;
                bloomBlurConfig.depthTest = false; bloomBlurConfig.depthWrite = false;
                bloomBlurConfig.blendEnabled = false;
                bloomBlurConfig.cullMode = VK_CULL_MODE_NONE;
                bloomBlurConfig.pushConstantRanges = { bloomBlurPC };
                m_BloomBlurPipeline = std::make_unique<VKPipeline>(
                    bloomBlurConfig, m_FullscreenVertSpv, m_BloomBlurFragSpv, ppLayouts);
            }

            // PostProcess composite: no push constants, UBO at binding 2
            if (!m_PostProcessFragSpv.empty())
            {
                PipelineConfig ppConfig;
                ppConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM }; // LDR output
                ppConfig.depthFormat = VK_FORMAT_UNDEFINED;
                ppConfig.depthTest = false; ppConfig.depthWrite = false;
                ppConfig.blendEnabled = false;
                ppConfig.cullMode = VK_CULL_MODE_NONE;
                m_PostProcessPipeline = std::make_unique<VKPipeline>(
                    ppConfig, m_FullscreenVertSpv, m_PostProcessFragSpv, ppLayouts);
            }
        }

        // ---- Outline pipeline ----
        if (!m_FullscreenVertSpv.empty() && !m_OutlineFragSpv.empty() && m_OutlineDescSetLayout != VK_NULL_HANDLE)
        {
            std::vector<VkDescriptorSetLayout> outlineLayouts = { m_OutlineDescSetLayout };

            VkPushConstantRange outlinePC{};
            outlinePC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            outlinePC.offset = 0;
            outlinePC.size = sizeof(float) * 8; // outlineWidth + texelSize(vec2) + outlineColor(vec4) + occludedAlpha = 32 bytes

            PipelineConfig outlineConfig;
            outlineConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM }; // writes to LDR output with blending
            outlineConfig.depthFormat = VK_FORMAT_UNDEFINED;
            outlineConfig.depthTest = false; outlineConfig.depthWrite = false;
            outlineConfig.blendEnabled = true; // alpha blend the outline on top
            outlineConfig.cullMode = VK_CULL_MODE_NONE;
            outlineConfig.pushConstantRanges = { outlinePC };
            m_OutlinePipeline = std::make_unique<VKPipeline>(
                outlineConfig, m_FullscreenVertSpv, m_OutlineFragSpv, outlineLayouts);
        }

        // ---- Grid pipeline (editor-only infinite grid fullscreen pass) ----
        if (!m_FullscreenVertSpv.empty() && !m_GridFragSpv.empty() && m_GridDescSetLayout != VK_NULL_HANDLE)
        {
            std::vector<VkDescriptorSetLayout> gridLayouts = { m_GridDescSetLayout };

            VkPushConstantRange gridPC{};
            gridPC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            gridPC.offset = 0;
            gridPC.size = sizeof(float) * 16; // 3 vec4 colors + 4 floats = 16 floats = 64 bytes

            PipelineConfig gridConfig;
            gridConfig.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT }; // HDR scene color
            gridConfig.depthFormat = VK_FORMAT_UNDEFINED;
            gridConfig.depthTest = false; gridConfig.depthWrite = false;
            gridConfig.blendEnabled = true; // alpha-composite grid over scene
            gridConfig.cullMode = VK_CULL_MODE_NONE;
            gridConfig.pushConstantRanges = { gridPC };
            m_GridPipeline = std::make_unique<VKPipeline>(
                gridConfig, m_FullscreenVertSpv, m_GridFragSpv, gridLayouts);
        }
    }

    void RenderPipeline::InitGPUObjectBuffers()
    {
        auto allocBuffer = [](u64 size, VkBufferUsageFlags usage,
                               VkBuffer& buf, VmaAllocation& alloc, void*& mapped)
        {
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size  = size;
            info.usage = usage;
            alloc  = VulkanAllocator::AllocateBuffer(info, VMA_MEMORY_USAGE_CPU_TO_GPU, buf);
            mapped = VulkanAllocator::Map(alloc);
        };

        allocBuffer(
            RenderPipeline::k_MaxGPUObjects * sizeof(GPUObjectData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            m_ObjectSSBO, m_ObjectSSBOAlloc, m_ObjectSSBOMapped);

        // Indirect buffer holds 5 regions (camera + 4 cascades), each with RenderPipeline::k_IndirectRegionStride commands.
        allocBuffer(
            RenderPipeline::k_IndirectRegionCount * RenderPipeline::k_IndirectRegionStride * sizeof(VkDrawIndexedIndirectCommand),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            m_IndirectBuffer, m_IndirectBufferAlloc, m_IndirectBufferMapped);

        // Create Set 5 descriptor pool + set for the ObjectSSBO (graphics pipeline)
        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets       = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_ObjectSSBODescPool);

        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool     = m_ObjectSSBODescPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_ObjectSSBODescLayout;
        vkAllocateDescriptorSets(device, &allocInfo, &m_ObjectSSBODescSet);

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_ObjectSSBO;
        bufInfo.offset = 0;
        bufInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = m_ObjectSSBODescSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo     = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    void RenderPipeline::InitCullPipeline()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Descriptor layout: binding 0 = ObjectSSBO (read), binding 1 = IndirectBuffer (write)
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_CullDescLayout);

        VulkanContext::Get().GetDescriptorAllocator().Allocate(m_CullDescLayout, m_CullDescSet);

        VkDescriptorBufferInfo objInfo{};
        objInfo.buffer = m_ObjectSSBO;
        objInfo.offset = 0;
        objInfo.range  = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo indInfo{};
        indInfo.buffer = m_IndirectBuffer;
        indInfo.offset = 0;
        indInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_CullDescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo     = &objInfo;
        writes[1]                 = writes[0];
        writes[1].dstBinding      = 1;
        writes[1].pBufferInfo     = &indInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        // Push constant range: 6 frustum planes (96B) + objectCount (4B) + destOffset (4B) = 104B
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(Vec4) * 6 + sizeof(u32) * 2;

        auto cullShader = ShaderLibrary::LoadEngine("shaders/gpu_cull.comp");
        auto spv = cullShader ? cullShader->GetSpirV() : std::vector<u32>{};
        if (spv.empty())
        {
            LH_CORE_ERROR("RenderingSystem: Failed to load gpu_cull.comp!");
            return;
        }

        m_CullPipeline = std::make_unique<VKComputePipeline>(
            spv,
            std::vector<VkDescriptorSetLayout>{ m_CullDescLayout },
            std::vector<VkPushConstantRange>{ pcRange });
    }

    // =========================================================================
    //  GTAO (Ground Truth Ambient Occlusion) — epic #58
    // =========================================================================
    //
    // InitAOResources allocates the persistent half-res textures that back
    // the GTAO pass chain (prefilter → main → denoise) and the prefilter
    // compute pipeline. Main/denoise pipelines land in their own sub-tasks;
    // their textures are allocated here too so Resize sizes everything in
    // one place.
    //
    // Descriptor writes live in UpdateAODescriptors — called at end of init
    // and again after Resize recreates the textures (which invalidates any
    // stored image view pointers in the descriptor sets).
    void RenderPipeline::InitAOResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // ---- Half-res persistent textures ----
        const u32 halfW = std::max(m_System.m_Targets.GetSceneColor()->GetWidth()  / 2, 1u);
        const u32 halfH = std::max(m_System.m_Targets.GetSceneColor()->GetHeight() / 2, 1u);

        auto makeStorage = [&](TextureFormat fmt) {
            return std::make_shared<VKTexture>(
                halfW, halfH, fmt,
                /*arrayLayers*/ 1,
                /*createFlags*/ 0u,
                /*mipLevels*/   1,
                VK_IMAGE_USAGE_STORAGE_BIT);
        };

        m_GTAOLinearDepth = makeStorage(TextureFormat::R32_Float);
        m_GTAORawAO       = makeStorage(TextureFormat::R8);
        m_GTAOEdges       = makeStorage(TextureFormat::R8);
        m_GTAOFinal       = makeStorage(TextureFormat::R8);

        // ---- Shared linear-clamp sampler for GTAO compute reads ----
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_GTAOSampler);

        // ---- Shared descriptor pool for all GTAO sets ----
        // Enough capacity for prefilter + main + denoise (sub-task E).
        VkDescriptorPoolSize poolSizes[3] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          8 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         4 },
        };
        VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolCI.maxSets       = 8;
        poolCI.poolSizeCount = 3;
        poolCI.pPoolSizes    = poolSizes;
        vkCreateDescriptorPool(device, &poolCI, nullptr, &m_GTAODescPool);

        // ---- GTAO UBO (GTAOUBO std140, 48B) ----
        m_GTAOUBOBuffer = std::make_shared<VKUniformBuffer>(sizeof(GTAOUBO));

        // ---- Depth prefilter: [sampler2D sceneDepth, image2D linearDepth] ----
        {
            VkDescriptorSetLayoutBinding bindings[2]{};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.bindingCount = 2;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_GTAOPrefilterDescLayout);

            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool     = m_GTAODescPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_GTAOPrefilterDescLayout;
            vkAllocateDescriptorSets(device, &allocInfo, &m_GTAOPrefilterDescSet);

            VkPushConstantRange pcRange{};
            pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcRange.offset     = 0;
            pcRange.size       = sizeof(i32) * 2 + sizeof(float) * 6; // halfResSize + invFullRes + nearZ + farZ + pads

            if (auto sh = ShaderLibrary::LoadEngine("shaders/gtao_depth_prefilter.comp"))
                m_GTAOPrefilterSpv = sh->GetSpirV();
            if (m_GTAOPrefilterSpv.empty())
            {
                LH_CORE_ERROR("RenderingSystem: Failed to load gtao_depth_prefilter.comp!");
                return;
            }
            m_GTAOPrefilterPipeline = std::make_unique<VKComputePipeline>(
                m_GTAOPrefilterSpv,
                std::vector<VkDescriptorSetLayout>{ m_GTAOPrefilterDescLayout },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // ---- Main pass: [sampler2D linearDepth, image2D rawAO, UBO] ----
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[2].binding         = 2;
            bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.bindingCount = 3;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_GTAOMainDescLayout);

            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool     = m_GTAODescPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_GTAOMainDescLayout;
            vkAllocateDescriptorSets(device, &allocInfo, &m_GTAOMainDescSet);

            VkPushConstantRange pcRange{};
            pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcRange.offset     = 0;
            pcRange.size       = sizeof(float) * 4 + sizeof(u32) * 4; // projParams + near/far + frameIndex + pads

            if (auto sh = ShaderLibrary::LoadEngine("shaders/gtao_main.comp"))
                m_GTAOMainSpv = sh->GetSpirV();
            if (m_GTAOMainSpv.empty())
            {
                LH_CORE_ERROR("RenderingSystem: Failed to load gtao_main.comp!");
                return;
            }
            m_GTAOMainPipeline = std::make_unique<VKComputePipeline>(
                m_GTAOMainSpv,
                std::vector<VkDescriptorSetLayout>{ m_GTAOMainDescLayout },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // ---- Denoise pass: [sampler2D rawAO, sampler2D linDepth, image2D finalAO] ----
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[2].binding         = 2;
            bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.bindingCount = 3;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_GTAODenoiseDescLayout);

            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool     = m_GTAODescPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_GTAODenoiseDescLayout;
            vkAllocateDescriptorSets(device, &allocInfo, &m_GTAODenoiseDescSet);

            // No push constants — resolution derived from textureSize() inside the shader.
            if (auto sh = ShaderLibrary::LoadEngine("shaders/gtao_denoise.comp"))
                m_GTAODenoiseSpv = sh->GetSpirV();
            if (m_GTAODenoiseSpv.empty())
            {
                LH_CORE_ERROR("RenderingSystem: Failed to load gtao_denoise.comp!");
                return;
            }
            m_GTAODenoisePipeline = std::make_unique<VKComputePipeline>(
                m_GTAODenoiseSpv,
                std::vector<VkDescriptorSetLayout>{ m_GTAODenoiseDescLayout },
                std::vector<VkPushConstantRange>{});
        }

        UpdateAODescriptors();
    }

    void RenderPipeline::UpdateAODescriptors()
    {
        if (m_GTAOPrefilterDescSet == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        auto vkSceneDepth = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneDepth());
        auto vkLinDepth   = std::static_pointer_cast<VKTexture>(m_GTAOLinearDepth);
        auto vkRawAO      = std::static_pointer_cast<VKTexture>(m_GTAORawAO);
        auto vkFinalAO    = std::static_pointer_cast<VKTexture>(m_GTAOFinal);

        // Shared VkDescriptorImageInfo / buffer-info slots reused across passes.
        VkDescriptorImageInfo  sceneDepthInfo{};
        sceneDepthInfo.sampler     = m_GTAOSampler;
        sceneDepthInfo.imageView   = vkSceneDepth->GetImageView();
        sceneDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo  linDepthSampledInfo{};
        linDepthSampledInfo.sampler     = m_GTAOSampler;
        linDepthSampledInfo.imageView   = vkLinDepth->GetImageView();
        linDepthSampledInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo  linDepthStorageInfo{};
        linDepthStorageInfo.sampler     = VK_NULL_HANDLE;
        linDepthStorageInfo.imageView   = vkLinDepth->GetImageView();
        linDepthStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo  rawAOStorageInfo{};
        rawAOStorageInfo.sampler     = VK_NULL_HANDLE;
        rawAOStorageInfo.imageView   = vkRawAO->GetImageView();
        rawAOStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = m_GTAOUBOBuffer ? m_GTAOUBOBuffer->GetVulkanBuffer() : VK_NULL_HANDLE;
        uboInfo.offset = 0;
        uboInfo.range  = VK_WHOLE_SIZE;

        // ---- Prefilter pass: [sceneDepth (sampler), linDepth (storage)] ----
        VkWriteDescriptorSet preWrites[2]{};
        preWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        preWrites[0].dstSet          = m_GTAOPrefilterDescSet;
        preWrites[0].dstBinding      = 0;
        preWrites[0].descriptorCount = 1;
        preWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        preWrites[0].pImageInfo      = &sceneDepthInfo;

        preWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        preWrites[1].dstSet          = m_GTAOPrefilterDescSet;
        preWrites[1].dstBinding      = 1;
        preWrites[1].descriptorCount = 1;
        preWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        preWrites[1].pImageInfo      = &linDepthStorageInfo;

        vkUpdateDescriptorSets(device, 2, preWrites, 0, nullptr);

        // ---- Main pass: [linDepth (sampler), rawAO (storage), UBO] ----
        if (m_GTAOMainDescSet == VK_NULL_HANDLE || uboInfo.buffer == VK_NULL_HANDLE) return;

        VkWriteDescriptorSet mainWrites[3]{};
        mainWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mainWrites[0].dstSet          = m_GTAOMainDescSet;
        mainWrites[0].dstBinding      = 0;
        mainWrites[0].descriptorCount = 1;
        mainWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mainWrites[0].pImageInfo      = &linDepthSampledInfo;

        mainWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mainWrites[1].dstSet          = m_GTAOMainDescSet;
        mainWrites[1].dstBinding      = 1;
        mainWrites[1].descriptorCount = 1;
        mainWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        mainWrites[1].pImageInfo      = &rawAOStorageInfo;

        mainWrites[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mainWrites[2].dstSet          = m_GTAOMainDescSet;
        mainWrites[2].dstBinding      = 2;
        mainWrites[2].descriptorCount = 1;
        mainWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mainWrites[2].pBufferInfo     = &uboInfo;

        vkUpdateDescriptorSets(device, 3, mainWrites, 0, nullptr);

        // ---- Denoise pass: [rawAO (sampler), linDepth (sampler), finalAO (storage)] ----
        if (m_GTAODenoiseDescSet == VK_NULL_HANDLE) return;

        VkDescriptorImageInfo rawAOSampledInfo{};
        rawAOSampledInfo.sampler     = m_GTAOSampler;
        rawAOSampledInfo.imageView   = vkRawAO->GetImageView();
        rawAOSampledInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo finalAOStorageInfo{};
        finalAOStorageInfo.sampler     = VK_NULL_HANDLE;
        finalAOStorageInfo.imageView   = vkFinalAO->GetImageView();
        finalAOStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet denoiseWrites[3]{};
        denoiseWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        denoiseWrites[0].dstSet          = m_GTAODenoiseDescSet;
        denoiseWrites[0].dstBinding      = 0;
        denoiseWrites[0].descriptorCount = 1;
        denoiseWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        denoiseWrites[0].pImageInfo      = &rawAOSampledInfo;

        denoiseWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        denoiseWrites[1].dstSet          = m_GTAODenoiseDescSet;
        denoiseWrites[1].dstBinding      = 1;
        denoiseWrites[1].descriptorCount = 1;
        denoiseWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        denoiseWrites[1].pImageInfo      = &linDepthSampledInfo;

        denoiseWrites[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        denoiseWrites[2].dstSet          = m_GTAODenoiseDescSet;
        denoiseWrites[2].dstBinding      = 2;
        denoiseWrites[2].descriptorCount = 1;
        denoiseWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        denoiseWrites[2].pImageInfo      = &finalAOStorageInfo;

        vkUpdateDescriptorSets(device, 3, denoiseWrites, 0, nullptr);

        // ---- Set 0 GTAO bindings (sampled by pbr.frag) ----
        if (m_GlobalDescriptorSet == VK_NULL_HANDLE) return;

        VkDescriptorImageInfo gtaoFinalInfo{};
        gtaoFinalInfo.sampler     = m_GTAOSampler;
        gtaoFinalInfo.imageView   = vkFinalAO->GetImageView();
        gtaoFinalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet globalWrites[2]{};
        globalWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        globalWrites[0].dstSet          = m_GlobalDescriptorSet;
        globalWrites[0].dstBinding      = 4;
        globalWrites[0].descriptorCount = 1;
        globalWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        globalWrites[0].pImageInfo      = &gtaoFinalInfo;

        globalWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        globalWrites[1].dstSet          = m_GlobalDescriptorSet;
        globalWrites[1].dstBinding      = 5;
        globalWrites[1].descriptorCount = 1;
        globalWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        globalWrites[1].pBufferInfo     = &uboInfo;

        vkUpdateDescriptorSets(device, 2, globalWrites, 0, nullptr);
    }

    void RenderPipeline::RegisterNamedTextures()
    {
        m_NamedTextures.clear();
        if (m_ShadowMap)      m_NamedTextures["ShadowMap"]      = m_ShadowMap;
        if (m_System.m_Targets.GetSceneColor())    m_NamedTextures["SceneColor"]    = m_System.m_Targets.GetSceneColor();
        if (m_System.m_Targets.GetSceneDepth())    m_NamedTextures["SceneDepth"]    = m_System.m_Targets.GetSceneDepth();
        if (m_System.m_Targets.GetLDROutput())     m_NamedTextures["LDROutput"]     = m_System.m_Targets.GetLDROutput();
        if (m_System.m_Targets.GetEntityIDBuffer())m_NamedTextures["EntityID"]     = m_System.m_Targets.GetEntityIDBuffer();
        if (m_BloomA)        m_NamedTextures["BloomA"]        = m_BloomA;
        if (m_BloomB)        m_NamedTextures["BloomB"]        = m_BloomB;
        if (m_IrradianceMap) m_NamedTextures["IrradianceMap"] = m_IrradianceMap;
        if (m_PrefilteredMap)m_NamedTextures["PrefilteredMap"]= m_PrefilteredMap;
        if (m_BRDFLut)       m_NamedTextures["BRDF_LUT"]     = m_BRDFLut;
    }

    void RenderPipeline::UpdatePostProcessUBO()
    {
        PostProcessUBO ubo{};
        ubo.bloomThreshold      = m_System.m_PostProcessSettings.bloomThreshold;
        ubo.bloomStrength       = m_System.m_PostProcessSettings.bloomStrength;
        ubo.exposure            = m_System.m_PostProcessSettings.exposure;
        ubo.contrast            = m_System.m_PostProcessSettings.contrast;
        ubo.saturation          = m_System.m_PostProcessSettings.saturation;
        ubo.tonemapOp           = static_cast<int>(m_System.m_PostProcessSettings.tonemapOp);
        ubo.vignetteAmount      = m_System.m_PostProcessSettings.vignetteAmount;
        ubo.vignetteHardness    = m_System.m_PostProcessSettings.vignetteHardness;
        ubo.grainAmount         = m_System.m_PostProcessSettings.grainAmount;
        ubo.sharpness           = m_System.m_PostProcessSettings.sharpness;
        ubo.chromaticAberration = m_System.m_PostProcessSettings.chromaticAberration;
        ubo.time                = Time::GetTime();
        ubo.shadowBalance       = m_System.m_PostProcessSettings.shadowBalance;
        ubo.midtoneBalance      = m_System.m_PostProcessSettings.midtoneBalance;
        ubo.highlightBalance    = m_System.m_PostProcessSettings.highlightBalance;
        m_PostProcessUBOBuffer->SetData(&ubo, sizeof(PostProcessUBO));
    }

    // =========================================================================
    // IBL precomputation
    // =========================================================================

    u32 RenderPipeline::EnsureMaterialRegistered(std::shared_ptr<Material> material)
    {
        auto it = m_MaterialSlotMap.find(material->Handle);
        if (it != m_MaterialSlotMap.end())
            return it->second;

        u32 slot = MaterialSystem::RegisterMaterial(material);
        m_MaterialSlotMap[material->Handle] = slot;
        return slot;
    }

    // =========================================================================
    // Per-frame updates
    // =========================================================================

    void RenderPipeline::UpdateGTAOUBO()
    {
        if (!m_GTAOUBOBuffer) return;

        const auto& s = m_System.m_PostProcessSettings.gtao;
        GTAOUBO ubo{};
        ubo.intensity      = s.intensity;
        ubo.radius         = s.radius;
        ubo.falloff        = s.falloff;
        ubo.power          = s.power;
        ubo.sliceCount     = s.sliceCount;
        ubo.stepsPerSlice  = s.stepsPerSlice;
        ubo.enabled        = s.enabled  ? 1 : 0;
        ubo.visualize      = s.visualize ? 1 : 0;

        const u32 halfW = m_GTAOLinearDepth ? m_GTAOLinearDepth->GetWidth()  : 1u;
        const u32 halfH = m_GTAOLinearDepth ? m_GTAOLinearDepth->GetHeight() : 1u;
        const u32 fullW = m_System.m_Targets.GetSceneColor() ? m_System.m_Targets.GetSceneColor()->GetWidth()  : 1u;
        const u32 fullH = m_System.m_Targets.GetSceneColor() ? m_System.m_Targets.GetSceneColor()->GetHeight() : 1u;
        ubo.invResolution[0]     = 1.0f / float(halfW);
        ubo.invResolution[1]     = 1.0f / float(halfH);
        ubo.invFullResolution[0] = 1.0f / float(fullW);
        ubo.invFullResolution[1] = 1.0f / float(fullH);

        m_GTAOUBOBuffer->SetData(&ubo, sizeof(GTAOUBO));
    }

    void RenderPipeline::BuildGPUObjectBuffer(entt::registry& registry)
    {
        auto* objectData   = static_cast<GPUObjectData*>(m_ObjectSSBOMapped);
        auto* indirectCmds = static_cast<VkDrawIndexedIndirectCommand*>(m_IndirectBufferMapped);
        u32   count        = 0;

        // Rebuild entity lookup table here (consumed by GeometryPass + mouse picking)
        // index 0 = null sentinel; valid entities start at index 1
        m_EntityLookup.clear();
        m_EntityLookup.push_back(entt::null);
        m_EntityToSSBOIndex.clear();

        auto view = registry.view<WorldTransform, MeshRenderer>();
        for (auto [entity, wt, mr] : view.each())
        {
            if (count >= RenderPipeline::k_MaxGPUObjects) break;
            if (!mr.ModelUUID.IsValid()) continue;

            auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
            if (!model) continue;
            const auto& meshesData = model->GetMeshesData();
            if (mr.MeshIndex >= (u32)meshesData.size()) continue;
            auto mesh = model->GetMesh(mr.MeshIndex);
            if (!mesh) continue;

            GPUObjectData& obj = objectData[count];
            obj.model = wt.Matrix;

            // Bounding sphere from BindPoseAABB (local space)
            const auto& aabb   = meshesData[mr.MeshIndex].BindPoseAABB;
            obj.boundingSphere = Vec4(aabb.Center(), glm::length(aabb.Extents()));

            // Material slot
            u32 matSlot = 0;
            if (mr.MaterialUUID.IsValid()) {
                auto it = m_MaterialSlotMap.find(mr.MaterialUUID);
                if (it != m_MaterialSlotMap.end()) matSlot = it->second;
            }
            obj.materialIndex = matSlot;
            obj.shadeMode     = static_cast<u32>(m_System.m_ShadeMode);
            // entityID is 1-indexed so the fragment shader output matches m_EntityLookup
            obj.entityID      = (u32)m_EntityLookup.size();  // assigned before push_back
            obj.boneOffset    = 0;

            m_EntityLookup.push_back(entity);      // m_EntityLookup[count + 1] = entity
            m_EntityToSSBOIndex[entity] = count;   // entity → 0-based SSBO index

            // Skinned mesh: get bone offset from Animation component on this entity
            if (meshesData[mr.MeshIndex].IsSkinned && registry.all_of<Animation>(entity))
            {
                auto& anim = registry.get<Animation>(entity);
                if (anim.BufferAllocated)
                    obj.boneOffset = anim.BoneBufferOffset;
            }

            auto* ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer()).get();
            obj.indexCount   = ib ? ib->GetCount() : 0;
            obj.firstIndex   = 0;
            obj.vertexOffset = 0;
            obj._pad         = 0;

            // Indirect command — instanceCount=1; per-region GPU cull zeros it if culled.
            // firstInstance = SSBO index (gl_BaseInstance in shader → objects[gl_BaseInstance]).
            // Duplicate into all RenderPipeline::k_IndirectRegionCount regions (camera + 4 cascades) so each
            // region has its own independently-cullable command for this object.
            VkDrawIndexedIndirectCommand baseCmd{};
            baseCmd.indexCount    = obj.indexCount;
            baseCmd.instanceCount = 1;
            baseCmd.firstIndex    = 0;
            baseCmd.vertexOffset  = 0;
            baseCmd.firstInstance = count;
            for (u32 r = 0; r < RenderPipeline::k_IndirectRegionCount; ++r)
                indirectCmds[r * RenderPipeline::k_IndirectRegionStride + count] = baseCmd;
            count++;
        }

        m_GPUObjectCount = count;
    }

    void RenderPipeline::UploadLightUBO(const LightUniforms& lights)
    {
        m_LightUniformBuffer->SetData(&lights, sizeof(LightUniforms));
    }

    void RenderPipeline::UpdateGlobalUniforms()
    {
        GlobalUniforms ubo{};
        ubo.view = m_System.m_CameraParams.view;
        ubo.projection = m_System.m_CameraParams.projection;
        ubo.projection[1][1] *= -1.0f;  // Vulkan Y-flip (shader only, not ImGuizmo)
        ubo.viewProjection = ubo.projection * ubo.view;
        ubo.cameraPos = m_System.m_CameraParams.position;
        ubo.time = Time::GetTime();
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            ubo.lightSpaceMatrix[i] = m_System.m_Cascades.lightSpaceMatrix[i];
        ubo.cascadeSplitsViewZ = m_System.m_Cascades.splitsViewZ;
        // Negative bias (sentinel) disables shadows entirely in the PBR shader.
        ubo.shadowBias       = m_System.m_ShadowParams.castShadows ? m_System.m_ShadowParams.shadowBias : Vec4(-1.0f);
        ubo.shadowNormalBias = m_System.m_ShadowParams.shadowNormalBias;
        ubo.cascadeTexelSize = m_System.m_Cascades.texelSize;
        ubo.iblIntensity    = m_System.m_CameraParams.iblIntensity;
        ubo.skyboxIntensity = m_System.m_CameraParams.skyboxIntensity;
        ubo.debugVisualizeCascades = m_System.m_ShadowParams.debugVisualizeCascades ? 1.0f : 0.0f;
        ubo.cascadeBlendWidth      = m_System.m_ShadowParams.cascadeBlendWidth;

        m_GlobalUniformBuffer->SetData(&ubo, sizeof(GlobalUniforms));
        m_CachedViewProj = ubo.viewProjection;
    }

    // =========================================================================
    // Main Update
    // =========================================================================

    std::shared_ptr<Texture> RenderPipeline::GetNamedTexture(const std::string& name) const
    {
        auto it = m_NamedTextures.find(name);
        return (it != m_NamedTextures.end()) ? it->second : nullptr;
    }

    // =========================================================================
    // Mouse Picking
    // =========================================================================

    void RenderPipeline::InitDebugBlitResources()
    {
        if (m_System.m_FrameDebugger.blitPipeline) return; // Already initialized

        if (auto sh = ShaderLibrary::LoadEngine("shaders/debugBlit.frag"))
            m_System.m_FrameDebugger.blitFragSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/debugDepth.frag"))
            m_System.m_FrameDebugger.depthFragSpv = sh->GetSpirV();

        if (m_System.m_FrameDebugger.blitFragSpv.empty() || m_System.m_FrameDebugger.depthFragSpv.empty() || m_FullscreenVertSpv.empty())
        {
            LH_CORE_ERROR("Failed to compile debug blit shaders");
            return;
        }

        auto device = VulkanContext::Get().GetDevice();

        // Create sampler
        VkSamplerCreateInfo samplerCI{};
        samplerCI.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerCI.magFilter = VK_FILTER_LINEAR;
        samplerCI.minFilter = VK_FILTER_LINEAR;
        samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(device, &samplerCI, nullptr, &m_System.m_FrameDebugger.sampler);

        // Descriptor set layout: binding 0 = combined image sampler
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCI{};
        layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCI.bindingCount = 1;
        layoutCI.pBindings    = &binding;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_System.m_FrameDebugger.descSetLayout);

        // Descriptor pool
        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo poolCI{};
        poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCI.maxSets       = 1;
        poolCI.poolSizeCount = 1;
        poolCI.pPoolSizes    = &poolSize;
        poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &poolCI, nullptr, &m_System.m_FrameDebugger.descPool);

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocCI{};
        allocCI.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocCI.descriptorPool     = m_System.m_FrameDebugger.descPool;
        allocCI.descriptorSetCount = 1;
        allocCI.pSetLayouts        = &m_System.m_FrameDebugger.descSetLayout;
        vkAllocateDescriptorSets(device, &allocCI, &m_System.m_FrameDebugger.descSet);

        // Create blit pipeline (color)
        std::vector<VkDescriptorSetLayout> layouts = { m_System.m_FrameDebugger.descSetLayout };
        PipelineConfig blitConfig;
        blitConfig.depthTest  = false;
        blitConfig.depthWrite = false;
        blitConfig.cullMode         = VK_CULL_MODE_NONE;
        blitConfig.colorFormats     = { VK_FORMAT_R8G8B8A8_UNORM };
        m_System.m_FrameDebugger.blitPipeline = std::make_unique<VKPipeline>(
            blitConfig, m_FullscreenVertSpv, m_System.m_FrameDebugger.blitFragSpv, layouts);

        // Create depth visualization pipeline
        PipelineConfig depthConfig;
        depthConfig.depthTest  = false;
        depthConfig.depthWrite = false;
        depthConfig.cullMode         = VK_CULL_MODE_NONE;
        depthConfig.colorFormats     = { VK_FORMAT_R8G8B8A8_UNORM };

        VkPushConstantRange depthPC{};
        depthPC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        depthPC.offset     = 0;
        depthPC.size       = sizeof(float) * 2; // near, far
        depthConfig.pushConstantRanges = { depthPC };

        m_System.m_FrameDebugger.depthPipeline = std::make_unique<VKPipeline>(
            depthConfig, m_FullscreenVertSpv, m_System.m_FrameDebugger.depthFragSpv, layouts);
    }

    RG::ResourceHandle RenderPipeline::AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth)
    {
        if (!m_System.m_FrameDebugger.blitPipeline || !m_System.m_Targets.GetLDROutput()) return inputHandle;

        struct DebugBlitData {
            RG::ResourceHandle output;
            RG::ResourceHandle input;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<DebugBlitData>("DebugDisplayBlit",
            [&](DebugBlitData& data, RG::RenderPassBuilder& builder)
            {
                auto ldrVk = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetLDROutput());
                RG::TextureDesc desc;
                desc.name   = "LDROutput";
                desc.width  = m_System.m_Targets.GetLDROutput()->GetWidth();
                desc.height = m_System.m_Targets.GetLDROutput()->GetHeight();
                desc.format = RG::TextureFormat::RGBA8_Unorm;

                data.output = rg.ImportResource(desc,
                    (void*)ldrVk->GetImage(), (void*)ldrVk->GetImageView(),
                    RG::ResourceState::ShaderResource);
                data.output = builder.Write(data.output);

                data.input = builder.Read(inputHandle);
                outputHandle = data.output;
            },
            [this, isDepth](DebugBlitData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                u32 w = m_System.m_Targets.GetLDROutput()->GetWidth();
                u32 h = m_System.m_Targets.GetLDROutput()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                if (isDepth && m_System.m_FrameDebugger.depthPipeline)
                {
                    m_System.m_FrameDebugger.depthPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_System.m_FrameDebugger.depthPipeline->GetLayout(), 0, 1, &m_System.m_FrameDebugger.descSet, 0, nullptr);

                    float pc[2] = { 0.1f, 200.0f }; // near/far for shadow maps
                    vkCmdPushConstants(cmd, m_System.m_FrameDebugger.depthPipeline->GetLayout(),
                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
                }
                else
                {
                    m_System.m_FrameDebugger.blitPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_System.m_FrameDebugger.blitPipeline->GetLayout(), 0, 1, &m_System.m_FrameDebugger.descSet, 0, nullptr);
                }

                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
        );

        return outputHandle;
    }

    // Phase 14C — RenderPipeline::RenderCapturedFrame removed.
    // Live re-replay was the source of the original sync bug. The Frozen-state
    // branch (top of Update) now serves archived images; per-draw inspection
    // is implemented below via ImmediateSubmit replay-then-copy.

    // =========================================================================
    //  Frame Debugger Phase 14E — Per-draw replay-then-copy
    // =========================================================================

    void RenderPipeline::EnsurePerDrawPreviewTexture(u32 width, u32 height)
    {
        if (m_PerDrawPreviewImage != VK_NULL_HANDLE
            && m_PerDrawPreviewWidth == width
            && m_PerDrawPreviewHeight == height) return;

        // Tear down any prior preview at a different size first. Deferred so
        // an in-flight ImGui frame can finish sampling the old view safely.
        if (m_PerDrawPreviewImage != VK_NULL_HANDLE)
        {
            VkImage       img   = m_PerDrawPreviewImage;
            VkImageView   view  = m_PerDrawPreviewView;
            VmaAllocation alloc = m_PerDrawPreviewAlloc;
            VulkanContext::Get().PushDeletion([img, view, alloc]() {
                auto dev = VulkanContext::Get().GetDevice();
                vkDestroyImageView(dev, view, nullptr);
                VulkanAllocator::FreeImage(img, alloc);
            });
            m_PerDrawPreviewImage = VK_NULL_HANDLE;
            m_PerDrawPreviewView  = VK_NULL_HANDLE;
            m_PerDrawPreviewAlloc = nullptr;
        }

        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.extent        = { width, height, 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        // Match the scene color target's format so vkCmdCopyImage is layout-compatible.
        ci.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        m_PerDrawPreviewAlloc = VulkanAllocator::AllocateImage(ci, VMA_MEMORY_USAGE_AUTO, m_PerDrawPreviewImage);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_PerDrawPreviewImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = ci.format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(VulkanContext::Get().GetDevice(), &vci, nullptr, &m_PerDrawPreviewView);

        m_PerDrawPreviewWidth  = width;
        m_PerDrawPreviewHeight = height;
        m_PerDrawPreviewKey    = UINT64_MAX;  // dimensions changed → invalidate cache
    }

    void RenderPipeline::DestroyPerDrawPreviewTexture()
    {
        if (m_PerDrawPreviewImage == VK_NULL_HANDLE) return;
        auto dev = VulkanContext::Get().GetDevice();
        vkDestroyImageView(dev, m_PerDrawPreviewView, nullptr);
        VulkanAllocator::FreeImage(m_PerDrawPreviewImage, m_PerDrawPreviewAlloc);
        m_PerDrawPreviewImage  = VK_NULL_HANDLE;
        m_PerDrawPreviewView   = VK_NULL_HANDLE;
        m_PerDrawPreviewAlloc  = nullptr;
        m_PerDrawPreviewWidth  = 0;
        m_PerDrawPreviewHeight = 0;
        m_PerDrawPreviewKey    = UINT64_MAX;
    }

    void RenderPipeline::ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx)
    {
        if (m_System.m_FrameDebugger.state != DebuggerState::Frozen) return;
        if (!m_System.m_FrameDebugger.capturedFrame.valid) return;
        if (passIdx >= m_System.m_FrameDebugger.capturedFrame.passes.size()) return;

        const auto& pass = m_System.m_FrameDebugger.capturedFrame.passes[passIdx];

        // v1 — only GeometryPass per-draw replay is wired. Other passes leave
        // the cache key unchanged so the panel falls back to pass-output archive.
        if (pass.name != "GeometryPass") return;
        if (!m_System.m_Targets.GetSceneColor() || !m_System.m_Targets.GetSceneDepth() || !m_System.m_Targets.GetEntityIDBuffer()) return;

        // Cache hit — same selection as last replay, nothing to do.
        const u64 key = ((u64)passIdx << 32) | (u64)localDrawIdx;
        if (key == m_PerDrawPreviewKey) return;

        const u32 width  = m_System.m_Targets.GetSceneColor()->GetWidth();
        const u32 height = m_System.m_Targets.GetSceneColor()->GetHeight();
        EnsurePerDrawPreviewTexture(width, height);
        if (m_PerDrawPreviewImage == VK_NULL_HANDLE) return;

        auto vkSceneColor = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneColor());
        auto vkSceneDepth = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneDepth());
        auto vkEntityID   = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetEntityIDBuffer());
        VkImage     sceneColorImg  = vkSceneColor->GetImage();
        VkImageView sceneColorView = vkSceneColor->GetImageView();
        VkImage     sceneDepthImg  = vkSceneDepth->GetImage();
        VkImageView sceneDepthView = vkSceneDepth->GetImageView();
        VkImage     entityIDImg    = vkEntityID->GetImage();
        VkImageView entityIDView   = vkEntityID->GetImageView();

        // Total draws to issue (1-based count). Original GeometryPass emits
        // draws in opaque → cutout → transparent order, which matches the
        // capture-time CapturedDrawCall ordering.
        const u32 maxDraws = localDrawIdx + 1;

        // Capture the CPU-side data we need by value (the lambda runs inside
        // ImmediateSubmit and must be self-contained).
        VkPolygonMode polyMode = (m_System.m_ShadeMode == ShadeMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        UUID pbrUUID = ShaderLibrary::Get("pbr.vert")->Handle;

        VulkanContext::Get().ImmediateSubmit([&, this](VkCommandBuffer cmd)
        {
            // ---- Phase 1: prep all attachments + preview ----
            // We use UNDEFINED → ATTACHMENT to ignore the prior contents (the
            // graph CLEARs anyway) and avoid relying on whatever layout the
            // last live frame left them in.
            VkImageMemoryBarrier2 prep[4]{};
            auto fillAtt = [](VkImageMemoryBarrier2& b, VkImage img,
                              VkImageAspectFlags aspect, VkImageLayout newLayout,
                              VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
            {
                b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                b.srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                b.srcAccessMask       = 0;
                b.dstStageMask        = dstStage;
                b.dstAccessMask       = dstAccess;
                b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout           = newLayout;
                b.image               = img;
                b.subresourceRange    = { aspect, 0, 1, 0, 1 };
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            };
            fillAtt(prep[0], sceneColorImg, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            fillAtt(prep[1], sceneDepthImg, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            fillAtt(prep[2], entityIDImg, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            VkDependencyInfo depPrep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depPrep.imageMemoryBarrierCount = 3;
            depPrep.pImageMemoryBarriers    = prep;
            vkCmdPipelineBarrier2(cmd, &depPrep);

            // ---- Phase 2: BeginRendering with cleared attachments ----
            AttachmentInfo colorAtt[2]{};
            colorAtt[0].ImageView  = sceneColorView;
            colorAtt[0].Format     = VK_FORMAT_R16G16B16A16_SFLOAT;
            colorAtt[0].LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt[0].StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt[0].ClearValue = { { {0.f, 0.f, 0.f, 0.f} } };
            colorAtt[0].Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            colorAtt[1].ImageView  = entityIDView;
            colorAtt[1].Format     = VK_FORMAT_R32_UINT;
            colorAtt[1].LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt[1].StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt[1].ClearValue.color.uint32[0] = 0;
            colorAtt[1].Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            AttachmentInfo depthAtt{};
            depthAtt.ImageView  = sceneDepthView;
            depthAtt.Format     = VK_FORMAT_D32_SFLOAT;
            depthAtt.LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.ClearValue.depthStencil = { 1.0f, 0 };
            depthAtt.Layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            RenderPassInfo rpInfo{};
            rpInfo.ColorAttachments = std::span<AttachmentInfo>(colorAtt, 2);
            rpInfo.DepthAttachment  = &depthAtt;
            rpInfo.RenderArea       = { {0, 0}, { width, height } };

            DynamicRendering::BeginRendering(cmd, rpInfo);

            // ---- Phase 3: bind pipelines + descriptors, replay draws ----
            // Same descriptor sets as the live GeometryPass — the underlying
            // UBOs/SSBOs are byte-stable in Frozen state (no live writers).
            auto* opaquePipeline = m_GeoPipelineManager.GetOrCreate(
                pbrUUID, Material::RenderMode::Opaque, Material::CullMode::Back,
                polyMode, m_PBRVertSpv, m_PBRFragSpv);
            if (!opaquePipeline)
            {
                DynamicRendering::EndRendering(cmd);
                return;
            }
            VkPipelineLayout pipelineLayout = opaquePipeline->GetLayout();

            VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
            VkDescriptorSet sets[] = {
                m_GlobalDescriptorSet, bindlessSet, MaterialSystem::GetDescriptorSet(),
                m_LightDescSet, BoneMatrixBuffer::GetDescriptorSet(), m_ObjectSSBODescSet
            };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipelineLayout, 0, 6, sets, 0, nullptr);

            VkViewport vp{}; vp.width = (float)width; vp.height = (float)height; vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D scRect{}; scRect.extent = { width, height };
            vkCmdSetScissor(cmd, 0, 1, &scRect);

            u32 drawsRemaining = maxDraws;

            auto ReplayBatch = [&](const std::vector<DrawCommand>& draws, Material::RenderMode mode)
            {
                if (drawsRemaining == 0 || draws.empty()) return;

                Material::CullMode currentCull = Material::CullMode::Back;
                bool currentSkinned = false;
                auto* pipeline = m_GeoPipelineManager.GetOrCreate(
                    pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                if (!pipeline) return;
                pipeline->Bind(cmd);

                for (const auto& dc : draws)
                {
                    if (drawsRemaining == 0) return;

                    if (dc.cullMode != currentCull || dc.isSkinned != currentSkinned)
                    {
                        currentCull    = dc.cullMode;
                        currentSkinned = dc.isSkinned;
                        VKPipeline* newPipeline = currentSkinned
                            ? m_GeoSkinnedPipelineManager.GetOrCreate(pbrUUID, mode, currentCull, polyMode, m_PBRSkinnedVertSpv, m_PBRFragSpv)
                            : m_GeoPipelineManager.GetOrCreate       (pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv,        m_PBRFragSpv);
                        if (!newPipeline) continue;
                        newPipeline->Bind(cmd);
                    }

                    auto mesh = dc.model->GetMesh(dc.meshIndex);
                    if (!mesh) { drawsRemaining--; continue; }
                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer >(mesh->GetIndexBuffer ());
                    if (!vb || !ib) { drawsRemaining--; continue; }

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                    // Use the captured indirect buffer (camera region, byte-stable
                    // in Frozen state). gpuObjectIndex is also the firstInstance
                    // base — same as live GeometryPass.
                    VkDeviceSize indirectOffset = dc.gpuObjectIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, m_IndirectBuffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));

                    --drawsRemaining;
                }
            };

            ReplayBatch(m_System.m_DrawList.opaque,      Material::RenderMode::Opaque);
            ReplayBatch(m_System.m_DrawList.cutout,      Material::RenderMode::Cutout);
            ReplayBatch(m_System.m_DrawList.transparent, Material::RenderMode::Transparent);

            DynamicRendering::EndRendering(cmd);

            // ---- Phase 4: copy SceneColor → preview ----
            VkImageMemoryBarrier2 toCopy[2]{};
            toCopy[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toCopy[0].srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toCopy[0].srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toCopy[0].dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toCopy[0].dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
            toCopy[0].oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toCopy[0].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toCopy[0].image               = sceneColorImg;
            toCopy[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toCopy[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            toCopy[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toCopy[1].srcStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toCopy[1].srcAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            toCopy[1].dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toCopy[1].dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            // First-ever replay is UNDEFINED, subsequent replays come from
            // SHADER_READ_ONLY_OPTIMAL after the panel sampled the previous
            // preview. Treat both uniformly via UNDEFINED — discards content
            // (which we'd overwrite anyway).
            toCopy[1].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            toCopy[1].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toCopy[1].image               = m_PerDrawPreviewImage;
            toCopy[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toCopy[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depCopy{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depCopy.imageMemoryBarrierCount = 2;
            depCopy.pImageMemoryBarriers    = toCopy;
            vkCmdPipelineBarrier2(cmd, &depCopy);

            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.extent         = { width, height, 1 };
            vkCmdCopyImage(cmd,
                sceneColorImg,         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_PerDrawPreviewImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &copy);

            // ---- Phase 5: restore layouts for next consumer ----
            // SceneColor → SHADER_READ_ONLY (matches what the live pipeline
            // expects after PostProcessPass; next live frame's barriers will
            // re-transition as needed).
            // Preview → SHADER_READ_ONLY for ImGui sampling.
            VkImageMemoryBarrier2 fin[2]{};
            fin[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            fin[0].srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            fin[0].srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
            fin[0].dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            fin[0].dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            fin[0].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            fin[0].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fin[0].image               = sceneColorImg;
            fin[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fin[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fin[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            fin[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            fin[1].srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            fin[1].srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            fin[1].dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            fin[1].dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            fin[1].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            fin[1].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fin[1].image               = m_PerDrawPreviewImage;
            fin[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fin[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fin[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depFin{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depFin.imageMemoryBarrierCount = 2;
            depFin.pImageMemoryBarriers    = fin;
            vkCmdPipelineBarrier2(cmd, &depFin);
        });

        m_PerDrawPreviewKey = key;
    }

    // =========================================================================
    //  Frame Debugger Phase 14F — Depth archive visualization (CSM cascades)
    // =========================================================================

    void RenderPipeline::EnsureDepthPreviewTexture(u32 width, u32 height)
    {
        if (m_DepthPreviewImage != VK_NULL_HANDLE
            && m_DepthPreviewWidth == width
            && m_DepthPreviewHeight == height) return;

        if (m_DepthPreviewImage != VK_NULL_HANDLE)
        {
            VkImage       img   = m_DepthPreviewImage;
            VkImageView   view  = m_DepthPreviewView;
            VmaAllocation alloc = m_DepthPreviewAlloc;
            VulkanContext::Get().PushDeletion([img, view, alloc]() {
                auto dev = VulkanContext::Get().GetDevice();
                vkDestroyImageView(dev, view, nullptr);
                VulkanAllocator::FreeImage(img, alloc);
            });
            m_DepthPreviewImage = VK_NULL_HANDLE;
            m_DepthPreviewView  = VK_NULL_HANDLE;
            m_DepthPreviewAlloc = nullptr;
        }

        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.extent        = { width, height, 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        // RGBA8 — depth blit shader writes a tonemapped grayscale that ImGui
        // can sample directly without HDR clipping concerns.
        ci.format        = VK_FORMAT_R8G8B8A8_UNORM;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ci.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        m_DepthPreviewAlloc = VulkanAllocator::AllocateImage(ci, VMA_MEMORY_USAGE_AUTO, m_DepthPreviewImage);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_DepthPreviewImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = ci.format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(VulkanContext::Get().GetDevice(), &vci, nullptr, &m_DepthPreviewView);

        m_DepthPreviewWidth  = width;
        m_DepthPreviewHeight = height;
        m_DepthPreviewKey    = UINT64_MAX;
    }

    void RenderPipeline::DestroyDepthPreviewTexture()
    {
        if (m_DepthPreviewImage == VK_NULL_HANDLE) return;
        auto dev = VulkanContext::Get().GetDevice();
        vkDestroyImageView(dev, m_DepthPreviewView, nullptr);
        VulkanAllocator::FreeImage(m_DepthPreviewImage, m_DepthPreviewAlloc);
        m_DepthPreviewImage  = VK_NULL_HANDLE;
        m_DepthPreviewView   = VK_NULL_HANDLE;
        m_DepthPreviewAlloc  = nullptr;
        m_DepthPreviewWidth  = 0;
        m_DepthPreviewHeight = 0;
        m_DepthPreviewKey    = UINT64_MAX;
    }

    void RenderPipeline::BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ)
    {
        if (m_System.m_FrameDebugger.state != DebuggerState::Frozen) return;
        if (!m_System.m_FrameDebugger.capturedFrame.valid) return;
        if (archiveIdx >= m_System.m_FrameDebugger.capturedFrame.archivedImages.size()) return;

        auto& archive = m_System.m_FrameDebugger.capturedFrame.archivedImages[archiveIdx];
        if (!archive.isDepth || archive.image == VK_NULL_HANDLE) return;

        InitDebugBlitResources();  // idempotent — needed for sampler + depthPipeline + descSet
        if (!m_System.m_FrameDebugger.depthPipeline || m_System.m_FrameDebugger.descSet == VK_NULL_HANDLE) return;

        // Cache short-circuit (use layer+1 in the low bits so layer == -1 maps to 0).
        const u64 key = ((u64)archiveIdx << 32) | (u64)((layer < 0 ? 0u : (u32)layer + 1u));
        if (key == m_DepthPreviewKey && m_DepthPreviewImage != VK_NULL_HANDLE) return;

        // Preview matches archive dimensions so sampling = 1:1 texel mapping.
        EnsureDepthPreviewTexture(archive.width, archive.height);
        if (m_DepthPreviewImage == VK_NULL_HANDLE) return;

        // Resolve the source view.
        //
        // - Multi-layer archive + layer in range → per-layer view (sampled as 2D).
        // - Single-layer archive (e.g. cascade "ShadowMap.C<i>" — each cascade
        //   pass owns its own single-layer archive backed by the layer-i view of
        //   the shared 4-layer image) → archive.view, which is already 2D.
        // - layer < 0 → whole image (only meaningful for non-array archives).
        //
        // Falling back to archive.view when (layer >= archive.layers) is what
        // makes cascade slices renderable: the EventNode's archiveLayer holds
        // the *cascade index* (0..3) for detail-panel lookups, even though
        // the underlying archive only has 1 layer.
        VkDevice device = VulkanContext::Get().GetDevice();
        VkImageView srcView = (layer < 0 || archive.layers <= 1 || (u32)layer >= archive.layers)
            ? archive.view
            : archive.GetOrCreateLayerView(device, (u32)layer);
        if (srcView == VK_NULL_HANDLE) return;

        // Update the shared debug descriptor to point at this view. Safe to do
        // synchronously: ImmediateSubmit below blocks on a fence so the
        // descriptor isn't in flight while we're rewriting it.
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = m_System.m_FrameDebugger.sampler;
        imgInfo.imageView   = srcView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = m_System.m_FrameDebugger.descSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);

        const u32 width  = archive.width;
        const u32 height = archive.height;
        VkImage     dstImg  = m_DepthPreviewImage;
        VkImageView dstView = m_DepthPreviewView;

        VulkanContext::Get().ImmediateSubmit([this, dstImg, dstView, width, height, nearZ, farZ](VkCommandBuffer cmd)
        {
            // Preview UNDEFINED → COLOR_ATTACHMENT_OPTIMAL (clear-on-load).
            VkImageMemoryBarrier2 prep{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            prep.srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            prep.srcAccessMask       = 0;
            prep.dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            prep.dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            prep.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            prep.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            prep.image               = dstImg;
            prep.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            prep.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            prep.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depPrep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depPrep.imageMemoryBarrierCount = 1;
            depPrep.pImageMemoryBarriers    = &prep;
            vkCmdPipelineBarrier2(cmd, &depPrep);

            AttachmentInfo colorAtt{};
            colorAtt.ImageView  = dstView;
            colorAtt.Format     = VK_FORMAT_R8G8B8A8_UNORM;
            colorAtt.LoadOp     = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt.StoreOp    = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt.ClearValue = { { {0.f, 0.f, 0.f, 1.f} } };
            colorAtt.Layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            RenderPassInfo rpInfo{};
            rpInfo.ColorAttachments = std::span<AttachmentInfo>(&colorAtt, 1);
            rpInfo.RenderArea       = { {0, 0}, { width, height } };

            DynamicRendering::BeginRendering(cmd, rpInfo);

            VkViewport vp{}; vp.width = (float)width; vp.height = (float)height; vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D scRect{}; scRect.extent = { width, height };
            vkCmdSetScissor(cmd, 0, 1, &scRect);

            m_System.m_FrameDebugger.depthPipeline->Bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_System.m_FrameDebugger.depthPipeline->GetLayout(), 0, 1, &m_System.m_FrameDebugger.descSet, 0, nullptr);

            float pc[2] = { nearZ, farZ };
            vkCmdPushConstants(cmd, m_System.m_FrameDebugger.depthPipeline->GetLayout(),
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

            // Fullscreen triangle baked into the vertex shader (3 verts, no VB).
            vkCmdDraw(cmd, 3, 1, 0, 0);

            DynamicRendering::EndRendering(cmd);

            // Preview → SHADER_READ_ONLY for ImGui sampling.
            VkImageMemoryBarrier2 fin{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            fin.srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            fin.srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            fin.dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            fin.dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            fin.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            fin.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fin.image               = dstImg;
            fin.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fin.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fin.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depFin{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depFin.imageMemoryBarrierCount = 1;
            depFin.pImageMemoryBarriers    = &fin;
            vkCmdPipelineBarrier2(cmd, &depFin);
        });

        m_DepthPreviewKey = key;
    }
}
