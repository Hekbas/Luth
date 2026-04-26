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
        auto& registry = scene->Registry();

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

            // Mirror the Vulkan Y-flip applied in UpdateGlobalUniforms so the
            // comparison matches what the GPU actually saw at capture time.
            Mat4 currentProj = m_CameraParams.projection;
            currentProj[1][1] *= -1.0f;
            Mat4 currentViewProj = currentProj * m_CameraParams.view;

            const bool cameraMoved = std::memcmp(&currentViewProj,
                                                  &m_FrameDebugger.capturedFrame.captureViewProj,
                                                  sizeof(Mat4)) != 0;

            if (!cameraMoved)
            {
                // Static — minimal graph: just blit ImGui to the swapchain.
                // The scene LDR retains its last captured image (SHADER_READ
                // from the capture's outline pass).
                //
                // Drop queued views — we can't render them in Frozen, and
                // letting the queue grow unbounded spikes the frame when the
                // debugger exits (all views flush at once).
                m_QueuedViews.clear();
                m_Pipeline->ExecuteMinimal();
                return;
            }

            // Camera moved — re-trigger capture and fall through. BeginCapture
            // (called below before ExecuteGraph) destroys the prior archives.
            m_FrameDebugger.state = DebuggerState::CaptureRequested;
        }

        if (Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan)
            return;

        // RenderSnapshot was captured at end of game stage (App::Run), where
        // material registration + MaterialSystem::Update also ran. Render
        // stage just consumes — no registry walks, no slot mutations.
        // RenderFrame() targets Current() during sync warm-up (frames 0/1) and
        // Previous() during steady state (frame ≥2, runs concurrent with Game N).
        const RenderSnapshot& snapshot = Renderer::GetFrameData()->RenderFrame().Snapshot;
        m_ActiveSnapshot = &snapshot;  // Passes read tags etc. via this pointer.

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

        // One primary cmd buffer for the whole frame. Queued views record
        // first (their LDRs are sampled by the scene view's ImGui pass),
        // then the scene view closes with the ImGui pass + present barrier.
        const u64 frameIndex = Renderer::GetFrameData()->GetFrameIndex();
        void* primaryCmd = Renderer::BeginPrimaryCmd(frameIndex);

        for (const RenderView& v : m_QueuedViews)
            RecordView(v, registry, primaryCmd);
        m_QueuedViews.clear();

        RecordView(sceneView, registry, primaryCmd);

        Renderer::EndPrimaryCmdAndSubmit(primaryCmd, frameIndex);
    }

    // =========================================================================
    // Per-view record
    // =========================================================================

    void RenderingSystem::RecordView(const RenderView& view, entt::registry& registry, void* primaryCmd)
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

        m_Pipeline->Execute(registry, view, primaryCmd);
    }

    // =========================================================================
    // Resize
    // =========================================================================

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        // Guard against unsigned underflow from negative float→u32 casts at startup
        if (m_SceneTargets.IsAllocated() && width > 0 && height > 0 && width <= 16384 && height <= 16384)
        {
            // FrameTargets::Resize replaces every scene texture's shared_ptr;
            // the old VkImageViews go through the deferred-delete queue but
            // any in-flight cmd buffer still binds them. Release the cached
            // ViewResources entry too — its descriptor sets reference the
            // about-to-be-destroyed views, and EnsureViewResources's
            // size-keyed cache otherwise misses pure-content swaps. Drain
            // the GPU so the destroy-pool path runs on a quiescent device.
            // Resize is rare (panel layout change), so the stall is fine.
            Renderer::WaitForGPU();
            m_Pipeline->ReleaseViewResources(m_SceneTargets);

            m_SceneTargets.Resize(width, height);
            m_Pipeline->OnResize(width, height);
        }
    }
}
