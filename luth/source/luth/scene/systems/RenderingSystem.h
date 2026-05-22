#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/scene/Entity.h"
#include "luth/memory/Memory.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/DrawListBuilder.h"
#include "luth/renderer/FrameDebugger.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/draw/DrawList.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/renderer/rendergraph/FrameCapture.h"
#include "luth/renderer/lighting/LightTypes.h"
#include "luth/renderer/settings/PostProcessSettings.h"

#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class Texture;
    struct RenderSnapshot;

    // Per-frame global shader inputs (Set 0 UBO). Layout mirrors GLSL binding.
    struct GlobalUniforms {
        Mat4 viewProjection;
        Mat4 prevViewProjection;  // frame N reads frame N-1's VP (motion vectors + TAA reprojection)
        Mat4 view;
        Mat4 projection;
        Vec3 cameraPos;
        float     time;
        Mat4 lightSpaceMatrix[k_ShadowCascadeCount];
        Vec4 cascadeSplitsViewZ;
        Vec4 shadowBias;
        Vec4 shadowNormalBias;
        Vec4 cascadeTexelSize;
        float     iblIntensity;
        float     skyboxIntensity;
        float     debugVisualizeCascades;
        float     cascadeBlendWidth;
    };

    enum class ShadeMode : u8 {
        Lit = 0, Unlit, Wireframe, Normals, EntityID,
        // Slim G-buffer live viz — bypasses tonemap and blits the selected attachment to LDR.
        // Implemented in PostProcessSubsystem::AddSlimVizPass via slim_viz.frag.
        SlimNormal, SlimRoughness, SlimMotion, SlimMaterialID
    };

    struct GeometryOutput {
        RG::ResourceHandle color;
        RG::ResourceHandle depth;
        RG::ResourceHandle entityID;
    };

    // SlimGBufferPass outputs — written between DepthPrepass and GTAO Prefilter. Consumed by
    // A.5 TAA (motion), Phase B/C RT denoisers (normal + roughness), D RT reflections (normal).
    struct SlimGBufferOutput {
        RG::ResourceHandle normal;     // RG16F — octahedral world-space normal
        RG::ResourceHandle roughness;  // R8    — perceptual roughness
        RG::ResourceHandle motion;     // RG16F — NDC delta (currNDC - prevNDC)
        RG::ResourceHandle materialID; // R16U  — bindless material slot
    };

    struct SelectionMaskOutput {
        RG::ResourceHandle mask;
        RG::ResourceHandle depth;
    };

    // ECS-glue layer for the renderer. Owns frame-level scene inputs
    // (CameraParams, DrawList, FrameTargets) and orchestrates per-frame
    // work by invoking RenderPipeline. Lighting inputs (gatherer, cascade
    // fit, shadow params) live on LightingSystem; RenderingSystem looks
    // it up from SystemRegistry each frame.
    //
    // Graphics resources (pipelines, descriptor sets, samplers, UBOs, SSBOs,
    // indirect/object buffers, IBL cubemaps, bloom textures, GPU timers,
    // named-texture registry, captured graph snapshot, per-draw + depth
    // preview textures) live on RenderPipeline.
    class RenderingSystem : public ISystem
    {
    public:
        RenderingSystem(u32 viewportWidth = 1280, u32 viewportHeight = 720);
        ~RenderingSystem();

        void Update(Scene* scene) override;
        void Resize(u32 width, u32 height);

        // Queue an extra view to render this frame. Called by editor panels
        // (e.g. GamePanel) before Update; views record in queued order ahead
        // of the scene view's subgraph. Cleared each Update.
        void QueueView(const RenderView& view) { m_QueuedViews.push_back(view); }

        // Project lifecycle hooks: extend / restrict the shader hot-reload
        // watcher to cover the active project's shaders directory.
        void OnProjectLoaded();
        void OnProjectUnloaded();

        std::shared_ptr<Texture> GetSceneColor() const {
            const auto& ldr = m_SceneTargets.GetLDROutput();
            return ldr ? ldr : m_SceneTargets.GetSceneColor();
        }
        PostProcessSettings& GetPostProcessSettings() { return m_PostProcessSettings; }
        const PostProcessSettings& GetPostProcessSettings() const { return m_PostProcessSettings; }

        u64 GetFrameAllocatorUsage() const { return m_FrameAllocator->GetUsedMemory(); }
        u64 GetFrameAllocatorTotal() const { return m_FrameAllocator->GetTotalSize(); }

        const RG::RenderGraphSnapshot& GetGraphSnapshot() const;
        std::shared_ptr<Texture> GetNamedTexture(const std::string& name) const;

        u32 GetTriangleCount() const { return m_DrawList.visibleTriCount; }

        ShadeMode GetShadeMode() const { return m_ShadeMode; }
        void SetShadeMode(ShadeMode mode) { m_ShadeMode = mode; }

        // Accessors used by PickingSystem (readback reads the EntityID target
        // and maps the sampled index back to an entity via the Pipeline) and
        // GamePanel (which owns its own FrameTargets but shares the Pipeline).
        FrameTargets&   GetSceneTargets() { return m_SceneTargets; }
        RenderPipeline& GetPipeline()     { return *m_Pipeline; }

        // Renderer-side accessors. The render path reads these per frame
        // (passes consume DrawList + CameraParams; debugger captures via
        // FrameDebugger; RG allocators come from FrameAllocator).
        FrameDebugger&             GetFrameDebugger()       { return m_FrameDebugger; }
        const FrameDebugger&       GetFrameDebugger() const { return m_FrameDebugger; }
        Memory::LinearAllocator&   GetFrameAllocator()      { return *m_FrameAllocator; }
        const DrawList&            GetDrawList() const      { return m_DrawList; }
        const CameraParams&        GetCameraParams() const  { return m_CameraParams; }

        // Selection outline + editor grid params now flow through CameraParams
        // (populated in App.cpp from EditorViewportState each frame).

        // Skybox / IBL
        void ReloadSkybox(const std::filesystem::path& hdrPath);

        // Editor grid visibility (screen-space infinite grid pass)
        void SetGridVisible(bool v) { m_GridVisible = v; }
        bool IsGridVisible() const  { return m_GridVisible; }

        // Camera / editor state (set by App each frame before Update)
        void SetCameraParams(const CameraParams& params) { m_CameraParams = params; }

        // Snapshot the renderer reads this frame. Set by Update before passes
        // record; consumed by passes (Frame Debugger tag lookups, etc.) via the
        // RenderPipeline friend access. Lifetime is one frame.
        const RenderSnapshot& GetActiveSnapshot() const { return *m_ActiveSnapshot; }

        // Frame debugger capture
        void RequestCapture()   { if (m_FrameDebugger.state == DebuggerState::Inactive) m_FrameDebugger.state = DebuggerState::CaptureRequested; }
        void ExitCapture();
        DebuggerState GetDebuggerState() const { return m_FrameDebugger.state; }
        const RG::CapturedFrame& GetCapturedFrame() const { return m_FrameDebugger.capturedFrame; }
        VkSampler GetDebugSampler() const { return m_FrameDebugger.sampler; }

        // Capture source — Scene (editor camera) or Game (hierarchy camera).
        // Mutating only changes the *next* capture; Frozen overlays use
        // capturedSource so a mid-capture toggle doesn't redirect the overlay
        // to a viewport whose camera never produced these archives.
        void          SetCaptureSource(CaptureSource s) { m_FrameDebugger.requestedSource = s; }
        CaptureSource GetCaptureSource() const          { return m_FrameDebugger.requestedSource; }
        CaptureSource GetCapturedSource() const         { return m_FrameDebugger.capturedSource; }

        // Frame-debugger preview forwarders (implementations on RenderPipeline).
        void        ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx);
        VkImageView GetPerDrawPreviewView() const;
        u64         GetPerDrawPreviewKey()  const;
        u32         GetPerDrawPreviewWidth()  const;
        u32         GetPerDrawPreviewHeight() const;

        void        BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ);
        VkImageView GetDepthPreviewView()   const;
        u32         GetDepthPreviewWidth()  const;
        u32         GetDepthPreviewHeight() const;

        void        BlitArchivedSlimToPreview(u32 archiveIdx, u32 mode, float scale);
        VkImageView GetSlimPreviewView()    const;
        u32         GetSlimPreviewWidth()   const;
        u32         GetSlimPreviewHeight()  const;

    private:
        // Run the per-view prep chain (lighting fit, PrepareForTargets, UBO uploads) and record the subgraph
        // into the view's QueueRecorders triplet. Returns true iff the graph routed any pass to async-compute —
        // forwarded to Renderer::EndPrimaryCmdAndSubmit so SubmitView knows whether to issue the compute submit.
        // Cross-view RAW sync for shared resources (m_ShadowMap) is enforced by the per-view 3-submit topology's
        // timeline waits at submit boundaries (replaces the legacy InsertInterViewBarrier). See arch/multi-queue.md.
        bool RecordView(const RenderView& view, QueueRecorders recorders);

        // Camera / editor state set each frame by App.
        CameraParams m_CameraParams;

        // Extra views queued by editor panels; drained each Update.
        std::vector<RenderView> m_QueuedViews;

        // Memory.
        std::unique_ptr<Memory::LinearAllocator> m_FrameAllocator;

        // Scene panel's render targets. GamePanel owns its own FrameTargets
        // so the two views resize independently.
        FrameTargets m_SceneTargets;

        // Per-frame draw list (RenderMode-sorted buckets + tri count).
        DrawListBuilder m_DrawListBuilder;
        DrawList        m_DrawList;

        // Graphics resources + render-graph orchestration (owns all pipelines,
        // descriptor sets, samplers, UBOs, SSBOs, preview textures, etc.).
        std::unique_ptr<RenderPipeline> m_Pipeline;

        // Editor-facing state.
        PostProcessSettings m_PostProcessSettings;
        ShadeMode           m_ShadeMode    = ShadeMode::Lit;
        bool                m_GridVisible  = true;

        // Frame debugger runtime state (capture state machine + archives).
        FrameDebugger m_FrameDebugger;

        // Snapshot consumed by passes this frame (set in Update; non-owning).
        const RenderSnapshot* m_ActiveSnapshot = nullptr;
    };
}
