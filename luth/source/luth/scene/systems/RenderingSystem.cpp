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

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

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
        // Phase 14E — drop the per-draw replay cache key so the next capture
        // starts clean (preview texture itself is reused across captures).
        m_Pipeline->ResetPreviewCacheKeys();
    }

    VkImageView RenderingSystem::GetPerDrawPreviewView()   const { return m_Pipeline->GetPerDrawPreviewView(); }
    u64         RenderingSystem::GetPerDrawPreviewKey()    const { return m_Pipeline->GetPerDrawPreviewKey(); }
    u32         RenderingSystem::GetPerDrawPreviewWidth()  const { return m_Pipeline->GetPerDrawPreviewWidth(); }
    u32         RenderingSystem::GetPerDrawPreviewHeight() const { return m_Pipeline->GetPerDrawPreviewHeight(); }
    VkImageView RenderingSystem::GetDepthPreviewView()     const { return m_Pipeline->GetDepthPreviewView(); }
    u32         RenderingSystem::GetDepthPreviewWidth()    const { return m_Pipeline->GetDepthPreviewWidth(); }
    u32         RenderingSystem::GetDepthPreviewHeight()   const { return m_Pipeline->GetDepthPreviewHeight(); }

    // =========================================================================
    // Project lifecycle
    // =========================================================================

    void RenderingSystem::OnProjectLoaded()
    {
        if (!FileSystem::HasProject()) return;
        m_Pipeline->GetShaderWatcher().AddProjectDir(FileSystem::AssetsPath("shaders"));
    }

    void RenderingSystem::OnProjectUnloaded()
    {
        m_Pipeline->GetShaderWatcher().RemoveProjectDir();
    }

    // =========================================================================
    // Per-frame dispatcher
    // =========================================================================

    void RenderingSystem::Update(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        (void)scene; // Render path reads the snapshot, not the registry.

        m_FrameAllocator->Reset();

        // --- Frame Debugger: Frozen state ---
        // Phase 14C — strict snapshot model with auto-recapture on camera move.
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

            // Auto-recapture-on-camera-move only meaningful for Scene captures:
            // the comparison camera (m_CameraParams = editor) is the same one
            // that produced the capture's viewProj. For Game captures the
            // captured viewProj came from the game camera (lives on GamePanel),
            // so comparing it here would *always* report "moved" and loop the
            // state machine every frame. Game captures stay Frozen until the
            // user explicitly disables. (The resource churn that previously
            // made the Scene auto-recapture path expensive — and froze the
            // editor under continuous camera movement — was eliminated by
            // archive-image reuse in FrameDebugger::OnPassExecuted.)
            bool cameraMoved = false;
            if (m_FrameDebugger.capturedSource == CaptureSource::Scene)
            {
                // Mirror the Vulkan Y-flip applied in UpdateGlobalUniforms so the
                // comparison matches what the GPU actually saw at capture time.
                Mat4 currentProj = m_CameraParams.projection;
                currentProj[1][1] *= -1.0f;
                Mat4 currentViewProj = currentProj * m_CameraParams.view;

                cameraMoved = std::memcmp(&currentViewProj,
                                          &m_FrameDebugger.capturedFrame.captureViewProj,
                                          sizeof(Mat4)) != 0;

                // Throttle auto-recapture to ~10 Hz at 60 fps. Archive image
                // reuse (FrameDebugger::OnPassExecuted) eliminated allocation
                // churn, but each recapture still issues ~10 vkCmdCopyImage
                // + ~40 barriers — saturating mid-tier GPUs at 60 Hz under
                // continuous camera movement (the bandwidth, not the CPU
                // side, is the bottleneck). Stepping at 10 Hz drops GPU work
                // 6× and stays smooth-feeling.
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

        // Primary view — always rendered, emits the per-frame ImGui pass.
        RenderView sceneView;
        sceneView.targets              = &m_SceneTargets;
        sceneView.camera               = m_CameraParams;
        sceneView.viewIndex            = 0;
        sceneView.drawGrid             = m_GridVisible;
        sceneView.drawSelectionOutline = true;
        sceneView.emitImGuiPass        = true;
        // Capture-source gate: only the scene view installs the archive sink
        // when the user has chosen Scene as the source. Game capture lives on
        // GamePanel's queued view.
        sceneView.captureRequested     = (m_FrameDebugger.state == DebuggerState::CaptureRequested
                                          && m_FrameDebugger.requestedSource == CaptureSource::Scene);

        // One primary cmd buffer for the whole frame. Queued views record
        // first (their LDRs are sampled by the scene view's ImGui pass),
        // then the scene view closes with the ImGui pass + present barrier.
        const u64 frameIndex = Renderer::GetFrameData()->GetFrameIndex();
        void* primaryCmd = Renderer::BeginPrimaryCmd(frameIndex);

        for (const RenderView& v : m_QueuedViews)
            RecordView(v, primaryCmd);
        m_QueuedViews.clear();

        RecordView(sceneView, primaryCmd);

        Renderer::EndPrimaryCmdAndSubmit(primaryCmd, frameIndex);
    }

    // =========================================================================
    // Per-view record
    // =========================================================================

    void RenderingSystem::RecordView(const RenderView& view, void* primaryCmd)
    {
        LH_PROFILE_FUNCTION();

        if (!view.targets || Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan)
            return;

        // Cascade fit is camera-dependent so this refits per view
        // (~1 ms GPU with game panel open; frustum-union fit is backlog).
        auto* lighting = SystemRegistry::GetSystem<LightingSystem>();
        lighting->UpdateFor(Renderer::GetFrameData()->RenderFrame().Snapshot, view.camera);

        // Must precede the per-view UBO writes below — they read
        // m_CurrentViewResources, which PrepareForTargets sets.
        m_Pipeline->PrepareForTargets(*view.targets);

        m_Pipeline->UploadLightUBO(lighting->GetLights());
        m_Pipeline->UpdateGlobalUniforms(view.camera, lighting->GetCascades(), lighting->GetShadowParams());
        m_Pipeline->UpdatePostProcessUBO();
        m_Pipeline->UpdateGTAOUBO();

        m_Pipeline->Execute(view, primaryCmd);
    }

    // =========================================================================
    // Resize
    // =========================================================================

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
