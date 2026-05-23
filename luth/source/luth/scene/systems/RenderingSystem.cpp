#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/LightingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/resources/FileSystem.h"
#include "luth/scene/Scene.h"
#include "luth/core/diagnostics/Profiler.h"

namespace Luth
{

    // ── Construction / Destruction ──

    RenderingSystem::RenderingSystem(u32 viewportWidth, u32 viewportHeight)
    {
        m_FrameAllocator = std::make_unique<Memory::LinearAllocator>(1 * Memory::MB);
        m_Pipeline       = std::make_unique<RenderPipeline>(*this);
        m_Pipeline->Initialize(viewportWidth, viewportHeight);
    }

    RenderingSystem::~RenderingSystem()
    {
        m_Pipeline->Shutdown();
    }

    void RenderingSystem::ReloadSkybox(const fs::path& hdrPath)
    {
        m_Pipeline->ReloadSkybox(hdrPath);
    }

    std::shared_ptr<Texture> RenderingSystem::GetNamedTexture(const std::string& name) const
    {
        return m_Pipeline->GetNamedTexture(name);
    }

    void RenderingSystem::ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx)
    {
        m_Pipeline->ReplayPassUpToDraw(passIdx, localDrawIdx);
    }

    void RenderingSystem::BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ)
    {
        m_Pipeline->BlitArchivedDepthToPreview(archiveIdx, layer, nearZ, farZ);
    }

    const RG::RenderGraphSnapshot& RenderingSystem::GetGraphSnapshot() const
    {
        return m_Pipeline->GetGraphSnapshot();
    }

    void RenderingSystem::ExitCapture()
    {
        // Free GPU-owned archives BEFORE clearing the metadata vectors.
        m_FrameDebugger.DestroyArchives();
        m_FrameDebugger.state = DebuggerState::Inactive;
        m_FrameDebugger.capturedFrame.Clear();
        // Drop the per-draw replay cache key so the next capture starts clean —
        // the preview texture itself is reused across captures.
        m_Pipeline->ResetPreviewCacheKeys();
    }

    VkImageView RenderingSystem::GetPerDrawPreviewView()   const { return m_Pipeline->GetPerDrawPreviewView(); }
    u64         RenderingSystem::GetPerDrawPreviewKey()    const { return m_Pipeline->GetPerDrawPreviewKey(); }
    u32         RenderingSystem::GetPerDrawPreviewWidth()  const { return m_Pipeline->GetPerDrawPreviewWidth(); }
    u32         RenderingSystem::GetPerDrawPreviewHeight() const { return m_Pipeline->GetPerDrawPreviewHeight(); }
    VkImageView RenderingSystem::GetDepthPreviewView()     const { return m_Pipeline->GetDepthPreviewView(); }
    u32         RenderingSystem::GetDepthPreviewWidth()    const { return m_Pipeline->GetDepthPreviewWidth(); }
    u32         RenderingSystem::GetDepthPreviewHeight()   const { return m_Pipeline->GetDepthPreviewHeight(); }

    void RenderingSystem::BlitArchivedSlimToPreview(u32 archiveIdx, u32 mode, float scale)
    {
        m_Pipeline->BlitArchivedSlimToPreview(archiveIdx, mode, scale);
    }
    VkImageView RenderingSystem::GetSlimPreviewView()      const { return m_Pipeline->GetSlimPreviewView(); }
    u32         RenderingSystem::GetSlimPreviewWidth()     const { return m_Pipeline->GetSlimPreviewWidth(); }
    u32         RenderingSystem::GetSlimPreviewHeight()    const { return m_Pipeline->GetSlimPreviewHeight(); }

    // ── Project lifecycle ──

    void RenderingSystem::OnProjectLoaded()
    {
        if (!FileSystem::HasProject()) return;
        m_Pipeline->GetShaderWatcher().AddProjectDir(FileSystem::AssetsPath("shaders"));
    }

    void RenderingSystem::OnProjectUnloaded()
    {
        m_Pipeline->GetShaderWatcher().RemoveProjectDir();
    }

    // ── Per-frame dispatcher ──

    void RenderingSystem::Update(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        (void)scene; // Render path reads the snapshot, not the registry.

        m_FrameAllocator->Reset();

        // Drain pending shader reloads once per frame (FileWatcher detections from
        // its bg thread). Pre-shader-reload-async this lived inside RenderPipeline::Execute
        // and ran twice per frame when both Scene + Game viewports were open.
        m_Pipeline->GetShaderWatcher().Poll();

        // ── Frame Debugger: Frozen state ──
        // Strict snapshot model with auto-recapture on camera move.
        //
        // While Frozen, the live render graph is NOT rebuilt or re-executed.
        // The LDR output target retains the LAST CAPTURED image (no other code
        // writes it in this state), so the editor's ScenePanel — which samples
        // it through ImGui — keeps showing the GPU-true captured frame.
        //
        // Each Frozen tick we cheaply recompute the camera viewProj (no GPU
        // upload) and bit-compare against captureViewProj. If different, the
        // user has moved the camera, so we flip the state machine back to
        // CaptureRequested and fall through to the normal capture flow below;
        // FrameDebugger::BeginCapture will tear down the prior archives.
        if (m_FrameDebugger.state == DebuggerState::Frozen)
        {
            if (Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan) return;

            // Auto-recapture-on-camera-move only meaningful for Scene captures
            // — the comparison camera (m_CameraParams = editor) matches the
            // source. For Game captures the captureViewProj came from the
            // game camera, so this comparison would always report "moved"
            // and loop the state machine every frame. Game captures stay
            // Frozen until the user explicitly disables.
            bool cameraMoved = false;
            if (m_FrameDebugger.capturedSource == CaptureSource::Scene)
            {
                // Mirror the Vulkan Y-flip from UpdateGlobalUniforms so the
                // comparison matches the GPU's view at capture time.
                Mat4 currentProj = m_CameraParams.projection;
                currentProj[1][1] *= -1.0f;
                Mat4 currentViewProj = currentProj * m_CameraParams.view;

                // Pack viewProj + IBL intensities into one struct for a single
                // memcmp. Catches user inspector tweaks to Sun/Sky settings
                // mid-Freeze. Cascade splits / shadow bias would need the
                // lighting system to recompute during Frozen — out of scope.
                struct CompareKey
                {
                    Mat4  viewProj;
                    float iblIntensity;
                    float skyboxIntensity;
                    float _pad[2] = { 0.0f, 0.0f };
                };
                CompareKey live{};
                live.viewProj        = currentViewProj;
                live.iblIntensity    = m_CameraParams.iblIntensity;
                live.skyboxIntensity = m_CameraParams.skyboxIntensity;
                CompareKey captured{};
                captured.viewProj        = m_FrameDebugger.capturedFrame.captureViewProj;
                captured.iblIntensity    = m_FrameDebugger.capturedFrame.capturedIblIntensity;
                captured.skyboxIntensity = m_FrameDebugger.capturedFrame.capturedSkyboxIntensity;
                cameraMoved = std::memcmp(&live, &captured, sizeof(CompareKey)) != 0;

                // Throttle to ~10 Hz at 60 fps. Per-recapture GPU work
                // (~10 vkCmdCopyImage + barriers, mostly cascade depth)
                // saturates mid-tier GPUs at frame rate; 6× less keeps
                // the editor smooth without visibly stale overlays.
                static constexpr u64 k_AutoRecaptureMinIntervalFrames = 6;
                if (cameraMoved)
                {
                    const u64 currentFrame = Renderer::GetFrameData()->GetFrameIndex();
                    if (currentFrame - m_FrameDebugger.lastRecaptureFrameIndex
                        < k_AutoRecaptureMinIntervalFrames)
                        cameraMoved = false;
                }
            }

            if (!cameraMoved)
            {
                // Static or throttled — minimal graph: just blit ImGui to the
                // swapchain. Drop queued views — letting the queue grow
                // unbounded spikes the frame when the debugger exits.
                m_QueuedViews.clear();
                m_Pipeline->ExecuteMinimal();
                return;
            }

            // Camera moved — re-trigger capture and fall through.
            m_FrameDebugger.state = DebuggerState::CaptureRequested;
            m_FrameDebugger.lastRecaptureFrameIndex = Renderer::GetFrameData()->GetFrameIndex();
        }

        if (Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan)
            return;

        // Snapshot is captured at end of game stage; render stage only reads.
        const RenderSnapshot& snapshot = Renderer::GetFrameData()->RenderFrame().Snapshot;
        m_ActiveSnapshot = &snapshot;

        // Build GPU object buffer (after materials are registered)
        m_Pipeline->BuildGPUObjectBuffer(snapshot);

        // Partition snapshot mesh rows into opaque/cutout/transparent buckets.
        // Must follow BuildGPUObjectBuffer so gpuObjectIndex/entityIndex
        // reference the freshly populated indirect buffer.
        m_DrawListBuilder.Build(snapshot, m_Pipeline->GetMaterialSlotMap(), m_Pipeline->GetEntityToSSBOIndex(), m_DrawList);

        // LightGatherer + CSM fit. Set 3 is per-view now (cluster grid + light index differ per
        // view), so the LightSSBO upload + Set 3 binding writes happen inside BuildGraph per view.
        if (auto* lighting = SystemRegistry::GetSystem<LightingSystem>())
            lighting->UpdateFor(snapshot, m_CameraParams);

        // Primary view — always rendered, emits the per-frame ImGui pass.
        RenderView sceneView;
        sceneView.targets              = &m_SceneTargets;
        sceneView.camera               = m_CameraParams;
        sceneView.viewIndex            = 0;
        sceneView.drawGrid             = m_GridVisible;
        sceneView.drawSelectionOutline = true;
        sceneView.drawDebugShapes      = true;
        sceneView.emitImGuiPass        = true;
        // Capture-source gate: only the scene view installs the archive sink
        // when the user has chosen Scene as the source. Game capture lives on
        // GamePanel's queued view.
        sceneView.captureRequested     = (m_FrameDebugger.state == DebuggerState::CaptureRequested
                                          && m_FrameDebugger.requestedSource == CaptureSource::Scene);

        // Per-view 3-submit topology: each view gets its own gA / compute / gB primary cmd buffers, submitted with
        // timeline-semaphore waits at boundaries. Queued views record first (their LDRs are sampled by the scene
        // view's ImGui pass), then the scene view closes with the ImGui pass + present barrier. Cross-view ordering
        // for shared resources (m_ShadowMap, IBL maps) is enforced by view K+1's gA submit waiting on view K's gB
        // signal at EARLY_FRAGMENT_TESTS_BIT — same stage relationship as the legacy inline pipeline barrier.
        const u64 frameIndex  = Renderer::GetFrameData()->GetFrameIndex();
        const u32 totalViews  = (u32)m_QueuedViews.size() + 1;  // queued + scene view
        LH_CORE_ASSERT(totalViews <= MAX_VIEWS_PER_FRAME, "view count exceeds MAX_VIEWS_PER_FRAME");
        u32 viewSlot = 0;

        for (const RenderView& v : m_QueuedViews)
        {
            QueueRecorders r = Renderer::BeginPrimaryCmd(frameIndex, viewSlot);
            const bool hasCompute = RecordView(v, r);
            Renderer::EndPrimaryCmdAndSubmit(r, frameIndex, viewSlot, hasCompute, /*isLastView=*/false);
            ++viewSlot;
        }
        m_QueuedViews.clear();

        QueueRecorders r = Renderer::BeginPrimaryCmd(frameIndex, viewSlot);
        const bool hasCompute = RecordView(sceneView, r);
        Renderer::EndPrimaryCmdAndSubmit(r, frameIndex, viewSlot, hasCompute, /*isLastView=*/true);
    }

    // ── Per-view record ──

    bool RenderingSystem::RecordView(const RenderView& view, QueueRecorders recorders)
    {
        LH_PROFILE_FUNCTION();

        if (!view.targets || Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan)
            return false;

        // Cascade fit is camera-dependent so this refits per view
        // (~1 ms GPU with game panel open; frustum-union fit is backlog).
        // m_Lights was already gathered once in Update before this loop;
        // UpdateFor here only needs the cascade rebuild for view.camera.
        // (Re-gathering m_Lights from the same snapshot is idempotent — left as
        // a no-cost guard against future signature drift.)
        auto* lighting = SystemRegistry::GetSystem<LightingSystem>();
        lighting->UpdateFor(Renderer::GetFrameData()->RenderFrame().Snapshot, view.camera);

        // Must precede the per-view UBO writes below — they read
        // m_CurrentViewResources, which PrepareForTargets sets.
        m_Pipeline->PrepareForTargets(*view.targets);

        // Light UBO (Set 3) is hoisted to Update — view-independent, single global Set 3
        // would race across views otherwise.
        m_Pipeline->UpdateGlobalUniforms(view.camera, lighting->GetCascades(), lighting->GetShadowParams());
        m_Pipeline->UpdatePostProcessUBO();
        m_Pipeline->UpdateGTAOUBO();

        return m_Pipeline->Execute(view, recorders);
    }

    // ── Resize ──

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        // Guard against unsigned underflow from negative float→u32 casts at startup
        if (m_SceneTargets.IsAllocated() && width > 0 && height > 0 && width <= 16384 && height <= 16384)
        {
            // Drain GPU + drop ViewResources before swapping textures —
            // see GamePanel::SetOnResize for the same hazard description.
            Renderer::WaitForGPU();
            m_Pipeline->ReleaseViewResources(m_SceneTargets);

            m_SceneTargets.Resize(width, height);
            m_Pipeline->OnResize(width, height);
        }
    }
}
