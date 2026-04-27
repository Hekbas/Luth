#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/debug/FrameDebuggerContext.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
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
#include "luth/core/types/LuthMath.h"
#include "luth/core/time/Time.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/scene/Components.h"
#include "luth/scene/Scene.h"

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
        , m_Debugger(std::make_unique<FrameDebuggerContext>(*this))
    {
    }

    RenderPipeline::~RenderPipeline() = default;

    // =========================================================================
    //  Lifecycle — Initialize / Shutdown
    // =========================================================================

    void RenderPipeline::Initialize(u32 viewportWidth, u32 viewportHeight)
    {
        if (Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan) return;

        auto& s = m_System;
        m_System.m_SceneTargets.Allocate(viewportWidth, viewportHeight);

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

        // Shader hot-reload watcher (engine-shaders dir; project dirs added via
        // RenderingSystem::OnProjectLoaded). Queues background-thread detections
        // for main-thread Poll at the top of Execute.
        m_ShaderWatcher.Start(FileSystem::EngineAssetsPath("shaders"));

        // Capacity covers worst-case current frame (5 cull + 4 shadow cascades +
        // geometry + selection + skybox + 3 bloom + grid + post-process + outline
        // + ImGui ≈ 19 passes) with headroom for future passes (GTAO etc.).
        m_GPUTimers.Init(64);
        RegisterNamedTextures();
    }

    void RenderPipeline::Shutdown()
    {
        auto& s = m_System;

        m_ShaderWatcher.Stop();
        ShaderLibrary::SetReloadCallback(nullptr);
        m_GPUTimers.Shutdown();

        BoneMatrixBuffer::Shutdown();

        VkDevice device = VulkanContext::Get().GetDevice();

        // Release per-view state before the shared layouts it references.
        for (auto& [targets, vr] : m_ViewResources)
            DestroyViewResources(vr);
        m_ViewResources.clear();

        m_Debugger->Shutdown();
        m_System.m_FrameDebugger.Shutdown(device);

        if (m_OutlineSampler)       vkDestroySampler(device, m_OutlineSampler, nullptr);
        if (m_OutlineDescSetLayout) vkDestroyDescriptorSetLayout(device, m_OutlineDescSetLayout, nullptr);
        if (m_GridDepthSampler)     vkDestroySampler(device, m_GridDepthSampler, nullptr);
        if (m_GridDescSetLayout)    vkDestroyDescriptorSetLayout(device, m_GridDescSetLayout, nullptr);
        if (m_PPSampler)            vkDestroySampler(device, m_PPSampler, nullptr);
        if (m_PPDescSetLayout)      vkDestroyDescriptorSetLayout(device, m_PPDescSetLayout, nullptr);
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

        // GTAO resources (epic #58) — shared.
        m_GTAOPrefilterPipeline.reset();
        m_GTAOMainPipeline.reset();
        m_GTAODenoisePipeline.reset();
        if (m_GTAOSampler)             vkDestroySampler(device, m_GTAOSampler, nullptr);
        if (m_GTAOPrefilterDescLayout) vkDestroyDescriptorSetLayout(device, m_GTAOPrefilterDescLayout, nullptr);
        if (m_GTAOMainDescLayout)      vkDestroyDescriptorSetLayout(device, m_GTAOMainDescLayout, nullptr);
        if (m_GTAODenoiseDescLayout)   vkDestroyDescriptorSetLayout(device, m_GTAODenoiseDescLayout, nullptr);
    }

    void RenderPipeline::OnResize(u32 width, u32 height)
    {
        // Scene-panel resize. FrameTargets is already resized by
        // RenderingSystem::Resize; EnsureViewResources picks up the size
        // change and rebuilds textures + descriptors.
        EnsureViewResources(m_System.m_SceneTargets);
        RegisterNamedTextures();
    }

    void RenderPipeline::PrepareForTargets(FrameTargets& targets)
    {
        m_CurrentViewResources = &EnsureViewResources(targets);
    }

    void RenderPipeline::ExecuteMinimal()
    {
        auto& s = m_System;
        RG::RenderGraph rg(*m_System.m_FrameAllocator);
        AddImGuiPass(rg, RG::ResourceHandle{}); // invalid → ImGuiPass skips the optional Read
        rg.Compile();
        Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex(), nullptr);
    }

    void RenderPipeline::Execute(const RenderView& view, void* primaryCmd)
    {
        auto& s = m_System;
        m_CurrentView          = &view;
        m_CurrentViewResources = view.targets ? &EnsureViewResources(*view.targets) : nullptr;

        // Drain pending shader reloads (queued by FileWatcher on bg thread)
        // before the next graph is assembled so pipelines rebuilt via the
        // ShaderLibrary::Reload callback are ready for this frame.
        m_ShaderWatcher.Poll();

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

        // Frustum cull — 5 dispatches per view (camera + 4 cascades).
        // Each view owns a disjoint range in the shared indirect buffer.
        {
            const u32 baseRegion = view.viewIndex * k_IndirectRegionsPerView;
            Frustum camFrustum = CreateFrustumFromCamera(m_CachedViewProj);
            AddCullComputePass(rg, hObjectBuf, hIndirectBuf,
                m_CullPipeline.get(), m_CullDescSet, camFrustum.planes, m_GPUObjectCount,
                baseRegion * k_IndirectRegionStride, "FrustumCull.Cam", &m_System.m_FrameDebugger);

            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            {
                Frustum cascadeFrustum = CreateFrustumFromCamera(m_FrameCascades.lightSpaceMatrix[i]);
                const u32 destOffset = (baseRegion + 1 + i) * k_IndirectRegionStride;
                const std::string name = "FrustumCull.C" + std::to_string(i);
                AddCullComputePass(rg, hObjectBuf, hIndirectBuf,
                    m_CullPipeline.get(), m_CullDescSet, cascadeFrustum.planes, m_GPUObjectCount,
                    destOffset, name.c_str(), &m_System.m_FrameDebugger);
            }
        }

        RG::ResourceHandle shadowHandles[k_ShadowCascadeCount];
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            shadowHandles[i] = AddShadowPass(rg, hIndirectBuf, i);

        // Z-prepass produces SceneDepth before forward shading. The render
        // graph can schedule it in parallel with the shadow cascades.
        RG::ResourceHandle prepassDepth = AddDepthPrepass(rg, hIndirectBuf);

        // GTAO chain runs every frame so the Set 0 binding-4 sampler sees
        // a valid SHADER_READ_ONLY layout (the `gtao.enabled` flag in the
        // UBO is what disables the modulation inside pbr.frag). ~0.3-1 ms
        // on a mid-range GPU at 1080p; can be gated later if a cheaper
        // bypass path is worth the complexity.
        RG::ResourceHandle gtaoLinearDepth = AddGTAODepthPrefilterPass(rg, prepassDepth);
        RG::ResourceHandle gtaoRawAO       = AddGTAOMainPass(rg, gtaoLinearDepth);
        RG::ResourceHandle gtaoFinalAO     = AddGTAODenoisePass(rg, gtaoRawAO, gtaoLinearDepth);

        auto geoOutput                 = AddGeometryPass(rg, shadowHandles, hIndirectBuf, prepassDepth, gtaoFinalAO);
        SelectionMaskOutput maskOutput = view.drawSelectionOutline
                                         ? AddSelectionMaskPass(rg)
                                         : SelectionMaskOutput{};
        RG::ResourceHandle skyboxColor = AddSkyboxPass(rg, geoOutput.color, geoOutput.depth);
        RG::ResourceHandle bloomResult = AddBloomPasses(rg, skyboxColor); // bloom reads PRE-grid color so grid lines don't bloom
        RG::ResourceHandle gridColor   = view.drawGrid
                                         ? AddGridPass(rg, skyboxColor, geoOutput.depth)
                                         : skyboxColor;
        RG::ResourceHandle ldrOutput   = AddPostProcessPass(rg, gridColor, bloomResult);
        RG::ResourceHandle finalOutput = view.drawSelectionOutline
                                         ? AddOutlinePass(rg, ldrOutput, maskOutput, geoOutput.depth)
                                         : ldrOutput;
        if (view.emitImGuiPass)
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

        // Wire the archive sink for this capture. The sink copies each
        // tracked RT after the pass that writes it. Gate on emitImGuiPass
        // so extra views don't double-register tracked RTs.
        if (view.emitImGuiPass && m_System.m_FrameDebugger.state == DebuggerState::CaptureRequested)
        {
            // Phase 14D — ensure the debug sampler exists for ImGui archive previews.
            // Idempotent: returns immediately once blitPipeline is set.
            m_Debugger->InitDebugBlitResources();

            // Invalidate per-draw and depth preview caches. Cache keys are
            // (passIdx, drawIdx) / (archiveIdx, layer+1) which can collide
            // across captures even though the underlying scene state has
            // changed (camera moved → recapture → same indices, new
            // content). Without this reset, re-clicking the same draw or
            // cascade slice after recapture would hit stale cached previews.
            m_Debugger->ResetPreviewCacheKeys();

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

        // FrameDebugger writes from every view's pass lambdas — serialize
        // every view's RG, not just the one carrying the sink.
        if (m_System.m_FrameDebugger.state == DebuggerState::CaptureRequested)
            rg.SetSerialize(true);

        Renderer::RecordGraph(primaryCmd, rg, &m_GPUTimers);

        // Non-primary views: transition LDR → SHADER_READ so the scene
        // view's ImGui pass can sample it. (The scene view's RG already
        // does this via ImGuiPass's builder.Read(sceneColor).)
        if (!view.emitImGuiPass && view.targets && view.targets->GetLDROutput())
        {
            auto vkLdr = std::static_pointer_cast<VKTexture>(view.targets->GetLDROutput());
            VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = vkLdr->GetImage();
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            vkCmdPipelineBarrier((VkCommandBuffer)primaryCmd,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        // Finalize capture (primary view only — matches the sink gate above).
        if (view.emitImGuiPass && m_System.m_FrameDebugger.state == DebuggerState::CaptureRequested)
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
            m_System.m_FrameDebugger.capturedFrame.cascadeSplitsViewZ = m_FrameCascades.splitsViewZ;
            m_System.m_FrameDebugger.capturedFrame.shadowBias         = m_FrameShadowParams.shadowBias;
            m_System.m_FrameDebugger.capturedFrame.shadowNormalBias   = m_FrameShadowParams.shadowNormalBias;
            m_System.m_FrameDebugger.capturedFrame.cascadeTexelSize   = m_FrameCascades.texelSize;
            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                m_System.m_FrameDebugger.capturedFrame.lightSpaceMatrix[i] = m_FrameCascades.lightSpaceMatrix[i];

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
    void RenderPipeline::RegisterNamedTextures()
    {
        m_NamedTextures.clear();
        if (m_ShadowMap)      m_NamedTextures["ShadowMap"]      = m_ShadowMap;
        if (m_System.m_SceneTargets.GetSceneColor())    m_NamedTextures["SceneColor"]    = m_System.m_SceneTargets.GetSceneColor();
        if (m_System.m_SceneTargets.GetSceneDepth())    m_NamedTextures["SceneDepth"]    = m_System.m_SceneTargets.GetSceneDepth();
        if (m_System.m_SceneTargets.GetLDROutput())     m_NamedTextures["LDROutput"]     = m_System.m_SceneTargets.GetLDROutput();
        if (m_System.m_SceneTargets.GetEntityIDBuffer())m_NamedTextures["EntityID"]     = m_System.m_SceneTargets.GetEntityIDBuffer();
        // Scene-view bloom textures — Frame Debugger is scene-view-only.
        if (auto it = m_ViewResources.find(&m_System.m_SceneTargets); it != m_ViewResources.end()) {
            if (it->second.bloomA) m_NamedTextures["BloomA"] = it->second.bloomA;
            if (it->second.bloomB) m_NamedTextures["BloomB"] = it->second.bloomB;
        }
        if (m_IrradianceMap) m_NamedTextures["IrradianceMap"] = m_IrradianceMap;
        if (m_PrefilteredMap)m_NamedTextures["PrefilteredMap"]= m_PrefilteredMap;
        if (m_BRDFLut)       m_NamedTextures["BRDF_LUT"]     = m_BRDFLut;
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
    //  Frame debugger — forwarders into FrameDebuggerContext
    // =========================================================================

    VkImageView RenderPipeline::GetPerDrawPreviewView()  const { return m_Debugger->GetPerDrawPreviewView(); }
    u64         RenderPipeline::GetPerDrawPreviewKey()   const { return m_Debugger->GetPerDrawPreviewKey(); }
    u32         RenderPipeline::GetPerDrawPreviewWidth() const { return m_Debugger->GetPerDrawPreviewWidth(); }
    u32         RenderPipeline::GetPerDrawPreviewHeight()const { return m_Debugger->GetPerDrawPreviewHeight(); }
    VkImageView RenderPipeline::GetDepthPreviewView()    const { return m_Debugger->GetDepthPreviewView(); }
    u32         RenderPipeline::GetDepthPreviewWidth()   const { return m_Debugger->GetDepthPreviewWidth(); }
    u32         RenderPipeline::GetDepthPreviewHeight()  const { return m_Debugger->GetDepthPreviewHeight(); }
    void        RenderPipeline::ResetPreviewCacheKeys() { m_Debugger->ResetPreviewCacheKeys(); }

    void RenderPipeline::ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx)
    {
        m_Debugger->ReplayPassUpToDraw(passIdx, localDrawIdx);
    }

    void RenderPipeline::BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ)
    {
        m_Debugger->BlitArchivedDepthToPreview(archiveIdx, layer, nearZ, farZ);
    }
}
