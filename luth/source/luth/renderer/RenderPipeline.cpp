#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/BoneMatrixBuffer.h"
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
#include "luth/renderer/shader/ShaderCompiler.h"
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
        s.m_Targets.Allocate(viewportWidth, viewportHeight);

        InitGlobalUniforms();
        InitShadowResources();

        // Load shaders via AssetManager (imports + caches SPIR-V on first run)
        UUID pbrUUID = AssetDatabase::GetUUID(FileSystem::EngineAssetsPath("shaders/pbr.vert"));
        auto pbrShader = std::static_pointer_cast<Shader>(AssetManager::LoadImmediate(pbrUUID));
        if (!pbrShader)
        {
            LH_CORE_ERROR("Failed to load PBR shader via AssetManager!");
            return;
        }
        ShaderLibrary::Register("pbr", pbrShader);

        UUID shadowUUID = AssetDatabase::GetUUID(FileSystem::EngineAssetsPath("shaders/shadowDepth.vert"));
        auto shadowShader = std::static_pointer_cast<Shader>(AssetManager::LoadImmediate(shadowUUID));
        if (!shadowShader)
        {
            LH_CORE_ERROR("Failed to load shadow shader via AssetManager!");
            return;
        }
        ShaderLibrary::Register("shadowDepth", shadowShader);

        auto vkPbr = std::static_pointer_cast<VulkanShader>(pbrShader);
        s.m_PBRVertSpv = vkPbr->GetSpirV(VK_SHADER_STAGE_VERTEX_BIT);
        s.m_PBRFragSpv = vkPbr->GetSpirV(VK_SHADER_STAGE_FRAGMENT_BIT);
        if (s.m_PBRVertSpv.empty() || s.m_PBRFragSpv.empty())
        {
            LH_CORE_ERROR("Failed to compile PBR shaders!");
            return;
        }

        auto vkShadow = std::static_pointer_cast<VulkanShader>(shadowShader);
        s.m_ShadowVertSpv = vkShadow->GetSpirV(VK_SHADER_STAGE_VERTEX_BIT);
        s.m_ShadowFragSpv = vkShadow->GetSpirV(VK_SHADER_STAGE_FRAGMENT_BIT);
        if (s.m_ShadowVertSpv.empty() || s.m_ShadowFragSpv.empty())
        {
            LH_CORE_ERROR("Failed to compile shadow shaders!");
            return;
        }

        // Skinned + selection + depth-prepass SPIR-V (compiled inline — not asset pipeline).
        {
            auto shadersPath = FileSystem::EngineAssetsPath("shaders");
            s.m_PBRSkinnedVertSpv            = ShaderCompiler::Compile(shadersPath / "pbr_skinned.vert");
            s.m_ShadowSkinnedVertSpv         = ShaderCompiler::Compile(shadersPath / "shadowDepth_skinned.vert");
            s.m_SelectionMaskVertSpv         = ShaderCompiler::Compile(shadersPath / "selectionMask.vert");
            s.m_SelectionMaskFragSpv         = ShaderCompiler::Compile(shadersPath / "selectionMask.frag");
            s.m_SelectionMaskSkinnedVertSpv  = ShaderCompiler::Compile(shadersPath / "selectionMask_skinned.vert");
            s.m_DepthPrepassVertSpv          = ShaderCompiler::Compile(shadersPath / "depthPrepass.vert");
            s.m_DepthPrepassSkinnedVertSpv   = ShaderCompiler::Compile(shadersPath / "depthPrepass_skinned.vert");

            if (s.m_PBRSkinnedVertSpv.empty() || s.m_ShadowSkinnedVertSpv.empty())
                LH_CORE_ERROR("Failed to compile skinned shaders!");
            if (s.m_SelectionMaskVertSpv.empty() || s.m_SelectionMaskFragSpv.empty())
                LH_CORE_ERROR("Failed to compile selection mask shaders!");
            if (s.m_DepthPrepassVertSpv.empty() || s.m_DepthPrepassSkinnedVertSpv.empty())
                LH_CORE_ERROR("Failed to compile depth prepass shaders!");
        }

        // Post-process / outline / grid SPIR-V.
        {
            auto shadersPath = FileSystem::EngineAssetsPath("shaders");
            s.m_FullscreenVertSpv    = ShaderCompiler::Compile(shadersPath / "fullscreen.vert");
            s.m_BloomExtractFragSpv  = ShaderCompiler::Compile(shadersPath / "bloomExtract.frag");
            s.m_BloomBlurFragSpv     = ShaderCompiler::Compile(shadersPath / "bloomBlur.frag");
            s.m_PostProcessFragSpv   = ShaderCompiler::Compile(shadersPath / "postprocess.frag");
            s.m_OutlineFragSpv       = ShaderCompiler::Compile(shadersPath / "outline.frag");
            s.m_GridFragSpv          = ShaderCompiler::Compile(shadersPath / "grid.frag");

            if (s.m_FullscreenVertSpv.empty() || s.m_BloomExtractFragSpv.empty() ||
                s.m_BloomBlurFragSpv.empty() || s.m_PostProcessFragSpv.empty() ||
                s.m_OutlineFragSpv.empty() || s.m_GridFragSpv.empty())
            {
                LH_CORE_ERROR("Failed to compile post-process shaders!");
            }
        }

        BoneMatrixBuffer::Init();
        InitPostProcessResources();
        InitIBLResources(FileSystem::ResolveAsset("textures/environment.hdr"));
        CreatePipelines();
        InitGPUObjectBuffers();
        InitCullPipeline();
        InitAOResources();

        // Shader hot-reload callback: rebuilds pipelines when a shader reloads.
        ShaderLibrary::SetReloadCallback([this](const std::string& name) {
            auto& s = m_System;
            vkDeviceWaitIdle(VulkanContext::Get().GetDevice());

            if (name == "pbr") {
                auto vk = std::static_pointer_cast<VulkanShader>(ShaderLibrary::Get("pbr"));
                s.m_PBRVertSpv = vk->GetSpirV(VK_SHADER_STAGE_VERTEX_BIT);
                s.m_PBRFragSpv = vk->GetSpirV(VK_SHADER_STAGE_FRAGMENT_BIT);
            } else if (name == "shadowDepth") {
                auto vk = std::static_pointer_cast<VulkanShader>(ShaderLibrary::Get("shadowDepth"));
                s.m_ShadowVertSpv = vk->GetSpirV(VK_SHADER_STAGE_VERTEX_BIT);
                s.m_ShadowFragSpv = vk->GetSpirV(VK_SHADER_STAGE_FRAGMENT_BIT);
            }

            if (name == "pbr") {
                s.m_GeoPipelineManager.InvalidateShader(ShaderLibrary::Get("pbr")->Handle);
                s.m_GeoSkinnedPipelineManager.InvalidateShader(ShaderLibrary::Get("pbr")->Handle);
            } else {
                s.m_GeoPipelineManager.Clear();
                s.m_GeoSkinnedPipelineManager.Clear();
            }
            s.m_ShadowPipeline.reset();
            s.m_ShadowSkinnedPipeline.reset();
            s.m_DepthPrepassPipeline.reset();
            s.m_DepthPrepassSkinnedPipeline.reset();
            s.m_SkyboxPipeline.reset();
            s.m_BloomExtractPipeline.reset();
            s.m_BloomBlurPipeline.reset();
            s.m_PostProcessPipeline.reset();
            s.m_OutlinePipeline.reset();
            s.m_GridPipeline.reset();
            s.m_SelectionMaskPipeline.reset();
            s.m_SelectionMaskSkinnedPipeline.reset();
            CreatePipelines();
            LH_CORE_INFO("Pipelines rebuilt after shader reload: {}", name);
        });

        // File watcher for shader hot-reload (fires on background thread).
        s.m_ShaderWatcher.AddWatch(FileSystem::EngineAssetsPath("shaders"));
        s.m_ShaderWatcher.SetCallback([this](const fs::path& changedFile, FileWatcher::FileStatus status) {
            auto& s = m_System;
            if (status != FileWatcher::FileStatus::Modified) return;

            std::string ext = changedFile.extension().string();
            if (ext != ".vert" && ext != ".frag") return;

            std::string stem = changedFile.stem().string();
            bool matched = false;
            for (const auto& [name, shader] : ShaderLibrary::GetAll()) {
                if (shader->GetPath().stem().string() == stem) {
                    std::lock_guard lock(s.m_ReloadMutex);
                    s.m_PendingReloads.insert(name);
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                std::lock_guard lock(s.m_ReloadMutex);
                s.m_PendingUtilityReload = true;
            }
        });
        s.m_ShaderWatcher.Start(true);

        // Capacity covers worst-case current frame (5 cull + 4 shadow cascades +
        // geometry + selection + skybox + 3 bloom + grid + post-process + outline
        // + ImGui ≈ 19 passes) with headroom for future passes (GTAO etc.).
        s.m_GPUTimers.Init(64);
        RegisterNamedTextures();
    }

    void RenderPipeline::Shutdown()
    {
        auto& s = m_System;

        s.m_ShaderWatcher.Stop();
        ShaderLibrary::SetReloadCallback(nullptr);
        s.m_GPUTimers.Shutdown();

        BoneMatrixBuffer::Shutdown();

        VkDevice device = VulkanContext::Get().GetDevice();

        s.DestroyPerDrawPreviewTexture();
        s.DestroyDepthPreviewTexture();
        s.m_FrameDebugger.Shutdown(device);

        if (s.m_OutlineSampler)       vkDestroySampler(device, s.m_OutlineSampler, nullptr);
        if (s.m_OutlineDescSetLayout) vkDestroyDescriptorSetLayout(device, s.m_OutlineDescSetLayout, nullptr);
        if (s.m_OutlineDescPool)      vkDestroyDescriptorPool(device, s.m_OutlineDescPool, nullptr);
        if (s.m_GridDepthSampler)     vkDestroySampler(device, s.m_GridDepthSampler, nullptr);
        if (s.m_GridDescSetLayout)    vkDestroyDescriptorSetLayout(device, s.m_GridDescSetLayout, nullptr);
        if (s.m_GridDescPool)         vkDestroyDescriptorPool(device, s.m_GridDescPool, nullptr);
        if (s.m_PPSampler)            vkDestroySampler(device, s.m_PPSampler, nullptr);
        if (s.m_PPDescSetLayout)      vkDestroyDescriptorSetLayout(device, s.m_PPDescSetLayout, nullptr);
        if (s.m_PPDescPool)           vkDestroyDescriptorPool(device, s.m_PPDescPool, nullptr);
        if (s.m_IBLSampler)           vkDestroySampler(device, s.m_IBLSampler, nullptr);
        if (s.m_ShadowSampler)        vkDestroySampler(device, s.m_ShadowSampler, nullptr);
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i) {
            if (s.m_ShadowLayerViews[i]) vkDestroyImageView(device, s.m_ShadowLayerViews[i], nullptr);
        }
        if (s.m_LightSetLayout)  vkDestroyDescriptorSetLayout(device, s.m_LightSetLayout, nullptr);
        if (s.m_LightDescPool)   vkDestroyDescriptorPool(device, s.m_LightDescPool, nullptr);
        if (s.m_GlobalSetLayout) vkDestroyDescriptorSetLayout(device, s.m_GlobalSetLayout, nullptr);

        if (s.m_ObjectSSBO) {
            VulkanAllocator::Unmap(s.m_ObjectSSBOAlloc);
            VulkanAllocator::FreeBuffer(s.m_ObjectSSBO, s.m_ObjectSSBOAlloc);
        }
        if (s.m_IndirectBuffer) {
            VulkanAllocator::Unmap(s.m_IndirectBufferAlloc);
            VulkanAllocator::FreeBuffer(s.m_IndirectBuffer, s.m_IndirectBufferAlloc);
        }
        if (s.m_ObjectSSBODescPool)   vkDestroyDescriptorPool(device, s.m_ObjectSSBODescPool, nullptr);
        if (s.m_ObjectSSBODescLayout) vkDestroyDescriptorSetLayout(device, s.m_ObjectSSBODescLayout, nullptr);
        if (s.m_CullDescLayout)       vkDestroyDescriptorSetLayout(device, s.m_CullDescLayout, nullptr);
        s.m_CullPipeline.reset();

        // GTAO resources (epic #58).
        s.m_GTAOPrefilterPipeline.reset();
        s.m_GTAOMainPipeline.reset();
        s.m_GTAODenoisePipeline.reset();
        if (s.m_GTAOSampler)            vkDestroySampler(device, s.m_GTAOSampler, nullptr);
        if (s.m_GTAODescPool)           vkDestroyDescriptorPool(device, s.m_GTAODescPool, nullptr);
        if (s.m_GTAOPrefilterDescLayout) vkDestroyDescriptorSetLayout(device, s.m_GTAOPrefilterDescLayout, nullptr);
        if (s.m_GTAOMainDescLayout)      vkDestroyDescriptorSetLayout(device, s.m_GTAOMainDescLayout, nullptr);
        if (s.m_GTAODenoiseDescLayout)   vkDestroyDescriptorSetLayout(device, s.m_GTAODenoiseDescLayout, nullptr);
        // Textures + UBO destroyed automatically via shared_ptr reset when RenderingSystem dies.
    }

    void RenderPipeline::OnResize(u32 width, u32 height)
    {
        auto& s = m_System;

        s.m_BloomA = Texture::Create(std::max(width / 2, 1u), std::max(height / 2, 1u), TextureFormat::RGBA16F);
        s.m_BloomB = Texture::Create(std::max(width / 2, 1u), std::max(height / 2, 1u), TextureFormat::RGBA16F);
        UpdatePostProcessDescriptors();

        // GTAO half-res storage textures (recreated on resize; descriptors
        // refreshed below because they cache image-view pointers).
        {
            const u32 halfW = std::max(width  / 2, 1u);
            const u32 halfH = std::max(height / 2, 1u);
            auto makeStorage = [&](TextureFormat fmt) {
                return std::make_shared<VKTexture>(halfW, halfH, fmt, 1u, 0u, 1u, VK_IMAGE_USAGE_STORAGE_BIT);
            };
            s.m_GTAOLinearDepth = makeStorage(TextureFormat::R32_Float);
            s.m_GTAORawAO       = makeStorage(TextureFormat::R8);
            s.m_GTAOEdges       = makeStorage(TextureFormat::R8);
            s.m_GTAOFinal       = makeStorage(TextureFormat::R8);
        }
        UpdateAODescriptors();

        // Update outline descriptors — mask + depth buffer views changed.
        if (s.m_OutlineDescSet && s.m_OutlineSampler)
        {
            auto vkMask       = std::static_pointer_cast<VKTexture>(s.m_Targets.GetSelectionMask());
            auto vkSelDepth   = std::static_pointer_cast<VKTexture>(s.m_Targets.GetSelectionDepth());
            auto vkSceneDepth = std::static_pointer_cast<VKTexture>(s.m_Targets.GetSceneDepth());

            VkDescriptorImageInfo maskImgInfo{};
            maskImgInfo.sampler     = s.m_OutlineSampler;
            maskImgInfo.imageView   = vkMask->GetImageView();
            maskImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo selDepthImgInfo{};
            selDepthImgInfo.sampler     = s.m_OutlineSampler;
            selDepthImgInfo.imageView   = vkSelDepth->GetImageView();
            selDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo sceneDepthImgInfo{};
            sceneDepthImgInfo.sampler     = s.m_OutlineSampler;
            sceneDepthImgInfo.imageView   = vkSceneDepth->GetImageView();
            sceneDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3] = {};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = s.m_OutlineDescSet;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &maskImgInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = s.m_OutlineDescSet;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &selDepthImgInfo;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = s.m_OutlineDescSet;
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &sceneDepthImgInfo;

            vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 3, writes, 0, nullptr);
        }

        // Update grid descriptor set: scene depth view changed on resize.
        if (s.m_GridDescSet && s.m_GridDepthSampler)
        {
            auto vkSceneDepth = std::static_pointer_cast<VKTexture>(s.m_Targets.GetSceneDepth());

            VkDescriptorImageInfo gridDepthImgInfo{};
            gridDepthImgInfo.sampler     = s.m_GridDepthSampler;
            gridDepthImgInfo.imageView   = vkSceneDepth->GetImageView();
            gridDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet gridWrite{};
            gridWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gridWrite.dstSet = s.m_GridDescSet;
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
        RG::RenderGraph rg(*s.m_FrameAllocator);
        AddImGuiPass(rg, RG::ResourceHandle{}); // invalid → ImGuiPass skips the optional Read
        rg.Compile();
        Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex(), nullptr);
    }

    void RenderPipeline::Execute(entt::registry& registry)
    {
        auto& s = m_System;

        RG::RenderGraph rg(*s.m_FrameAllocator);

        // Import persistent buffers into the render graph for barrier tracking.
        RG::BufferDesc objDesc {
            "ObjectSSBO",
            RenderingSystem::k_MaxGPUObjects * sizeof(GPUObjectData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        };
        RG::BufferDesc indDesc {
            "IndirectBuffer",
            RenderingSystem::k_IndirectRegionCount * RenderingSystem::k_IndirectRegionStride * sizeof(VkDrawIndexedIndirectCommand),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
        };
        RG::BufferHandle hObjectBuf   = rg.ImportBuffer(objDesc, (void*)s.m_ObjectSSBO,    RG::ResourceState::Undefined);
        RG::BufferHandle hIndirectBuf = rg.ImportBuffer(indDesc, (void*)s.m_IndirectBuffer, RG::ResourceState::Undefined);

        // Frustum cull — 5 dispatches: camera region + 4 shadow cascade regions.
        // Each cascade uses its own light-space viewProj frustum so shadow casters
        // outside the camera frustum but inside the cascade still get rendered.
        {
            Frustum camFrustum = CreateFrustumFromCamera(s.m_CachedViewProj);
            AddCullComputePass(rg, hObjectBuf, hIndirectBuf,
                s.m_CullPipeline.get(), s.m_CullDescSet, camFrustum.planes, s.m_GPUObjectCount,
                /*destOffset*/ 0, "FrustumCull.Cam", &s.m_FrameDebugger);

            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            {
                Frustum cascadeFrustum = CreateFrustumFromCamera(s.m_Cascades.lightSpaceMatrix[i]);
                const u32 destOffset = (i + 1) * RenderingSystem::k_IndirectRegionStride;
                const std::string name = "FrustumCull.C" + std::to_string(i);
                AddCullComputePass(rg, hObjectBuf, hIndirectBuf,
                    s.m_CullPipeline.get(), s.m_CullDescSet, cascadeFrustum.planes, s.m_GPUObjectCount,
                    destOffset, name.c_str(), &s.m_FrameDebugger);
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
        RG::ResourceHandle gridColor   = s.m_GridVisible
                                         ? AddGridPass(rg, skyboxColor, geoOutput.depth)
                                         : skyboxColor;
        RG::ResourceHandle ldrOutput   = AddPostProcessPass(rg, gridColor, bloomResult);
        RG::ResourceHandle finalOutput = AddOutlinePass(rg, ldrOutput, maskOutput, geoOutput.depth);
        AddImGuiPass(rg, finalOutput);

        rg.Compile();

        // Capture render graph snapshot for Frame Debugger panel
        s.m_GraphSnapshot = CaptureSnapshot(rg);

        // Read GPU timing from completed frames and fill snapshot
        std::vector<float> gpuTimes;
        u32 nonCulledCount = 0;
        for (auto& p : s.m_GraphSnapshot.passes)
            if (!p.culled) nonCulledCount++;

        s.m_GPUTimers.ReadResults(nonCulledCount, gpuTimes);
        float totalMs = 0.0f;
        u32 timerIdx = 0;
        for (auto& p : s.m_GraphSnapshot.passes)
        {
            if (p.culled) continue;
            if (timerIdx < (u32)gpuTimes.size())
            {
                p.gpuTimeMs = gpuTimes[timerIdx];
                if (gpuTimes[timerIdx] > 0.0f) totalMs += gpuTimes[timerIdx];
            }
            timerIdx++;
        }
        s.m_GraphSnapshot.totalGpuTimeMs = totalMs;

        // --- Phase 14B — Wire archive sink for the capture frame ---
        // The sink will copy each tracked render target into a fresh ArchivedImage
        // after each pass that writes it. Keep the tracked-RT set tight to bound
        // memory (~50 MB at 1080p for the v1 set). The sink is a no-op when state
        // != CaptureRequested, so re-checking here is sufficient.
        if (s.m_FrameDebugger.state == DebuggerState::CaptureRequested)
        {
            // Phase 14D — ensure the debug sampler exists for ImGui archive previews.
            // Idempotent: returns immediately once blitPipeline is set.
            s.InitDebugBlitResources();

            // Phase 14E — invalidate the per-draw replay cache. The cache
            // is keyed by (passIdx, drawIdx) which can collide across
            // captures even though the underlying scene state has changed
            // (camera moved → recapture → same passIdx/drawIdx but new
            // GPUObjectData / IndirectBuffer contents). Without this reset,
            // re-clicking the same draw after recapture would hit the
            // stale cached preview.
            s.m_PerDrawPreviewKey = UINT64_MAX;
            // Same for the Phase 14F depth-preview blit cache — keyed by
            // (archiveIdx, layer+1); recapture rebuilds archives at the
            // same indices, so without this reset, re-selecting a depth
            // pass would skip the re-blit and show the previous frame.
            s.m_DepthPreviewKey = UINT64_MAX;

            s.m_FrameDebugger.BeginCapture(VulkanContext::Get().GetDevice(),
                                            VulkanContext::Get().GetAllocator());
            s.m_FrameDebugger.RegisterTrackedRT("SceneColor");
            s.m_FrameDebugger.RegisterTrackedRT("SceneDepth");
            // Phase 13 ShadowPass imports per-cascade resources named
            // "ShadowMap.C<i>" (one per cascade, single-layer view onto
            // the shared 4-layer array). Track each variant so the sink
            // archives them — without this, cascade nodes have no
            // primary output and the panel shows "no output preview".
            for (u32 ci = 0; ci < k_ShadowCascadeCount; ++ci)
                s.m_FrameDebugger.RegisterTrackedRT("ShadowMap.C" + std::to_string(ci));
            s.m_FrameDebugger.RegisterTrackedRT("LDROutput");
            s.m_FrameDebugger.RegisterTrackedRT("EntityID");
            s.m_FrameDebugger.RegisterTrackedRT("BloomAFinal");
            s.m_FrameDebugger.RegisterTrackedRT("GTAOLinearDepth");
            s.m_FrameDebugger.RegisterTrackedRT("GTAORawAO");
            s.m_FrameDebugger.RegisterTrackedRT("GTAOFinal");
            rg.SetArchiveSink(&s.m_FrameDebugger);
        }

        Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex(), &s.m_GPUTimers);

        // --- Frame Debugger: Finalize capture and enter frozen state ---
        if (s.m_FrameDebugger.state == DebuggerState::CaptureRequested)
        {
            // Phase 14C — captured*Draws / drawLimit removed.
            // Per-draw replay (Phase 14E) re-derives draw inputs from the
            // CapturedDrawCall records + frozen indirect/object SSBOs.

            // Copy resource and timing info from the graph snapshot
            s.m_FrameDebugger.capturedFrame.resources      = s.m_GraphSnapshot.resources;
            s.m_FrameDebugger.capturedFrame.totalGpuTimeMs = s.m_GraphSnapshot.totalGpuTimeMs;

            // Copy per-pass GPU times into captured passes
            {
                u32 capturedIdx = 0;
                for (auto& ps : s.m_GraphSnapshot.passes)
                {
                    if (ps.culled) continue;
                    if (capturedIdx < s.m_FrameDebugger.capturedFrame.passes.size())
                        s.m_FrameDebugger.capturedFrame.passes[capturedIdx].gpuTimeMs = ps.gpuTimeMs;
                    capturedIdx++;
                }
            }

            // Snapshot capture-time camera viewProj for the Frozen-state
            // auto-recapture comparison (see top of Update).
            s.m_FrameDebugger.FinalizeCapture(s.m_CachedViewProj);

            // Phase 14F — stamp CSM state into the captured frame so the
            // cascade detail panel can show GPU-true values from the moment
            // of capture, even if the user later twiddles light settings.
            s.m_FrameDebugger.capturedFrame.cascadeSplitsViewZ = s.m_Cascades.splitsViewZ;
            s.m_FrameDebugger.capturedFrame.shadowBias         = s.m_ShadowParams.shadowBias;
            s.m_FrameDebugger.capturedFrame.shadowNormalBias   = s.m_ShadowParams.shadowNormalBias;
            s.m_FrameDebugger.capturedFrame.cascadeTexelSize   = s.m_Cascades.texelSize;
            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                s.m_FrameDebugger.capturedFrame.lightSpaceMatrix[i] = s.m_Cascades.lightSpaceMatrix[i];

            s.m_FrameDebugger.capturedFrame.valid = true;
            s.m_FrameDebugger.state               = DebuggerState::Frozen;
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
        u32 totalDraws = (u32)(s.m_DrawList.opaque.size() + s.m_DrawList.cutout.size() + s.m_DrawList.transparent.size());
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
        sumIndices(s.m_DrawList.opaque);
        sumIndices(s.m_DrawList.cutout);
        sumIndices(s.m_DrawList.transparent);

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
    void RenderPipeline::RecompileUtilityShaders()
    {
        auto shadersPath = FileSystem::EngineAssetsPath("shaders");

        m_System.m_PBRSkinnedVertSpv           = ShaderCompiler::Compile(shadersPath / "pbr_skinned.vert");
        m_System.m_ShadowSkinnedVertSpv        = ShaderCompiler::Compile(shadersPath / "shadowDepth_skinned.vert");
        m_System.m_SelectionMaskVertSpv        = ShaderCompiler::Compile(shadersPath / "selectionMask.vert");
        m_System.m_SelectionMaskFragSpv        = ShaderCompiler::Compile(shadersPath / "selectionMask.frag");
        m_System.m_SelectionMaskSkinnedVertSpv = ShaderCompiler::Compile(shadersPath / "selectionMask_skinned.vert");
        m_System.m_DepthPrepassVertSpv         = ShaderCompiler::Compile(shadersPath / "depthPrepass.vert");
        m_System.m_DepthPrepassSkinnedVertSpv  = ShaderCompiler::Compile(shadersPath / "depthPrepass_skinned.vert");
        m_System.m_GTAOPrefilterSpv            = ShaderCompiler::Compile(shadersPath / "gtao_depth_prefilter.comp");
        m_System.m_GTAOMainSpv                 = ShaderCompiler::Compile(shadersPath / "gtao_main.comp");
        m_System.m_GTAODenoiseSpv              = ShaderCompiler::Compile(shadersPath / "gtao_denoise.comp");

        m_System.m_FullscreenVertSpv   = ShaderCompiler::Compile(shadersPath / "fullscreen.vert");
        m_System.m_BloomExtractFragSpv = ShaderCompiler::Compile(shadersPath / "bloomExtract.frag");
        m_System.m_BloomBlurFragSpv    = ShaderCompiler::Compile(shadersPath / "bloomBlur.frag");
        m_System.m_PostProcessFragSpv  = ShaderCompiler::Compile(shadersPath / "postprocess.frag");
        m_System.m_OutlineFragSpv      = ShaderCompiler::Compile(shadersPath / "outline.frag");
        m_System.m_GridFragSpv         = ShaderCompiler::Compile(shadersPath / "grid.frag");

        m_System.m_SkyboxVertSpv = ShaderCompiler::Compile(shadersPath / "skybox.vert");
        m_System.m_SkyboxFragSpv = ShaderCompiler::Compile(shadersPath / "skybox.frag");

        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
        m_System.m_GeoPipelineManager.Clear();
        m_System.m_GeoSkinnedPipelineManager.Clear();
        m_System.m_ShadowPipeline.reset();
        m_System.m_ShadowSkinnedPipeline.reset();
        m_System.m_DepthPrepassPipeline.reset();
        m_System.m_DepthPrepassSkinnedPipeline.reset();
        m_System.m_SkyboxPipeline.reset();
        m_System.m_BloomExtractPipeline.reset();
        m_System.m_BloomBlurPipeline.reset();
        m_System.m_PostProcessPipeline.reset();
        m_System.m_OutlinePipeline.reset();
        m_System.m_GridPipeline.reset();
        m_System.m_SelectionMaskPipeline.reset();
        m_System.m_SelectionMaskSkinnedPipeline.reset();
        CreatePipelines();

        // Rebuild GTAO compute pipelines from freshly-compiled SPIR-V. The
        // descriptor layouts are unchanged, so the descriptor sets survive.
        if (!m_System.m_GTAOPrefilterSpv.empty() && m_System.m_GTAOPrefilterDescLayout)
        {
            VkPushConstantRange pc{};
            pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pc.offset     = 0;
            pc.size       = sizeof(i32) * 2 + sizeof(float) * 6;
            m_System.m_GTAOPrefilterPipeline = std::make_unique<VKComputePipeline>(
                m_System.m_GTAOPrefilterSpv,
                std::vector<VkDescriptorSetLayout>{ m_System.m_GTAOPrefilterDescLayout },
                std::vector<VkPushConstantRange>{ pc });
        }
        if (!m_System.m_GTAOMainSpv.empty() && m_System.m_GTAOMainDescLayout)
        {
            VkPushConstantRange pc{};
            pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pc.offset     = 0;
            pc.size       = sizeof(float) * 4 + sizeof(u32) * 4;
            m_System.m_GTAOMainPipeline = std::make_unique<VKComputePipeline>(
                m_System.m_GTAOMainSpv,
                std::vector<VkDescriptorSetLayout>{ m_System.m_GTAOMainDescLayout },
                std::vector<VkPushConstantRange>{ pc });
        }
        if (!m_System.m_GTAODenoiseSpv.empty() && m_System.m_GTAODenoiseDescLayout)
        {
            m_System.m_GTAODenoisePipeline = std::make_unique<VKComputePipeline>(
                m_System.m_GTAODenoiseSpv,
                std::vector<VkDescriptorSetLayout>{ m_System.m_GTAODenoiseDescLayout },
                std::vector<VkPushConstantRange>{});
        }

        LH_CORE_INFO("Utility shaders recompiled and pipelines rebuilt");
    }

    // =========================================================================
    // Initialization
    // =========================================================================

    void RenderPipeline::InitGlobalUniforms()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        m_System.m_GlobalUniformBuffer = std::make_shared<VKUniformBuffer>(sizeof(GlobalUniforms));

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
        // once m_System.m_GTAOFinal is allocated (InitAOResources runs after InitGlobalUniforms).
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

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_System.m_GlobalSetLayout);

        VulkanContext::Get().GetDescriptorAllocator().Allocate(m_System.m_GlobalSetLayout, m_System.m_GlobalDescriptorSet);

        // Write binding 0 (UBO) immediately; bindings 1-3 written after IBL init
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_System.m_GlobalUniformBuffer->GetVulkanBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(GlobalUniforms);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_System.m_GlobalDescriptorSet;
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
        m_System.m_ShadowMap = std::make_shared<VKTexture>(
            k_ShadowResolution, k_ShadowResolution, TextureFormat::D32_Float,
            k_ShadowCascadeCount, /*createFlags*/ 0u, /*mipLevels*/ 1u, /*extraUsage*/ 0u);

        // Per-layer 2D views for ShadowPass.Ci depth attachments (Phase 13C).
        auto shadowTexForViews = std::static_pointer_cast<VKTexture>(m_System.m_ShadowMap);
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            m_System.m_ShadowLayerViews[i] = shadowTexForViews->CreateLayerView(i);

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
        vkCreateSampler(device, &samplerInfo, nullptr, &m_System.m_ShadowSampler);

        // --- Light UBO ---
        m_System.m_LightUniformBuffer = std::make_shared<VKUniformBuffer>(sizeof(LightUniforms));

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
        vkCreateDescriptorSetLayout(device, &lightLayoutInfo, nullptr, &m_System.m_LightSetLayout);

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
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_System.m_LightDescPool);

        // --- Allocate set ---
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_System.m_LightDescPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_System.m_LightSetLayout;
        vkAllocateDescriptorSets(device, &allocInfo, &m_System.m_LightDescSet);

        // --- Write descriptors ---
        VkDescriptorBufferInfo lightBufInfo{};
        lightBufInfo.buffer = m_System.m_LightUniformBuffer->GetVulkanBuffer();
        lightBufInfo.offset = 0;
        lightBufInfo.range = sizeof(LightUniforms);

        auto vkShadowTex = std::static_pointer_cast<VKTexture>(m_System.m_ShadowMap);
        VkDescriptorImageInfo shadowImgInfo{};
        shadowImgInfo.sampler     = m_System.m_ShadowSampler;
        shadowImgInfo.imageView   = vkShadowTex->GetImageView();
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_System.m_LightDescSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &lightBufInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_System.m_LightDescSet;
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
        m_System.m_BloomA = Texture::Create(std::max(w / 2, 1u), std::max(h / 2, 1u), TextureFormat::RGBA16F);
        m_System.m_BloomB = Texture::Create(std::max(w / 2, 1u), std::max(h / 2, 1u), TextureFormat::RGBA16F);

        // Post-process UBO
        m_System.m_PostProcessUBOBuffer = std::make_shared<VKUniformBuffer>(sizeof(PostProcessUBO));

        // Linear clamp sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        vkCreateSampler(device, &samplerInfo, nullptr, &m_System.m_PPSampler);

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
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_System.m_PPDescSetLayout);

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
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_System.m_PPDescPool);

        // Allocate 4 descriptor sets
        VkDescriptorSetLayout setLayouts[4] = { m_System.m_PPDescSetLayout, m_System.m_PPDescSetLayout, m_System.m_PPDescSetLayout, m_System.m_PPDescSetLayout };
        VkDescriptorSet sets[4];
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_System.m_PPDescPool;
        allocInfo.descriptorSetCount = 4;
        allocInfo.pSetLayouts = setLayouts;
        vkAllocateDescriptorSets(device, &allocInfo, sets);

        m_System.m_BloomExtractDescSet = sets[0];
        m_System.m_BloomBlurHDescSet   = sets[1];
        m_System.m_BloomBlurVDescSet   = sets[2];
        m_System.m_CompositeDescSet    = sets[3];

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
            vkCreateSampler(device, &outlineSamplerInfo, nullptr, &m_System.m_OutlineSampler);

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
            vkCreateDescriptorSetLayout(device, &outlineLayoutInfo, nullptr, &m_System.m_OutlineDescSetLayout);

            // Descriptor pool: 1 set, 3 combined image samplers
            VkDescriptorPoolSize outlinePoolSize{};
            outlinePoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            outlinePoolSize.descriptorCount = 3;

            VkDescriptorPoolCreateInfo outlinePoolInfo{};
            outlinePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            outlinePoolInfo.maxSets = 1;
            outlinePoolInfo.poolSizeCount = 1;
            outlinePoolInfo.pPoolSizes = &outlinePoolSize;
            vkCreateDescriptorPool(device, &outlinePoolInfo, nullptr, &m_System.m_OutlineDescPool);

            // Allocate descriptor set
            VkDescriptorSetAllocateInfo outlineAllocInfo{};
            outlineAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            outlineAllocInfo.descriptorPool = m_System.m_OutlineDescPool;
            outlineAllocInfo.descriptorSetCount = 1;
            outlineAllocInfo.pSetLayouts = &m_System.m_OutlineDescSetLayout;
            vkAllocateDescriptorSets(device, &outlineAllocInfo, &m_System.m_OutlineDescSet);

            // Write all 3 descriptors: selection mask, selection depth, scene depth
            auto vkMask      = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSelectionMask());
            auto vkSelDepth  = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSelectionDepth());
            auto vkScnDepth  = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneDepth());

            VkDescriptorImageInfo maskImgInfo{};
            maskImgInfo.sampler     = m_System.m_OutlineSampler;
            maskImgInfo.imageView   = vkMask->GetImageView();
            maskImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo selDepthImgInfo{};
            selDepthImgInfo.sampler     = m_System.m_OutlineSampler;
            selDepthImgInfo.imageView   = vkSelDepth->GetImageView();
            selDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo scnDepthImgInfo{};
            scnDepthImgInfo.sampler     = m_System.m_OutlineSampler;
            scnDepthImgInfo.imageView   = vkScnDepth->GetImageView();
            scnDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3] = {};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_System.m_OutlineDescSet;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &maskImgInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = m_System.m_OutlineDescSet;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &selDepthImgInfo;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = m_System.m_OutlineDescSet;
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
            vkCreateSampler(device, &gridSamplerInfo, nullptr, &m_System.m_GridDepthSampler);

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
            vkCreateDescriptorSetLayout(device, &gridLayoutInfo, nullptr, &m_System.m_GridDescSetLayout);

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
            vkCreateDescriptorPool(device, &gridPoolInfo, nullptr, &m_System.m_GridDescPool);

            VkDescriptorSetAllocateInfo gridAllocInfo{};
            gridAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            gridAllocInfo.descriptorPool = m_System.m_GridDescPool;
            gridAllocInfo.descriptorSetCount = 1;
            gridAllocInfo.pSetLayouts = &m_System.m_GridDescSetLayout;
            vkAllocateDescriptorSets(device, &gridAllocInfo, &m_System.m_GridDescSet);

            // Write: global UBO + scene depth
            VkDescriptorBufferInfo gridUBOInfo{};
            gridUBOInfo.buffer = m_System.m_GlobalUniformBuffer->GetVulkanBuffer();
            gridUBOInfo.offset = 0;
            gridUBOInfo.range  = sizeof(GlobalUniforms);

            auto vkScnDepthGrid = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneDepth());
            VkDescriptorImageInfo gridDepthImgInfo{};
            gridDepthImgInfo.sampler     = m_System.m_GridDepthSampler;
            gridDepthImgInfo.imageView   = vkScnDepthGrid->GetImageView();
            gridDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet gridWrites[2] = {};
            gridWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gridWrites[0].dstSet = m_System.m_GridDescSet;
            gridWrites[0].dstBinding = 0;
            gridWrites[0].descriptorCount = 1;
            gridWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            gridWrites[0].pBufferInfo = &gridUBOInfo;

            gridWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gridWrites[1].dstSet = m_System.m_GridDescSet;
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
        auto bloomAVk = std::static_pointer_cast<VKTexture>(m_System.m_BloomA);
        auto bloomBVk = std::static_pointer_cast<VKTexture>(m_System.m_BloomB);

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = m_System.m_PostProcessUBOBuffer->GetVulkanBuffer();
        uboInfo.offset = 0;
        uboInfo.range  = sizeof(PostProcessUBO);

        // Helper: write a combined image sampler descriptor
        auto MakeImageInfo = [this](VkImageView view) -> VkDescriptorImageInfo {
            VkDescriptorImageInfo info{};
            info.sampler     = m_System.m_PPSampler;
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
        AddWrite(m_System.m_BloomExtractDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bloomExtractImg0, nullptr);
        AddWrite(m_System.m_BloomExtractDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bloomExtractImg1, nullptr);
        AddWrite(m_System.m_BloomExtractDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // BloomBlurH set
        AddWrite(m_System.m_BloomBlurHDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurHImg0, nullptr);
        AddWrite(m_System.m_BloomBlurHDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurHImg1, nullptr);
        AddWrite(m_System.m_BloomBlurHDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // BloomBlurV set
        AddWrite(m_System.m_BloomBlurVDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurVImg0, nullptr);
        AddWrite(m_System.m_BloomBlurVDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurVImg1, nullptr);
        AddWrite(m_System.m_BloomBlurVDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // Composite set
        AddWrite(m_System.m_CompositeDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &compImg0, nullptr);
        AddWrite(m_System.m_CompositeDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &compImg1, nullptr);
        AddWrite(m_System.m_CompositeDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        vkUpdateDescriptorSets(device, idx, writes, 0, nullptr);
    }

    void RenderPipeline::InitIBLResources(const fs::path& hdrPath)
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Run precomputation (equirect -> cubemap -> irradiance -> prefilter -> BRDF LUT)
        IBLResult ibl = IBL::Precompute(hdrPath);

        m_System.m_IrradianceMap  = ibl.irradianceMap;
        m_System.m_PrefilteredMap = ibl.prefilteredMap;
        m_System.m_BRDFLut        = ibl.brdfLut;
        m_System.m_IBLSampler     = ibl.iblSampler;
        m_System.m_SkyboxVB       = ibl.skyboxVB;
        m_System.m_SkyboxVertSpv  = std::move(ibl.skyboxVertSpv);
        m_System.m_SkyboxFragSpv  = std::move(ibl.skyboxFragSpv);

        // Write IBL descriptors to Set 0 (bindings 1-3)
        {
            auto vkIrr = std::static_pointer_cast<VKTexture>(m_System.m_IrradianceMap);
            auto vkPf  = std::static_pointer_cast<VKTexture>(m_System.m_PrefilteredMap);
            auto vkLut = std::static_pointer_cast<VKTexture>(m_System.m_BRDFLut);

            VkDescriptorImageInfo irrInfo{};
            irrInfo.sampler = m_System.m_IBLSampler;
            irrInfo.imageView = vkIrr->GetImageView();
            irrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo pfInfo{};
            pfInfo.sampler = m_System.m_IBLSampler;
            pfInfo.imageView = vkPf->GetImageView();
            pfInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo lutInfo{};
            lutInfo.sampler = m_System.m_IBLSampler;
            lutInfo.imageView = vkLut->GetImageView();
            lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3] = {};
            writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[0].dstSet = m_System.m_GlobalDescriptorSet;
            writes[0].dstBinding = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &irrInfo;

            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[1].dstSet = m_System.m_GlobalDescriptorSet;
            writes[1].dstBinding = 2;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &pfInfo;

            writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[2].dstSet = m_System.m_GlobalDescriptorSet;
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
        if (m_System.m_IBLSampler) {
            vkDestroySampler(device, m_System.m_IBLSampler, nullptr);
            m_System.m_IBLSampler = VK_NULL_HANDLE;
        }

        InitIBLResources(hdrPath);

        // Rebuild skybox pipeline (new prefiltered map may have different mip count)
        m_System.m_SkyboxPipeline.reset();
        CreatePipelines();

        LH_CORE_INFO("Skybox reloaded from '{}'", hdrPath.string());
    }

    // =========================================================================
    // Pipeline creation
    // =========================================================================

    void RenderPipeline::InitObjectSSBODescriptorLayout()
    {
        if (m_System.m_ObjectSSBODescLayout != VK_NULL_HANDLE)
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

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_System.m_ObjectSSBODescLayout);
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
            m_System.m_GlobalSetLayout,                                    // Set 0
            VulkanContext::Get().GetBindlessSet().GetLayout(),   // Set 1
            MaterialSystem::GetDescriptorSetLayout(),            // Set 2
            m_System.m_LightSetLayout,                                    // Set 3
            BoneMatrixBuffer::GetDescriptorSetLayout()           // Set 4
        };

        // 6-set layout for geometry pipelines (adds Set 5 = GPUObjectData SSBO, no push constants)
        std::vector<VkDescriptorSetLayout> geoLayouts = {
            m_System.m_GlobalSetLayout,                                    // Set 0
            VulkanContext::Get().GetBindlessSet().GetLayout(),   // Set 1
            MaterialSystem::GetDescriptorSetLayout(),            // Set 2
            m_System.m_LightSetLayout,                                    // Set 3
            BoneMatrixBuffer::GetDescriptorSetLayout(),          // Set 4
            m_System.m_ObjectSSBODescLayout                               // Set 5
        };

        // ---- PBR geometry pipeline manager (lazy creation keyed by {shaderUUID, renderMode}) ----
        m_System.m_GeoPipelineManager.Init(geoLayouts,
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

        m_System.m_ShadowPipeline = std::make_unique<VKPipeline>(shadowConfig, m_System.m_ShadowVertSpv, m_System.m_ShadowFragSpv, geoLayouts);

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

        m_System.m_GeoSkinnedPipelineManager.Init(geoLayouts,
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
        if (!m_System.m_ShadowSkinnedVertSpv.empty())
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

            m_System.m_ShadowSkinnedPipeline = std::make_unique<VKPipeline>(
                shadowSkinnedConfig, m_System.m_ShadowSkinnedVertSpv, m_System.m_ShadowFragSpv, geoLayouts);
        }

        // ---- Depth prepass pipeline (camera-space, depth-only, position only) ----
        // Reuses the shadow frag SPIR-V (empty `void main(){}`) as the null fragment.
        // Rigid variant uses the position-only binding/attribs (shadowVertexLayout).
        if (!m_System.m_DepthPrepassVertSpv.empty() && !m_System.m_ShadowFragSpv.empty())
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

            m_System.m_DepthPrepassPipeline = std::make_unique<VKPipeline>(
                depthPrepassConfig, m_System.m_DepthPrepassVertSpv, m_System.m_ShadowFragSpv, geoLayouts);
        }

        if (!m_System.m_DepthPrepassSkinnedVertSpv.empty() && !m_System.m_ShadowFragSpv.empty())
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

            m_System.m_DepthPrepassSkinnedPipeline = std::make_unique<VKPipeline>(
                depthPrepassSkinnedConfig, m_System.m_DepthPrepassSkinnedVertSpv, m_System.m_ShadowFragSpv, geoLayouts);
        }

        // ---- Selection mask pipeline (static) ----
        if (!m_System.m_SelectionMaskVertSpv.empty() && !m_System.m_SelectionMaskFragSpv.empty())
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

            m_System.m_SelectionMaskPipeline = std::make_unique<VKPipeline>(
                maskConfig, m_System.m_SelectionMaskVertSpv, m_System.m_SelectionMaskFragSpv, layouts);
        }

        // ---- Selection mask pipeline (skinned) ----
        if (!m_System.m_SelectionMaskSkinnedVertSpv.empty() && !m_System.m_SelectionMaskFragSpv.empty())
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

            m_System.m_SelectionMaskSkinnedPipeline = std::make_unique<VKPipeline>(
                maskSkinnedConfig, m_System.m_SelectionMaskSkinnedVertSpv, m_System.m_SelectionMaskFragSpv, layouts);
        }

        // ---- Skybox pipeline ----
        if (!m_System.m_SkyboxVertSpv.empty() && !m_System.m_SkyboxFragSpv.empty())
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

            m_System.m_SkyboxPipeline = std::make_unique<VKPipeline>(skyboxConfig, m_System.m_SkyboxVertSpv, m_System.m_SkyboxFragSpv, layouts);
        }

        // ---- Post-process pipelines ----
        if (!m_System.m_FullscreenVertSpv.empty() && m_System.m_PPDescSetLayout != VK_NULL_HANDLE)
        {
            std::vector<VkDescriptorSetLayout> ppLayouts = { m_System.m_PPDescSetLayout };

            // Bloom extract: push constant = float threshold + pad
            if (!m_System.m_BloomExtractFragSpv.empty())
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
                m_System.m_BloomExtractPipeline = std::make_unique<VKPipeline>(
                    bloomExtractConfig, m_System.m_FullscreenVertSpv, m_System.m_BloomExtractFragSpv, ppLayouts);
            }

            // Bloom blur: push constant = vec2 direction + pad
            if (!m_System.m_BloomBlurFragSpv.empty())
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
                m_System.m_BloomBlurPipeline = std::make_unique<VKPipeline>(
                    bloomBlurConfig, m_System.m_FullscreenVertSpv, m_System.m_BloomBlurFragSpv, ppLayouts);
            }

            // PostProcess composite: no push constants, UBO at binding 2
            if (!m_System.m_PostProcessFragSpv.empty())
            {
                PipelineConfig ppConfig;
                ppConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM }; // LDR output
                ppConfig.depthFormat = VK_FORMAT_UNDEFINED;
                ppConfig.depthTest = false; ppConfig.depthWrite = false;
                ppConfig.blendEnabled = false;
                ppConfig.cullMode = VK_CULL_MODE_NONE;
                m_System.m_PostProcessPipeline = std::make_unique<VKPipeline>(
                    ppConfig, m_System.m_FullscreenVertSpv, m_System.m_PostProcessFragSpv, ppLayouts);
            }
        }

        // ---- Outline pipeline ----
        if (!m_System.m_FullscreenVertSpv.empty() && !m_System.m_OutlineFragSpv.empty() && m_System.m_OutlineDescSetLayout != VK_NULL_HANDLE)
        {
            std::vector<VkDescriptorSetLayout> outlineLayouts = { m_System.m_OutlineDescSetLayout };

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
            m_System.m_OutlinePipeline = std::make_unique<VKPipeline>(
                outlineConfig, m_System.m_FullscreenVertSpv, m_System.m_OutlineFragSpv, outlineLayouts);
        }

        // ---- Grid pipeline (editor-only infinite grid fullscreen pass) ----
        if (!m_System.m_FullscreenVertSpv.empty() && !m_System.m_GridFragSpv.empty() && m_System.m_GridDescSetLayout != VK_NULL_HANDLE)
        {
            std::vector<VkDescriptorSetLayout> gridLayouts = { m_System.m_GridDescSetLayout };

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
            m_System.m_GridPipeline = std::make_unique<VKPipeline>(
                gridConfig, m_System.m_FullscreenVertSpv, m_System.m_GridFragSpv, gridLayouts);
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
            RenderingSystem::k_MaxGPUObjects * sizeof(GPUObjectData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            m_System.m_ObjectSSBO, m_System.m_ObjectSSBOAlloc, m_System.m_ObjectSSBOMapped);

        // Indirect buffer holds 5 regions (camera + 4 cascades), each with RenderingSystem::k_IndirectRegionStride commands.
        allocBuffer(
            RenderingSystem::k_IndirectRegionCount * RenderingSystem::k_IndirectRegionStride * sizeof(VkDrawIndexedIndirectCommand),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            m_System.m_IndirectBuffer, m_System.m_IndirectBufferAlloc, m_System.m_IndirectBufferMapped);

        // Create Set 5 descriptor pool + set for the ObjectSSBO (graphics pipeline)
        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets       = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_System.m_ObjectSSBODescPool);

        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool     = m_System.m_ObjectSSBODescPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_System.m_ObjectSSBODescLayout;
        vkAllocateDescriptorSets(device, &allocInfo, &m_System.m_ObjectSSBODescSet);

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_System.m_ObjectSSBO;
        bufInfo.offset = 0;
        bufInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = m_System.m_ObjectSSBODescSet;
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
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_System.m_CullDescLayout);

        VulkanContext::Get().GetDescriptorAllocator().Allocate(m_System.m_CullDescLayout, m_System.m_CullDescSet);

        VkDescriptorBufferInfo objInfo{};
        objInfo.buffer = m_System.m_ObjectSSBO;
        objInfo.offset = 0;
        objInfo.range  = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo indInfo{};
        indInfo.buffer = m_System.m_IndirectBuffer;
        indInfo.offset = 0;
        indInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_System.m_CullDescSet;
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
        pcRange.size       = sizeof(glm::vec4) * 6 + sizeof(u32) * 2;

        auto spv = ShaderCompiler::Compile(FileSystem::EngineAssetsPath("shaders/gpu_cull.comp"));
        if (spv.empty())
        {
            LH_CORE_ERROR("RenderingSystem: Failed to compile gpu_cull.comp!");
            return;
        }

        m_System.m_CullPipeline = std::make_unique<VKComputePipeline>(
            spv,
            std::vector<VkDescriptorSetLayout>{ m_System.m_CullDescLayout },
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
        auto shadersPath = FileSystem::EngineAssetsPath("shaders");

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

        m_System.m_GTAOLinearDepth = makeStorage(TextureFormat::R32_Float);
        m_System.m_GTAORawAO       = makeStorage(TextureFormat::R8);
        m_System.m_GTAOEdges       = makeStorage(TextureFormat::R8);
        m_System.m_GTAOFinal       = makeStorage(TextureFormat::R8);

        // ---- Shared linear-clamp sampler for GTAO compute reads ----
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_System.m_GTAOSampler);

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
        vkCreateDescriptorPool(device, &poolCI, nullptr, &m_System.m_GTAODescPool);

        // ---- GTAO UBO (GTAOUBO std140, 48B) ----
        m_System.m_GTAOUBOBuffer = std::make_shared<VKUniformBuffer>(sizeof(GTAOUBO));

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
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_System.m_GTAOPrefilterDescLayout);

            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool     = m_System.m_GTAODescPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_System.m_GTAOPrefilterDescLayout;
            vkAllocateDescriptorSets(device, &allocInfo, &m_System.m_GTAOPrefilterDescSet);

            VkPushConstantRange pcRange{};
            pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcRange.offset     = 0;
            pcRange.size       = sizeof(i32) * 2 + sizeof(float) * 6; // halfResSize + invFullRes + nearZ + farZ + pads

            m_System.m_GTAOPrefilterSpv = ShaderCompiler::Compile(shadersPath / "gtao_depth_prefilter.comp");
            if (m_System.m_GTAOPrefilterSpv.empty())
            {
                LH_CORE_ERROR("RenderingSystem: Failed to compile gtao_depth_prefilter.comp!");
                return;
            }
            m_System.m_GTAOPrefilterPipeline = std::make_unique<VKComputePipeline>(
                m_System.m_GTAOPrefilterSpv,
                std::vector<VkDescriptorSetLayout>{ m_System.m_GTAOPrefilterDescLayout },
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
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_System.m_GTAOMainDescLayout);

            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool     = m_System.m_GTAODescPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_System.m_GTAOMainDescLayout;
            vkAllocateDescriptorSets(device, &allocInfo, &m_System.m_GTAOMainDescSet);

            VkPushConstantRange pcRange{};
            pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcRange.offset     = 0;
            pcRange.size       = sizeof(float) * 4 + sizeof(u32) * 4; // projParams + near/far + frameIndex + pads

            m_System.m_GTAOMainSpv = ShaderCompiler::Compile(shadersPath / "gtao_main.comp");
            if (m_System.m_GTAOMainSpv.empty())
            {
                LH_CORE_ERROR("RenderingSystem: Failed to compile gtao_main.comp!");
                return;
            }
            m_System.m_GTAOMainPipeline = std::make_unique<VKComputePipeline>(
                m_System.m_GTAOMainSpv,
                std::vector<VkDescriptorSetLayout>{ m_System.m_GTAOMainDescLayout },
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
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_System.m_GTAODenoiseDescLayout);

            VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocInfo.descriptorPool     = m_System.m_GTAODescPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &m_System.m_GTAODenoiseDescLayout;
            vkAllocateDescriptorSets(device, &allocInfo, &m_System.m_GTAODenoiseDescSet);

            // No push constants — resolution derived from textureSize() inside the shader.
            m_System.m_GTAODenoiseSpv = ShaderCompiler::Compile(shadersPath / "gtao_denoise.comp");
            if (m_System.m_GTAODenoiseSpv.empty())
            {
                LH_CORE_ERROR("RenderingSystem: Failed to compile gtao_denoise.comp!");
                return;
            }
            m_System.m_GTAODenoisePipeline = std::make_unique<VKComputePipeline>(
                m_System.m_GTAODenoiseSpv,
                std::vector<VkDescriptorSetLayout>{ m_System.m_GTAODenoiseDescLayout },
                std::vector<VkPushConstantRange>{});
        }

        UpdateAODescriptors();
    }

    void RenderPipeline::UpdateAODescriptors()
    {
        if (m_System.m_GTAOPrefilterDescSet == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        auto vkSceneDepth = std::static_pointer_cast<VKTexture>(m_System.m_Targets.GetSceneDepth());
        auto vkLinDepth   = std::static_pointer_cast<VKTexture>(m_System.m_GTAOLinearDepth);
        auto vkRawAO      = std::static_pointer_cast<VKTexture>(m_System.m_GTAORawAO);
        auto vkFinalAO    = std::static_pointer_cast<VKTexture>(m_System.m_GTAOFinal);

        // Shared VkDescriptorImageInfo / buffer-info slots reused across passes.
        VkDescriptorImageInfo  sceneDepthInfo{};
        sceneDepthInfo.sampler     = m_System.m_GTAOSampler;
        sceneDepthInfo.imageView   = vkSceneDepth->GetImageView();
        sceneDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo  linDepthSampledInfo{};
        linDepthSampledInfo.sampler     = m_System.m_GTAOSampler;
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
        uboInfo.buffer = m_System.m_GTAOUBOBuffer ? m_System.m_GTAOUBOBuffer->GetVulkanBuffer() : VK_NULL_HANDLE;
        uboInfo.offset = 0;
        uboInfo.range  = VK_WHOLE_SIZE;

        // ---- Prefilter pass: [sceneDepth (sampler), linDepth (storage)] ----
        VkWriteDescriptorSet preWrites[2]{};
        preWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        preWrites[0].dstSet          = m_System.m_GTAOPrefilterDescSet;
        preWrites[0].dstBinding      = 0;
        preWrites[0].descriptorCount = 1;
        preWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        preWrites[0].pImageInfo      = &sceneDepthInfo;

        preWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        preWrites[1].dstSet          = m_System.m_GTAOPrefilterDescSet;
        preWrites[1].dstBinding      = 1;
        preWrites[1].descriptorCount = 1;
        preWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        preWrites[1].pImageInfo      = &linDepthStorageInfo;

        vkUpdateDescriptorSets(device, 2, preWrites, 0, nullptr);

        // ---- Main pass: [linDepth (sampler), rawAO (storage), UBO] ----
        if (m_System.m_GTAOMainDescSet == VK_NULL_HANDLE || uboInfo.buffer == VK_NULL_HANDLE) return;

        VkWriteDescriptorSet mainWrites[3]{};
        mainWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mainWrites[0].dstSet          = m_System.m_GTAOMainDescSet;
        mainWrites[0].dstBinding      = 0;
        mainWrites[0].descriptorCount = 1;
        mainWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mainWrites[0].pImageInfo      = &linDepthSampledInfo;

        mainWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mainWrites[1].dstSet          = m_System.m_GTAOMainDescSet;
        mainWrites[1].dstBinding      = 1;
        mainWrites[1].descriptorCount = 1;
        mainWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        mainWrites[1].pImageInfo      = &rawAOStorageInfo;

        mainWrites[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mainWrites[2].dstSet          = m_System.m_GTAOMainDescSet;
        mainWrites[2].dstBinding      = 2;
        mainWrites[2].descriptorCount = 1;
        mainWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mainWrites[2].pBufferInfo     = &uboInfo;

        vkUpdateDescriptorSets(device, 3, mainWrites, 0, nullptr);

        // ---- Denoise pass: [rawAO (sampler), linDepth (sampler), finalAO (storage)] ----
        if (m_System.m_GTAODenoiseDescSet == VK_NULL_HANDLE) return;

        VkDescriptorImageInfo rawAOSampledInfo{};
        rawAOSampledInfo.sampler     = m_System.m_GTAOSampler;
        rawAOSampledInfo.imageView   = vkRawAO->GetImageView();
        rawAOSampledInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo finalAOStorageInfo{};
        finalAOStorageInfo.sampler     = VK_NULL_HANDLE;
        finalAOStorageInfo.imageView   = vkFinalAO->GetImageView();
        finalAOStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet denoiseWrites[3]{};
        denoiseWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        denoiseWrites[0].dstSet          = m_System.m_GTAODenoiseDescSet;
        denoiseWrites[0].dstBinding      = 0;
        denoiseWrites[0].descriptorCount = 1;
        denoiseWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        denoiseWrites[0].pImageInfo      = &rawAOSampledInfo;

        denoiseWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        denoiseWrites[1].dstSet          = m_System.m_GTAODenoiseDescSet;
        denoiseWrites[1].dstBinding      = 1;
        denoiseWrites[1].descriptorCount = 1;
        denoiseWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        denoiseWrites[1].pImageInfo      = &linDepthSampledInfo;

        denoiseWrites[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        denoiseWrites[2].dstSet          = m_System.m_GTAODenoiseDescSet;
        denoiseWrites[2].dstBinding      = 2;
        denoiseWrites[2].descriptorCount = 1;
        denoiseWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        denoiseWrites[2].pImageInfo      = &finalAOStorageInfo;

        vkUpdateDescriptorSets(device, 3, denoiseWrites, 0, nullptr);

        // ---- Set 0 GTAO bindings (sampled by pbr.frag) ----
        if (m_System.m_GlobalDescriptorSet == VK_NULL_HANDLE) return;

        VkDescriptorImageInfo gtaoFinalInfo{};
        gtaoFinalInfo.sampler     = m_System.m_GTAOSampler;
        gtaoFinalInfo.imageView   = vkFinalAO->GetImageView();
        gtaoFinalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet globalWrites[2]{};
        globalWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        globalWrites[0].dstSet          = m_System.m_GlobalDescriptorSet;
        globalWrites[0].dstBinding      = 4;
        globalWrites[0].descriptorCount = 1;
        globalWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        globalWrites[0].pImageInfo      = &gtaoFinalInfo;

        globalWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        globalWrites[1].dstSet          = m_System.m_GlobalDescriptorSet;
        globalWrites[1].dstBinding      = 5;
        globalWrites[1].descriptorCount = 1;
        globalWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        globalWrites[1].pBufferInfo     = &uboInfo;

        vkUpdateDescriptorSets(device, 2, globalWrites, 0, nullptr);
    }

    void RenderPipeline::RegisterNamedTextures()
    {
        m_System.m_NamedTextures.clear();
        if (m_System.m_ShadowMap)      m_System.m_NamedTextures["ShadowMap"]      = m_System.m_ShadowMap;
        if (m_System.m_Targets.GetSceneColor())    m_System.m_NamedTextures["SceneColor"]    = m_System.m_Targets.GetSceneColor();
        if (m_System.m_Targets.GetSceneDepth())    m_System.m_NamedTextures["SceneDepth"]    = m_System.m_Targets.GetSceneDepth();
        if (m_System.m_Targets.GetLDROutput())     m_System.m_NamedTextures["LDROutput"]     = m_System.m_Targets.GetLDROutput();
        if (m_System.m_Targets.GetEntityIDBuffer())m_System.m_NamedTextures["EntityID"]     = m_System.m_Targets.GetEntityIDBuffer();
        if (m_System.m_BloomA)        m_System.m_NamedTextures["BloomA"]        = m_System.m_BloomA;
        if (m_System.m_BloomB)        m_System.m_NamedTextures["BloomB"]        = m_System.m_BloomB;
        if (m_System.m_IrradianceMap) m_System.m_NamedTextures["IrradianceMap"] = m_System.m_IrradianceMap;
        if (m_System.m_PrefilteredMap)m_System.m_NamedTextures["PrefilteredMap"]= m_System.m_PrefilteredMap;
        if (m_System.m_BRDFLut)       m_System.m_NamedTextures["BRDF_LUT"]     = m_System.m_BRDFLut;
    }

}
