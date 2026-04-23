#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/LightingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/FileSystem.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/core/diagnostics/Profiler.h"

namespace Luth
{
    using namespace Component;

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
                // The editor panel reads the LDR output through ImGui::Image; the
                // image's persistent layout (SHADER_READ_ONLY_OPTIMAL after the
                // capture's outline pass) is preserved across frames.
                m_Pipeline->ExecuteMinimal();
                return;
            }

            // Camera moved — re-trigger capture and fall through. BeginCapture
            // (called below before ExecuteGraph) destroys the prior archives.
            m_FrameDebugger.state = DebuggerState::CaptureRequested;
        }

        if (Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan)
            return;

        // Camera-independent per-frame prep. These outputs are shared across
        // every view rendered this frame (scene + game panels).
        //
        // Register materials and hold assets for all visible entities
        auto matView = registry.view<WorldTransform, MeshRenderer>();
        for (auto [entity, wt, mr] : matView.each())
        {
            if (mr.ModelUUID.IsValid())
            {
                auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
                if (model)
                    scene->HoldAsset(mr.ModelUUID, model);
            }
            if (!mr.MaterialUUID.IsValid()) continue;
            auto material = AssetManager::GetAsset<Material>(mr.MaterialUUID);
            if (material)
            {
                scene->HoldAsset(mr.MaterialUUID, material);
                m_Pipeline->EnsureMaterialRegistered(material);
            }
        }

        // Upload dirty materials
        MaterialSystem::Update(VK_NULL_HANDLE);

        // Build GPU object buffer (after materials are registered)
        m_Pipeline->BuildGPUObjectBuffer(registry);

        // Partition entities into opaque/cutout/transparent draw buckets.
        // Must follow BuildGPUObjectBuffer so gpuObjectIndex/entityIndex
        // reference the freshly populated indirect buffer.
        m_DrawListBuilder.Build(registry, m_Pipeline->GetMaterialSlotMap(), m_Pipeline->GetEntityToSSBOIndex(), m_DrawList);

        // Render the scene panel view. The game panel (if visible) calls
        // RenderToView again from its OnRender with its own FrameTargets +
        // a CameraParams built from the first Component::Camera entity.
        RenderView sceneView;
        sceneView.targets              = &m_SceneTargets;
        sceneView.camera               = m_CameraParams;
        sceneView.drawGrid             = m_GridVisible;
        sceneView.drawSelectionOutline = true;
        RenderToView(sceneView, registry);
    }

    // =========================================================================
    // Per-view render
    // =========================================================================

    void RenderingSystem::RenderToView(const RenderView& view, entt::registry& registry)
    {
        LH_PROFILE_FUNCTION();

        if (!view.targets || Renderer::GetBackend()->GetAPI() != RenderBackend::API::Vulkan)
            return;

        // Gather lights + fit CSM cascades on the CPU. Cascade fit is
        // camera-dependent, so refits per view — ~1 ms extra GPU while both
        // panels are visible. A frustum-union fit is a follow-up.
        auto* lighting = SystemRegistry::GetSystem<LightingSystem>();
        lighting->UpdateFor(registry, view.camera);

        // Cache/allocate per-view GPU resources (bloom + GTAO textures, 9
        // descriptor sets pointing at this view's targets). Must precede any
        // per-view UBO write — those read m_CurrentViewResources internally.
        m_Pipeline->PrepareForTargets(*view.targets);

        // Upload per-frame GPU state. LightUBO + PP UBO are shared-content
        // (view-independent) so one upload per frame is enough; they're
        // written inside Execute for the scene view today and are harmless
        // to repeat for additional views.
        m_Pipeline->UploadLightUBO(lighting->GetLights());
        m_Pipeline->UpdateGlobalUniforms(view.camera, lighting->GetCascades(), lighting->GetShadowParams());
        m_Pipeline->UpdatePostProcessUBO();
        m_Pipeline->UpdateGTAOUBO();

        // Build + execute the render graph (graph assembly lives in RenderPipeline).
        m_Pipeline->Execute(registry, view);
    }

    // =========================================================================
    // Resize
    // =========================================================================

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        // Guard against unsigned underflow from negative float→u32 casts at startup
        if (m_SceneTargets.IsAllocated() && width > 0 && height > 0 && width <= 16384 && height <= 16384)
        {
            m_SceneTargets.Resize(width, height);
            m_Pipeline->OnResize(width, height);
        }
    }
}
