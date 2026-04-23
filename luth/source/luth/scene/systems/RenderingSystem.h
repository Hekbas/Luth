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

    // Per-frame global shader inputs (Set 0 UBO). Layout mirrors GLSL binding.
    struct GlobalUniforms {
        Mat4 viewProjection;
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

    enum class ShadeMode : u8 { Lit = 0, Unlit, Wireframe, Normals, EntityID };

    struct GeometryOutput {
        RG::ResourceHandle color;
        RG::ResourceHandle depth;
        RG::ResourceHandle entityID;
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
    // All graphics resources (pipelines, descriptor sets, samplers, UBOs,
    // SSBOs, indirect/object buffers, IBL cubemaps, bloom textures, GPU
    // timers, named-texture registry, captured graph snapshot, per-draw +
    // depth preview textures) live on RenderPipeline. RenderPipeline is
    // friend of RenderingSystem so it can read RS-side scene state
    // (CameraParams, FrameTargets, FrameDebugger, DrawList, editor toggles)
    // without a wide public accessor surface.
    class RenderingSystem : public ISystem
    {
        friend class RenderPipeline;
        friend class FrameDebuggerContext;

    public:
        RenderingSystem(u32 viewportWidth = 1280, u32 viewportHeight = 720);
        ~RenderingSystem();

        void Update(Scene* scene) override;
        void Resize(u32 width, u32 height);

        // Queue an additional view for this frame. Called by editor panels
        // (e.g. GamePanel::OnRender) before Update runs — Update collects
        // queued views and records each one into the frame's primary cmd
        // buffer, in queued order, before the primary (scene) view's
        // subgraph. The queue is cleared at the end of Update.
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

        // Selection outline
        void SetOutlineColor(float r, float g, float b, float a) { m_OutlineColor = { r, g, b, a }; }

        // Skybox / IBL
        void ReloadSkybox(const std::filesystem::path& hdrPath);

        // Editor grid visibility (screen-space infinite grid pass)
        void SetGridVisible(bool v) { m_GridVisible = v; }
        bool IsGridVisible() const  { return m_GridVisible; }

        // Camera / editor state (set by App each frame before Update)
        void SetCameraParams(const CameraParams& params) { m_CameraParams = params; }

        // Frame debugger capture
        void RequestCapture()   { if (m_FrameDebugger.state == DebuggerState::Inactive) m_FrameDebugger.state = DebuggerState::CaptureRequested; }
        void ExitCapture();
        DebuggerState GetDebuggerState() const { return m_FrameDebugger.state; }
        const RG::CapturedFrame& GetCapturedFrame() const { return m_FrameDebugger.capturedFrame; }
        VkSampler GetDebugSampler() const { return m_FrameDebugger.sampler; }

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

    private:
        // Per-frame view record-into-cmd helper. Called from Update for both
        // the scene view + each queued view. Owns the per-view state-prep
        // chain (lighting fit → PrepareForTargets → UBO uploads → subgraph
        // record) and writes directly into the supplied primary cmd buffer.
        void RecordView(const RenderView& view, entt::registry& registry, void* primaryCmd);

        // Camera / editor state set each frame by App.
        CameraParams m_CameraParams;

        // Views queued for this frame by editor panels (GamePanel etc.).
        // Cleared at the end of every Update.
        std::vector<RenderView> m_QueuedViews;

        // Memory.
        std::unique_ptr<Memory::LinearAllocator> m_FrameAllocator;

        // Persistent viewport-sized render targets for the scene panel's
        // view. The Game panel owns its own FrameTargets as a direct member
        // (see luthien/.../panels/GamePanel.h) so the two views can resize
        // independently.
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
        Vec4           m_OutlineColor = { 1.0f, 0.6f, 0.0f, 1.0f };
        bool                m_GridVisible  = true;

        // Frame debugger runtime state (capture state machine + archives).
        FrameDebugger m_FrameDebugger;
    };
}
