#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/subsystems/SvgfDenoiser.h"
#include "luth/renderer/debug/FrameDebuggerContext.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/scene/systems/LightingSystem.h"
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
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"
#include "luth/core/FrameData.h"
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
        , m_Denoise(std::make_unique<SvgfDenoiser>(DenoiserChannel::Di))
        , m_DenoiseGi(std::make_unique<SvgfDenoiser>(DenoiserChannel::Gi))
        , m_DenoiseRefl(std::make_unique<SvgfDenoiser>(DenoiserChannel::Reflections))
    {
    }

    RenderPipeline::~RenderPipeline() = default;

    // ── Lifecycle — Initialize / Shutdown ──

    void RenderPipeline::Initialize(u32 viewportWidth, u32 viewportHeight)
    {
        if (Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan) return;

        m_System.GetSceneTargets().Allocate(viewportWidth, viewportHeight);

        m_Global.Init(*this);

        BoneMatrixBuffer::Init();
        m_EditorOverlays.Init(*this);
        m_DebugDraw.Init(*this);
        m_PostProcess.Init(*this);

        // Lighting owns Set 3 + shadow map + IBL + skybox VB/SPVs. Engine ships no HDR;
        // an empty path triggers IBL::Precompute's silent dummy-cubemap fallback —
        // Editor::OnProjectChanged invokes ReloadSkybox once the project paths are live.
        m_Lighting.Init(*this, FileSystem::HasProject()
            ? FileSystem::ResolveAsset("textures/environment.hdr")
            : fs::path{});

        // Geometry owns Set 5 + cull + PBR + DepthPrepass. Init creates layouts +
        // descriptors + cull pipeline; pipelines that need geoLayouts build below.
        m_Geometry.Init(*this);
        // Transparency's Set 6 layout must exist before its BuildPipelines below appends it
        // to geoLayouts (same Init-before-BuildPipelines invariant as Geometry's Set 5).
        m_Transparency.Init(*this);

        // Shadow / skybox / PBR / DepthPrepass pipelines all need the shared 6-layout vector.
        std::vector<VkDescriptorSetLayout> geoLayouts = {
            m_Global.GetSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
            MaterialSystem::GetDescriptorSetLayout(),
            m_Lighting.GetSetLayout(),
            BoneMatrixBuffer::GetDescriptorSetLayout(),
            m_Geometry.GetSet5Layout()
        };
        m_Lighting.BuildPipelines(geoLayouts);
        m_Geometry.BuildPipelines(geoLayouts);
        m_Transparency.BuildPipelines(geoLayouts);
        m_EditorOverlays.BuildPipelines(geoLayouts);
        m_DebugDraw.BuildPipelines();

        m_GTAO.Init(*this);
        m_Volumetric.Init(*this);
        m_Rt.Init(*this);
        m_Restir.Init(*this);
        m_RestirGi.Init(*this);
        m_PathTrace.Init(*this);
        m_Reflections.Init(*this);
        m_Denoise->Init(*this);
        m_DenoiseGi->Init(*this);
        m_DenoiseRefl->Init(*this);
        m_Skinning.Init(*this);

        // Shader hot-reload callback: pulls fresh SPIR-V into the cached blob
        // and rebuilds pipelines that use it. Fires after ShaderLibrary::Reload
        // has already recompiled and re-reflected the single-stage shader.
        // Library keys are the shader filename (e.g. "pbr.vert", "gtao_main.comp").
        ShaderLibrary::SetReloadCallback([this](const std::string& name) {
            // No vkDeviceWaitIdle: old pipelines are deferred-destroyed via
            // VulkanContext::PushDeletion, which drains MAX_FRAMES_IN_FLIGHT
            // frames later in AcquireImage -- by then the GPU has retired any
            // command buffer that bound them. Keeps shader save under steady frame pacing.
            auto vk = std::static_pointer_cast<VulkanShader>(ShaderLibrary::Get(name));
            if (!vk || !vk->IsValid())
            {
                LH_CORE_ERROR("Shader reload: '{}' invalid — keeping existing pipelines", name);
                return;
            }
            const auto& spv = vk->GetSpirV();

            std::vector<VkDescriptorSetLayout> geoLayouts = {
                m_Global.GetSetLayout(),
                VulkanContext::Get().GetBindlessSet().GetLayout(),
                MaterialSystem::GetDescriptorSetLayout(),
                m_Lighting.GetSetLayout(),
                BoneMatrixBuffer::GetDescriptorSetLayout(),
                m_Geometry.GetSet5Layout()
            };
            // Subsystems handle their own shaders + pipeline rebuilds. Order ensures fullscreen.vert
            // reaches both PostProcess and EditorOverlays (PostProcess returns false for it; EditorOverlays
            // returns true). Debug shaders + IBL precompute remain RP residual.
            // Transparency runs OUTSIDE the || chain (overlays precedent): it must also see
            // pbr.vert / pbr_skinned.vert (handled = true by Geometry) to invalidate its variants.
            const bool transparencyHandled = m_Transparency.OnShaderReloaded(name, spv);
            const bool handled = m_Lighting.OnShaderReloaded(name, spv, geoLayouts)
                              || m_Geometry.OnShaderReloaded(name, spv, geoLayouts)
                              || m_GTAO.OnShaderReloaded(name, spv)
                              || m_Volumetric.OnShaderReloaded(name, spv)
                              || m_Skinning.OnShaderReloaded(name, spv)
                              || m_Rt.OnShaderReloaded(name, spv)
                              || m_Restir.OnShaderReloaded(name, spv)
                              || m_RestirGi.OnShaderReloaded(name, spv)
                              || m_PathTrace.OnShaderReloaded(name, spv)
                              || m_Reflections.OnShaderReloaded(name, spv)
                              || m_Denoise->OnShaderReloaded(name, spv)
                              || m_DenoiseGi->OnShaderReloaded(name, spv)
                              || m_DenoiseRefl->OnShaderReloaded(name, spv);
            // PostProcess returns false for fullscreen.vert so EditorOverlays still gets to rebuild
            // its outline/grid pipelines below.
            const bool ppHandled       = m_PostProcess.OnShaderReloaded(name, spv);
            const bool overlaysHandled = m_EditorOverlays.OnShaderReloaded(name, spv, geoLayouts);
            const bool debugHandled    = m_DebugDraw.OnShaderReloaded(name, spv);
            if (handled || ppHandled || overlaysHandled || debugHandled || transparencyHandled)
            {
                if      (name == "debugBlit.frag")  m_System.GetFrameDebugger().blitFragSpv  = spv;
                else if (name == "debugDepth.frag") m_System.GetFrameDebugger().depthFragSpv = spv;
                LH_CORE_INFO("Pipelines rebuilt after shader reload: {}", name);
                return;
            }

            // Debug-shader-only path (no pipeline rebuild on RP side; FrameDebuggerContext rebuilds lazily).
            if      (name == "debugBlit.frag")  m_System.GetFrameDebugger().blitFragSpv  = spv;
            else if (name == "debugDepth.frag") m_System.GetFrameDebugger().depthFragSpv = spv;
            // IBL precompute shaders refresh in the library; ReloadSkybox() must run to re-bake.
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
        m_System.GetFrameDebugger().Shutdown(device);

        // Subsystems own their layouts/pools/samplers/pipelines.
        m_Transparency.Shutdown();
        m_Skinning.Shutdown();
        m_DenoiseRefl->Shutdown();
        m_DenoiseGi->Shutdown();
        m_Denoise->Shutdown();
        m_Reflections.Shutdown();
        m_PathTrace.Shutdown();
        m_RestirGi.Shutdown();
        m_Restir.Shutdown();
        m_Rt.Shutdown();
        m_DebugDraw.Shutdown();
        m_EditorOverlays.Shutdown();
        m_PostProcess.Shutdown();
        m_Volumetric.Shutdown();
        m_GTAO.Shutdown();
        m_Geometry.Shutdown();
        m_Lighting.Shutdown();
        m_Global.Shutdown();
    }

    void RenderPipeline::OnResize(u32 width, u32 height)
    {
        // Scene-panel resize. FrameTargets is already resized by
        // RenderingSystem::Resize; EnsureViewResources picks up the size
        // change and rebuilds textures + descriptors.
        EnsureViewResources(m_System.GetSceneTargets());
        RegisterNamedTextures();
    }

    void RenderPipeline::PrepareForTargets(FrameTargets& targets)
    {
        m_CurrentViewResources = &EnsureViewResources(targets);
    }

    void RenderPipeline::ExecuteMinimal()
    {
        auto& s = m_System;
        RG::RenderGraph rg(m_System.GetFrameAllocator());
        AddImGuiPass(rg, RG::ResourceHandle{}); // invalid → ImGuiPass skips the optional Read
        rg.Compile();
        Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex(), nullptr);
    }

    bool RenderPipeline::Execute(const RenderView& view, QueueRecorders recorders)
    {
        VkCommandBuffer primaryCmd = recorders.gA;
        auto& s = m_System;
        m_CurrentView          = &view;
        m_CurrentViewResources = view.targets ? &EnsureViewResources(*view.targets) : nullptr;

        RG::RenderGraph rg(m_System.GetFrameAllocator());

        // Import this frame's tagged-heap regions for RG barrier tracking. Buffers can
        // change identity each frame (heap allocator may reuse pages or grow backings).
        const auto& objectRegion   = m_Geometry.GetObjectRegion();
        const auto& indirectRegion = m_Geometry.GetIndirectRegion();
        RG::BufferDesc objDesc { "ObjectSSBO",     objectRegion.size,   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        RG::BufferDesc indDesc { "IndirectBuffer", indirectRegion.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT };
        RG::BufferHandle hObjectBuf   = rg.ImportBuffer(objDesc, (void*)objectRegion.buffer,   RG::ResourceState::Undefined);
        RG::BufferHandle hIndirectBuf = rg.ImportBuffer(indDesc, (void*)indirectRegion.buffer, RG::ResourceState::Undefined);

        // Frustum cull — 5 dispatches per view (camera + 4 cascades). Each view owns a
        // disjoint range within the indirect region.
        {
            const u32 baseRegion = view.viewIndex * k_IndirectRegionsPerView;
            Frustum camFrustum = CreateFrustumFromCamera(m_Global.GetCachedViewProj());
            m_Geometry.AddCullPass(rg, hObjectBuf, hIndirectBuf, camFrustum.planes, baseRegion * k_IndirectRegionStride, "FrustumCull.Cam");

            // CSM cascade cull — needed when ShadowPass runs (CSM mode OR volumetric on, since
            // volumetric_inject_scatter samples cascades in both shadow modes).
            const bool runCsmCascades = m_Global.GetShadowParams().castShadows
                                     && ((m_Global.GetShadowParams().mode == ShadowingMode::RasterCSM)
                                         || view.camera.enableVolumetricFog);
            if (runCsmCascades)
            {
                for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                {
                    Frustum cascadeFrustum = CreateFrustumFromCamera(m_Global.GetCascades().lightSpaceMatrix[i]);
                    const u32 destOffset = (baseRegion + 1 + i) * k_IndirectRegionStride;
                    const std::string name = "FrustumCull.C" + std::to_string(i);
                    m_Geometry.AddCullPass(rg, hObjectBuf, hIndirectBuf, cascadeFrustum.planes, destOffset, name.c_str());
                }
            }
        }

        // Shadow pass renders cascade depth — needed for CSM mode AND for volumetric god-rays
        // in either shadow mode (volumetric scatter samples shadowMap at Set 1 b5).
        const bool runCsmShadowPasses = m_Global.GetShadowParams().castShadows
                                     && ((m_Global.GetShadowParams().mode == ShadowingMode::RasterCSM)
                                         || view.camera.enableVolumetricFog);
        RG::ResourceHandle shadowHandles[k_ShadowCascadeCount]{};
        if (runCsmShadowPasses)
        {
            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                shadowHandles[i] = m_Lighting.AddShadowPass(rg, hIndirectBuf, i);
        }

        // Z-prepass produces SceneDepth before forward shading. The render
        // graph can schedule it in parallel with the shadow cascades.
        RG::ResourceHandle prepassDepth = m_Geometry.AddDepthPrepass(rg, hIndirectBuf);

        // Slim G-buffer — opaque normal/roughness/motion/matID. Reads prepass depth
        // with EQUAL test; feeds TAA + downstream RT denoise + RT reflections.
        // Live ShadeMode toggle consumes slimGB downstream (in the AddSlimVizPass call below).
        SlimGBufferOutput slimGB = m_Geometry.AddSlimGBufferPass(rg, hIndirectBuf, prepassDepth);

        // Forward+ cluster AABB builder + light-to-cluster assignment. Both async-compute; the
        // assign pass consumes the build pass's AABB + grid handles directly (no re-import — see
        // arch hazard 1). UploadLightSSBO must run BEFORE AddLightAssignPass so the assign pass
        // can bind the same VkBuffer to its b0 read; WriteSet3PerView lands afterwards once all
        // three per-view tagged-heap regions are known.
        Memory::GPUSubRegion lightSSBORegion{};
        if (auto* lighting = SystemRegistry::GetSystem<LightingSystem>())
            lightSSBORegion = m_Lighting.UploadLightSSBO(lighting->GetLights());
        LightingSubsystem::ClusterBuildOutputs clusters = m_Lighting.AddClusterBuildPass(rg);
        LightingSubsystem::LightAssignOutputs  assign   = m_Lighting.AddLightAssignPass(rg, clusters);
        m_Lighting.WriteSet3PerView(lightSSBORegion, clusters.gridRegion, assign.indexRegion);

        // RT acceleration structures — per-frame skinning + skinned BLAS refit + TLAS build, on
        // AsyncCompute. Built BEFORE the volumetric chain so the inject-scatter pass's RT fog-shadow
        // rayQuery reads a BUILT TLAS — passes execute in registration order on the shared compute
        // primary; the inline AS barrier gives memory visibility, not execution ordering. Multi-view
        // guard inside RtSubsystem short-circuits the second view (TLAS is scene-global).
        const bool runRtShadows = (m_Global.GetShadowParams().mode == ShadowingMode::RtShadows)
                               && m_Global.GetShadowParams().castShadows;
        // Per-view fog toggle — also gates the volumetric term in needTlas, so a fog-off view doesn't
        // build a TLAS the (then-unregistered) scatter pass would never read.
        const bool volumetricEnabled = view.camera.enableVolumetricFog;
        // Build the TLAS whenever ANY RT consumer needs it — RT shadows / ReSTIR DI/GI / PathTrace /
        // reflections / volumetric RT fog shadows. The RT sun-shadow trace below stays runRtShadows-only.
        const bool needTlas = runRtShadows || m_Restir.IsEnabled() || m_RestirGi.IsEnabled()
                            || m_PathTrace.IsEnabled() || m_Reflections.IsEnabled()
                            || (volumetricEnabled && m_Volumetric.IsRtShadowsEnabled());
        if (needTlas)
            m_Rt.AddTlasBuildPass(rg);

        // Volumetric chain — gated by per-view editor toggle. When off the inject + integrate +
        // composite passes skip entirely; sceneColor flows through unchanged. injectOut hoisted
        // to outer scope so the debug viz pass below can reference the density atlas handle.
        VolumetricSubsystem::InjectOutputs injectOut{};
        RG::ResourceHandle volInScatterHandle{};  // post-integrate scratch (viz mode 1 samples this)
        RG::ResourceHandle volResolvedHandle{};   // post-resolve (composite + viz sample)
        if (volumetricEnabled && m_CurrentViewResources)
        {
            const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
            Memory::GPUSubRegion fogVolumeRegion{};
            if (auto* lighting = SystemRegistry::GetSystem<LightingSystem>())
                fogVolumeRegion = m_Volumetric.UploadFogVolumeSSBO(lighting->GetFogVolumes());
            // Density pass binds FogVolume SSBO; scatter pass binds Light/ClusterGrid/LightIndex.
            // Resolve/composite/viz b1-or-b2 parity-rewrite to ping-pong HistA/B for temporal
            // accumulation. Cycled slots keep rewrites disjoint from in-flight prior frame reads.
            m_Volumetric.WriteInjectDensityPerFrame(fogVolumeRegion);
            m_Volumetric.WriteInjectScatterPerFrame(lightSSBORegion, clusters.gridRegion, assign.indexRegion);
            m_Volumetric.WriteResolvePerFrame(*m_CurrentViewResources, frameAbs);
            m_Volumetric.WriteCompositePerFrame(*m_CurrentViewResources, *view.targets, frameAbs);
            m_Volumetric.WriteVizPerFrame(*m_CurrentViewResources, frameAbs);
            // Density pass writes volDensity; scatter pass reads it (via shared ResourceNode so RG
            // inserts the barrier) and samples it along the sun ray for proper density-aware
            // absorption. Scatter samples shadow cascades via descriptor binding 5 — per-cascade
            // RG Reads emit the DSA → SHADER_READ_ONLY transitions. Atlas handles chain through
            // integrate + resolve so RG transitions are coherent end-to-end.
            injectOut.density   = m_Volumetric.AddInjectDensityPass(rg);
            injectOut.inScatter = m_Volumetric.AddInjectScatterPass(rg, injectOut.density, shadowHandles);
            volInScatterHandle  = m_Volumetric.AddIntegratePass(rg, injectOut);
            volResolvedHandle   = m_Volumetric.AddResolvePass(rg, volInScatterHandle);
        }

        // Path-traced reference mode (rt-renderer C.5) — a megakernel that bypasses the entire raster +
        // ReSTIR chain. When active, its HDR output (ptColor) feeds the post chain in place of the raster
        // sceneColor; every raster/RT-GI pass below produces handles nothing consumes, so the RG dead-pass
        // culls them. AsyncCompute, after the TLAS build (which the needTlas gate above keeps alive).
        const bool usePathTrace = m_PathTrace.IsEnabled() && m_CurrentViewResources;
        RG::ResourceHandle ptColorHandle{};
        if (usePathTrace)
            ptColorHandle = m_PathTrace.AddPasses(rg);
        const bool ptActive = usePathTrace && ptColorHandle.IsValid();

        // RT sun-shadow trace — per-view (each view's depth/camera/mask differ), so this runs on
        // every view's RG. Writes per-view R8 mask, consumed by GeometryPass via Read(handle).
        // AsyncCompute pass overlaps with GTAO chain below. Gated on RT mode + CastShadows;
        // CSM mode (or CastShadows=false) returns invalid handle and GeometryPass skips the Read.
        // Threads prepassDepth + slimGB.normal so RG transitions them from DSA/COLOR_ATTACHMENT to
        // SHADER_READ_ONLY_OPTIMAL ahead of the raygen sample (descriptor declared that layout).
        RG::ResourceHandle rtShadowMaskHandle{};
        if (runRtShadows)
            rtShadowMaskHandle = m_Rt.AddRtSunShadowsPass(rg, prepassDepth, slimGB.normal);

        // ReSTIR DI — shadowed direct lighting for point lights via per-pixel reservoir RIS + one
        // visibility ray, then a demodulated-irradiance shade. AsyncCompute; reads prepass depth +
        // slim normal, traces the same TLAS the sun-shadow pass uses. Returns an invalid handle when
        // disabled or before the TLAS exists — GeometryPass then skips the Read and pbr.frag's point
        // loop runs instead (the restirParams.x flag gates the consumption).
        RG::ResourceHandle restirDIHandle = m_Restir.AddPasses(rg, prepassDepth, slimGB.normal, slimGB.motion);

        // Denoise the demodulated DI (SVGF; swappable to NRD/RELAX). Transparent filter — consumes the
        // ReSTIR DI handle, returns the denoised handle GeometryPass reads + Set 3 b5 binds. Invalid in
        // (ReSTIR off / pre-TLAS) → invalid out, and pbr.frag falls back to its own cluster light loop.
        RG::ResourceHandle denoisedDIHandle = m_Denoise->AddPasses(rg, DenoiseInputs{
            restirDIHandle, prepassDepth, slimGB.normal, slimGB.motion,
            slimGB.roughness, slimGB.materialID, {}, {} });

        // ReSTIR GI — 1-bounce indirect diffuse via per-pixel reservoir resampling. Returns the
        // demodulated GI image; restirParams.y gates the remodulation in pbr.frag. Invalid when
        // disabled / no TLAS.
        RG::ResourceHandle giDIHandle = m_RestirGi.AddPasses(rg, prepassDepth, slimGB.normal, slimGB.motion);

        // Denoise the demodulated GI (second SVGF instance, DenoiserChannel::Gi). Same transparent-
        // filter contract as DI: consumes the GI handle, returns the denoised handle GeometryPass reads
        // + Set 3 b6 binds. Invalid in → invalid out (pbr.frag then adds nothing under the .y gate).
        RG::ResourceHandle denoisedGiHandle = m_DenoiseGi->AddPasses(rg, DenoiseInputs{
            giDIHandle, prepassDepth, slimGB.normal, slimGB.motion,
            slimGB.roughness, slimGB.materialID, {}, {} });

        // RT specular reflections (rt-renderer D.1) — one GGX-VNDF ray/pixel from the slim G-buffer, then
        // a dedicated specular SVGF (3rd instance, DenoiserChannel::Reflections). The DenoiseInputs.motion
        // slot carries slim ROUGHNESS (the spec reproject's b3 — it computes the reflection's motion
        // internally via hit-distance virtual reprojection; hitDist rides reflRadiance's alpha).
        // denoisedReflHandle feeds GeometryPass (the pbr.frag Set 3 b7 composite lands in S4). AsyncCompute,
        // after the TLAS build (needTlas gate includes Reflections).
        RG::ResourceHandle reflHandle = m_Reflections.AddPasses(rg, prepassDepth, slimGB.normal, slimGB.roughness);
        RG::ResourceHandle denoisedReflHandle = m_DenoiseRefl->AddPasses(rg, DenoiseInputs{
            reflHandle, prepassDepth, slimGB.normal, slimGB.roughness,
            slimGB.roughness, slimGB.materialID, {}, {} });

        // GTAO chain runs every frame so the Set 0 binding-4 sampler sees
        // a valid SHADER_READ_ONLY layout (the `gtao.enabled` flag in the
        // UBO is what disables the modulation inside pbr.frag). ~0.3-1 ms
        // on a mid-range GPU at 1080p; can be gated later if a cheaper
        // bypass path is worth the complexity.
        RG::ResourceHandle gtaoLinearDepth = m_GTAO.AddPrefilterPass(rg, prepassDepth);
        RG::ResourceHandle gtaoRawAO       = m_GTAO.AddMainPass(rg, gtaoLinearDepth);
        RG::ResourceHandle gtaoFinalAO     = m_GTAO.AddDenoisePass(rg, gtaoRawAO, gtaoLinearDepth);

        auto geoOutput                 = m_Geometry.AddGeometryPass(rg, shadowHandles, hIndirectBuf, prepassDepth, gtaoFinalAO, rtShadowMaskHandle, denoisedDIHandle, denoisedGiHandle, denoisedReflHandle);
        SelectionMaskOutput maskOutput = view.drawSelectionOutline
                                         ? m_EditorOverlays.AddSelectionMaskPass(rg)
                                         : SelectionMaskOutput{};
        RG::ResourceHandle skyboxColor = m_Lighting.AddSkyboxPass(rg, geoOutput.color, geoOutput.depth);
        // Volumetric composite — blends fog into sceneColor (alpha-blend equation) BEFORE bloom so
        // bright in-scattered fog can bloom and the grid pass overlays unfogged grid lines.
        // Skipped when the editor toggle is off — downstream uses skyboxColor unchanged.
        RG::ResourceHandle fogColor    = (volumetricEnabled && m_CurrentViewResources)
                                         ? m_Volumetric.AddCompositePass(rg, skyboxColor, prepassDepth, volResolvedHandle)
                                         : skyboxColor;
        // Transparent tier — after the fog composite so glass blends over the fogged background
        // (its own fog is per-fragment at the glass depth, sampled from the resolved atlas inside
        // pbr_transparent.frag). Skipped in PT mode: the raster chain is dead-pass-culled there.
        RG::ResourceHandle transparentColor = fogColor;
        if (!ptActive && m_CurrentViewResources)
        {
            const u32 frameAbsT = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
            m_Transparency.WritePerFrame(*m_CurrentViewResources, frameAbsT);
            transparentColor = m_Transparency.AddPasses(rg, fogColor, geoOutput.entityID, geoOutput.depth,
                volumetricEnabled ? volResolvedHandle : RG::ResourceHandle{}, hIndirectBuf);
        }
        // TAA Resolve — Karis14 YCoCg-clip. Runs AFTER volumetric composite, BEFORE bloom + grid
        // (HDR-domain TAA per Karis recipe). Bloom + grid + composite then consume the resolved
        // color. Per-frame WriteTaaResolvePerFrame rebinds the parity-picked history-prev sampler;
        // the resolve pass writes to the parity-picked history-curr (bound as color attachment).
        const PostProcessSettings& pps = m_System.GetPostProcessSettings();
        // PT does its own progressive AA via accumulation, so TAA is off in PT mode (both ping-pong on
        // frameAbs parity — running them together would cross-contaminate the history).
        const bool taaEnabled = pps.taaEnabled && m_CurrentViewResources && !ptActive;
        if (m_CurrentViewResources)
        {
            const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
            if (taaEnabled)
                m_PostProcess.WriteTaaResolvePerFrame(*m_CurrentViewResources, frameAbs);
            // Per-frame rebind of bloom-extract + composite binding 0 so downstream consumes the actual
            // HDR source: the PT display image when PT is active, else the TAA chain output (taaHistoryCurr)
            // when TAA is on, else SceneColor. Without this the bindings statically reference SceneColor.
            m_PostProcess.UpdateBloomCompositeInput(*m_CurrentViewResources, *view.targets, frameAbs);
        }
        RG::ResourceHandle taaColor    = taaEnabled
                                         ? m_PostProcess.AddTaaResolvePass(rg, transparentColor, slimGB.motion, prepassDepth)
                                         : transparentColor;
        // HDR source for the post chain: the PT megakernel output replaces the raster sceneColor when PT
        // is active (the raster chain above is then dead-pass-culled). Grid is editor-overlay-only → off in PT.
        RG::ResourceHandle hdrForPost  = ptActive ? ptColorHandle : taaColor;
        RG::ResourceHandle bloomResult = m_PostProcess.AddBloomPasses(rg, hdrForPost); // bloom reads PRE-grid color so grid lines don't bloom
        RG::ResourceHandle gridColor   = (view.drawGrid && !ptActive)
                                         ? m_EditorOverlays.AddGridPass(rg, hdrForPost, geoOutput.depth)
                                         : hdrForPost;
        RG::ResourceHandle ldrOutput = m_PostProcess.AddCompositePass(rg, gridColor, bloomResult);

        // Slim G-buffer ShadeMode toggles overwrite LDROutput with a decoded attachment.
        // Mode index = enum offset from ShadeMode::SlimNormal (0..3). Motion scale hardcoded —
        // the frame-debugger panel exposes a slider for per-capture tuning; live viz uses a
        // sensible default matching the existing thumbnail UX. PT mode forces Lit (the debug-viz
        // blits read the culled G-buffer / cluster / reservoir state — meaningless over the PT image).
        const ShadeMode shadeMode = ptActive ? ShadeMode::Lit : m_System.GetShadeMode();
        if (shadeMode >= ShadeMode::SlimNormal && shadeMode <= ShadeMode::SlimMaterialID)
        {
            const u32 slimMode = static_cast<u32>(shadeMode) - static_cast<u32>(ShadeMode::SlimNormal);
            ldrOutput = m_PostProcess.AddSlimVizPass(rg, ldrOutput, slimGB, slimMode, /*motionScale*/20.0f);
        }
        else if (shadeMode == ShadeMode::ClustersDensity)
        {
            ldrOutput = m_Lighting.AddClusterVizPass(rg, ldrOutput, prepassDepth);
        }
        else if ((shadeMode == ShadeMode::VolumetricDensity ||
                  shadeMode == ShadeMode::VolumetricInScatter) &&
                 volumetricEnabled && m_CurrentViewResources)
        {
            const u32 vizMode = (shadeMode == ShadeMode::VolumetricDensity) ? 0u : 1u;
            ldrOutput = m_Volumetric.AddVizPass(rg, ldrOutput, injectOut.density, volResolvedHandle, prepassDepth, vizMode);
        }
        else if (shadeMode == ShadeMode::RestirGiReservoir && m_RestirGi.IsEnabled() && m_CurrentViewResources)
        {
            ldrOutput = m_RestirGi.AddReservoirVizPass(rg, ldrOutput, prepassDepth);
        }

        // Selection outline + debug shapes need the raster G-buffer (entityID mask + scene depth), which
        // PT culls — so both are off in PT mode (the reference is an offline-accumulation view, not interactive).
        RG::ResourceHandle finalOutput = (view.drawSelectionOutline && !ptActive)
                                         ? m_EditorOverlays.AddOutlinePass(rg, ldrOutput, maskOutput, geoOutput.depth)
                                         : ldrOutput;
        if (view.drawDebugShapes && !ptActive)
            finalOutput = m_DebugDraw.AddDebugDrawPass(rg, finalOutput);
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
        // tracked RT after the pass that writes it. Gate on the per-view
        // captureRequested flag (set by the view's owner — RenderingSystem
        // for the scene view, GamePanel for the game view) so the chosen
        // capture source's RG installs the sink, not the editor's by default.
        if (view.captureRequested && m_System.GetFrameDebugger().state == DebuggerState::CaptureRequested)
        {
            // Ensure the debug sampler exists for ImGui archive previews. Idempotent — returns
            // immediately once blitPipeline is already set.
            m_Debugger->InitDebugBlitResources();

            // Invalidate per-draw and depth preview caches. Cache keys are
            // (passIdx, drawIdx) / (archiveIdx, layer+1) which can collide
            // across captures even though the underlying scene state has
            // changed (camera moved → recapture → same indices, new
            // content). Without this reset, re-clicking the same draw or
            // cascade slice after recapture would hit stale cached previews.
            m_Debugger->ResetPreviewCacheKeys();

            m_System.GetFrameDebugger().BeginCapture(VulkanContext::Get().GetDevice(),
                                            VulkanContext::Get().GetAllocator());
            m_System.GetFrameDebugger().RegisterTrackedRT("SceneColor");
            m_System.GetFrameDebugger().RegisterTrackedRT("SceneDepth");
            // ShadowPass imports per-cascade resources named "ShadowMap.C<i>" (one per cascade,
            // each a single-layer view onto the shared 4-layer array). Track each variant so the
            // sink archives them — without this, cascade nodes have no primary output and the
            // panel shows "no output preview".
            for (u32 ci = 0; ci < k_ShadowCascadeCount; ++ci)
                m_System.GetFrameDebugger().RegisterTrackedRT("ShadowMap.C" + std::to_string(ci));
            m_System.GetFrameDebugger().RegisterTrackedRT("LDROutput");
            m_System.GetFrameDebugger().RegisterTrackedRT("EntityID");
            m_System.GetFrameDebugger().RegisterTrackedRT("BloomAFinal");
            m_System.GetFrameDebugger().RegisterTrackedRT("GTAOLinearDepth");
            m_System.GetFrameDebugger().RegisterTrackedRT("GTAORawAO");
            m_System.GetFrameDebugger().RegisterTrackedRT("GTAOFinal");
            // Slim G-buffer attachments. Archive sink copies all 4 after the pass.
            m_System.GetFrameDebugger().RegisterTrackedRT("SlimNormal");
            m_System.GetFrameDebugger().RegisterTrackedRT("SlimRoughness");
            m_System.GetFrameDebugger().RegisterTrackedRT("SlimMotion");
            m_System.GetFrameDebugger().RegisterTrackedRT("SlimMaterialID");
            rg.SetArchiveSink(&m_System.GetFrameDebugger());
        }

        // Only the capturing view needs serial Phase-1 dispatch — its
        // lambdas push into shared FrameDebugger metadata vectors. Non-
        // capturing views' pushes are suppressed below, so they record
        // in parallel exactly as in non-capture frames.
        if (view.captureRequested && m_System.GetFrameDebugger().state == DebuggerState::CaptureRequested)
            rg.SetSerialize(true);

        // Mask state to Inactive around non-capturing views' RG execute so
        // their lambdas' BeginCapturePass / CaptureXX early-return — no
        // pushes, no race, no need for SetSerialize.
        const DebuggerState savedDbgState = m_System.GetFrameDebugger().state;
        const bool suppressDebuggerMetadata = !view.captureRequested
                                              && savedDbgState == DebuggerState::CaptureRequested;
        if (suppressDebuggerMetadata)
            m_System.GetFrameDebugger().state = DebuggerState::Inactive;

        const bool hasComputeWork = Renderer::RecordGraph(recorders, rg, &m_GPUTimers);

        if (suppressDebuggerMetadata)
            m_System.GetFrameDebugger().state = savedDbgState;

        // Non-primary views: transition LDR → SHADER_READ so the scene view's ImGui pass can sample it (scene
        // view's RG already does this via ImGuiPass's builder.Read(sceneColor)).
        // Recorded into recorders.gB because the LDR is written by PBR / post-process in the gB segment — gA runs
        // first on the GPU timeline, so recording the transition there would precede the write and the next-frame
        // PBR would see the image in SHADER_READ_ONLY_OPTIMAL instead of the expected COLOR_ATTACHMENT_OPTIMAL.
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

            vkCmdPipelineBarrier(recorders.gB,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        // Finalize capture (only the source view — matches the sink gate above).
        if (view.captureRequested && m_System.GetFrameDebugger().state == DebuggerState::CaptureRequested)
        {
            // Per-draw replay re-derives inputs from CapturedDrawCall + frozen indirect/object SSBOs.

            // Copy resource and timing info from the graph snapshot
            m_System.GetFrameDebugger().capturedFrame.resources      = m_GraphSnapshot.resources;
            m_System.GetFrameDebugger().capturedFrame.totalGpuTimeMs = m_GraphSnapshot.totalGpuTimeMs;

            // Copy per-pass GPU times into captured passes
            {
                u32 capturedIdx = 0;
                for (auto& ps : m_GraphSnapshot.passes)
                {
                    if (ps.culled) continue;
                    if (capturedIdx < m_System.GetFrameDebugger().capturedFrame.passes.size())
                        m_System.GetFrameDebugger().capturedFrame.passes[capturedIdx].gpuTimeMs = ps.gpuTimeMs;
                    capturedIdx++;
                }
            }

            // Snapshot capture-time camera viewProj for the Frozen-state auto-recapture comparison (see top of Update).
            m_System.GetFrameDebugger().FinalizeCapture(m_Global.GetCachedViewProj());

            // Stamp CSM state into the captured frame so the cascade detail panel always shows
            // GPU-true values from the moment of capture, even if the user later twiddles light
            // settings on the live editor side.
            auto& cf = m_System.GetFrameDebugger().capturedFrame;
            cf.cascadeSplitsViewZ = m_Global.GetCascades().splitsViewZ;
            cf.shadowBias         = m_Global.GetShadowParams().shadowBias;
            cf.shadowNormalBias   = m_Global.GetShadowParams().shadowNormalBias;
            cf.cascadeTexelSize   = m_Global.GetCascades().texelSize;
            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                cf.lightSpaceMatrix[i] = m_Global.GetCascades().lightSpaceMatrix[i];

            // Snapshot captured-view metadata + Set 0 binding sources for replay.
            // invariant: replay reads these instead of m_CurrentViewResources / live IBL
            // textures, since the live state reflects whichever view ran last and IBL
            // can change mid-Freeze.
            cf.capturedView.targets         = view.targets;
            cf.capturedView.viewResourcesId = m_CurrentViewResources ? m_CurrentViewResources->id : 0;
            cf.capturedView.viewIndex       = view.viewIndex;
            if (view.targets && view.targets->GetSceneColor())
            {
                cf.capturedView.width  = view.targets->GetSceneColor()->GetWidth();
                cf.capturedView.height = view.targets->GetSceneColor()->GetHeight();
            }
            m_Global.GetLastUboBytes(cf.capturedGlobalUboBytes);
            cf.capturedIrradiance     = m_Lighting.GetIrradianceMap();
            cf.capturedPrefiltered    = m_Lighting.GetPrefilteredMap();
            cf.capturedBRDF           = m_Lighting.GetBRDFLut();
            cf.capturedGTAOFinal      = m_CurrentViewResources ? m_CurrentViewResources->gtaoFinal : nullptr;
            cf.capturedIblIntensity   = view.camera.iblIntensity;
            cf.capturedSkyboxIntensity = view.camera.skyboxIntensity;
            // Resolve descendants once at capture; replay reads this without
            // touching m_CurrentView (stack-allocated, dangles in Frozen).
            {
                std::unordered_set<entt::entity> resolved;
                m_EditorOverlays.CollectSelectedHandles(view.camera.selectedEntities, resolved);
                cf.capturedSelectionHandles.assign(resolved.begin(), resolved.end());
            }

            cf.valid = true;
            // Snapshot which source produced this capture so viewport overlays
            // survive the user toggling requestedSource between captures.
            m_System.GetFrameDebugger().capturedSource = m_System.GetFrameDebugger().requestedSource;
            m_System.GetFrameDebugger().state          = DebuggerState::Frozen;
        }

        return hasComputeWork;
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
        u32 totalDraws = (u32)(m_System.GetDrawList().opaque.size() + m_System.GetDrawList().cutout.size() + m_System.GetDrawList().transparent.size());
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
        sumIndices(m_System.GetDrawList().opaque);
        sumIndices(m_System.GetDrawList().cutout);
        sumIndices(m_System.GetDrawList().transparent);

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
        if (m_Lighting.GetShadowMap())                     m_NamedTextures["ShadowMap"]    = m_Lighting.GetShadowMap();
        if (m_System.GetSceneTargets().GetSceneColor())    m_NamedTextures["SceneColor"]   = m_System.GetSceneTargets().GetSceneColor();
        if (m_System.GetSceneTargets().GetSceneDepth())    m_NamedTextures["SceneDepth"]   = m_System.GetSceneTargets().GetSceneDepth();
        if (m_System.GetSceneTargets().GetLDROutput())     m_NamedTextures["LDROutput"]    = m_System.GetSceneTargets().GetLDROutput();
        if (m_System.GetSceneTargets().GetEntityIDBuffer())m_NamedTextures["EntityID"]     = m_System.GetSceneTargets().GetEntityIDBuffer();
        // Scene-view bloom textures — Frame Debugger is scene-view-only.
        if (auto it = m_ViewResources.find(&m_System.GetSceneTargets()); it != m_ViewResources.end()) {
            if (it->second.bloomA) m_NamedTextures["BloomA"] = it->second.bloomA;
            if (it->second.bloomB) m_NamedTextures["BloomB"] = it->second.bloomB;
            if (it->second.volDensity)          m_NamedTextures["VolDensity"]           = it->second.volDensity;
            if (it->second.volInScatter)        m_NamedTextures["VolInScatter"]         = it->second.volInScatter;
            if (it->second.volInScatterHistA)   m_NamedTextures["VolInScatterHistA"]   = it->second.volInScatterHistA;
            if (it->second.volInScatterHistB)   m_NamedTextures["VolInScatterHistB"]   = it->second.volInScatterHistB;
            if (it->second.reflRadiance)        m_NamedTextures["Reflections"]         = it->second.reflRadiance;
        }
        if (m_Lighting.GetIrradianceMap())  m_NamedTextures["IrradianceMap"]  = m_Lighting.GetIrradianceMap();
        if (m_Lighting.GetPrefilteredMap()) m_NamedTextures["PrefilteredMap"] = m_Lighting.GetPrefilteredMap();
        if (m_Lighting.GetBRDFLut())        m_NamedTextures["BRDF_LUT"]       = m_Lighting.GetBRDFLut();
        // Slim G-buffer attachments. Empty until SlimGBufferPass writes them.
        if (m_System.GetSceneTargets().GetSlimNormal())     m_NamedTextures["SlimNormal"]     = m_System.GetSceneTargets().GetSlimNormal();
        if (m_System.GetSceneTargets().GetSlimRoughness())  m_NamedTextures["SlimRoughness"]  = m_System.GetSceneTargets().GetSlimRoughness();
        if (m_System.GetSceneTargets().GetSlimMotion())     m_NamedTextures["SlimMotion"]     = m_System.GetSceneTargets().GetSlimMotion();
        if (m_System.GetSceneTargets().GetSlimMaterialID()) m_NamedTextures["SlimMaterialID"] = m_System.GetSceneTargets().GetSlimMaterialID();
    }

    std::shared_ptr<Texture> RenderPipeline::GetNamedTexture(const std::string& name) const
    {
        auto it = m_NamedTextures.find(name);
        return (it != m_NamedTextures.end()) ? it->second : nullptr;
    }

    // ── Frame debugger — forwarders into FrameDebuggerContext ──

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

    void RenderPipeline::BlitArchivedSlimToPreview(u32 archiveIdx, u32 mode, float scale)
    {
        m_Debugger->BlitArchivedSlimToPreview(archiveIdx, mode, scale);
    }

    VkImageView RenderPipeline::GetSlimPreviewView()   const { return m_Debugger->GetSlimPreviewView(); }
    u32         RenderPipeline::GetSlimPreviewWidth()  const { return m_Debugger->GetSlimPreviewWidth(); }
    u32         RenderPipeline::GetSlimPreviewHeight() const { return m_Debugger->GetSlimPreviewHeight(); }

    // ── Public-API forwarders into subsystems (preserve caller compat) ──

    void RenderPipeline::UpdateGlobalUniforms(const CameraParams& camera,
                                              const CascadeData& cascades,
                                              const DirectionalLightShadowParams& shadowParams)
    {
        m_Global.UpdateUBO(camera, cascades, shadowParams);
    }

    void RenderPipeline::BuildGPUObjectBuffer(const RenderSnapshot& snapshot)
    {
        m_Geometry.BuildGPUObjectBuffer(snapshot);
    }

    u32 RenderPipeline::EnsureMaterialRegistered(std::shared_ptr<Material> material)
    {
        return m_Geometry.EnsureMaterialRegistered(material);
    }

    void RenderPipeline::UpdatePostProcessUBO() { m_PostProcess.UpdateUBO(); }
    void RenderPipeline::UpdateGTAOUBO()        { m_GTAO.UpdateUBO(); }

    void RenderPipeline::ReloadSkybox(const fs::path& hdrPath)
    {
        std::vector<VkDescriptorSetLayout> geoLayouts = {
            m_Global.GetSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
            MaterialSystem::GetDescriptorSetLayout(),
            m_Lighting.GetSetLayout(),
            BoneMatrixBuffer::GetDescriptorSetLayout(),
            m_Geometry.GetSet5Layout()
        };
        m_Lighting.ReloadSkybox(hdrPath, geoLayouts);

        // Rewrite Set 0 IBL bindings on every cached view (new textures behind the old views).
        GlobalViewWriteContext ctx{};
        ctx.haveIBL          = m_Lighting.IsIBLReady();
        ctx.iblSampler       = m_Lighting.GetIBLSampler();
        ctx.gtaoSampler      = m_GTAO.GetSampler();
        if (ctx.haveIBL)
        {
            ctx.irradianceView  = std::static_pointer_cast<VKTexture>(m_Lighting.GetIrradianceMap())->GetImageView();
            ctx.prefilteredView = std::static_pointer_cast<VKTexture>(m_Lighting.GetPrefilteredMap())->GetImageView();
            ctx.brdfView        = std::static_pointer_cast<VKTexture>(m_Lighting.GetBRDFLut())->GetImageView();
        }
        for (auto& [targets, vr] : m_ViewResources)
        {
            ctx.gtaoFinalView = vr.gtaoFinal
                ? std::static_pointer_cast<VKTexture>(vr.gtaoFinal)->GetImageView()
                : VK_NULL_HANDLE;
            m_Global.WriteView(vr, ctx);
        }
    }
}
