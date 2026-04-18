#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/scene/Entity.h"
#include "luth/memory/Memory.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/DrawListBuilder.h"
#include "luth/renderer/FrameDebugger.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/draw/DrawList.h"
#include "luth/renderer/lighting/CascadeBuilder.h"
#include "luth/renderer/lighting/LightGatherer.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/renderer/rendergraph/FrameCapture.h"
#include "luth/renderer/lighting/LightTypes.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/resources/FileWatcher.h"

#include <entt/entt.hpp>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace Luth
{
    class RenderPipeline;
    class Texture;

    // Per-frame global shader inputs (Set 0 UBO). Layout mirrors GLSL binding.
    struct GlobalUniforms {
        glm::mat4 viewProjection;
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec3 cameraPos;
        float     time;
        glm::mat4 lightSpaceMatrix[k_ShadowCascadeCount];
        glm::vec4 cascadeSplitsViewZ;
        glm::vec4 shadowBias;
        glm::vec4 shadowNormalBias;
        glm::vec4 cascadeTexelSize;
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
    // (CameraParams, ShadowParams, Cascades, DrawList, FrameTargets) and
    // orchestrates per-frame work by invoking RenderPipeline.
    //
    // All graphics resources (pipelines, descriptor sets, samplers, UBOs,
    // SSBOs, indirect/object buffers, IBL cubemaps, bloom textures, GPU
    // timers, named-texture registry, captured graph snapshot, per-draw +
    // depth preview textures) live on RenderPipeline. RenderPipeline is
    // friend of RenderingSystem so it can read RS-side scene state
    // (CameraParams, ShadowParams, Cascades, FrameTargets, FrameDebugger,
    // DrawList, editor toggles) without a wide public accessor surface.
    class RenderingSystem : public ISystem
    {
        friend class RenderPipeline;

    public:
        RenderingSystem(u32 viewportWidth = 1280, u32 viewportHeight = 720);
        ~RenderingSystem();

        void Update(Scene* scene) override;
        void Resize(u32 width, u32 height);

        // Project lifecycle hooks: extend / restrict the shader hot-reload
        // watcher to cover the active project's shaders directory.
        void OnProjectLoaded();
        void OnProjectUnloaded();

        std::shared_ptr<Texture> GetSceneColor() const {
            const auto& ldr = m_Targets.GetLDROutput();
            return ldr ? ldr : m_Targets.GetSceneColor();
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

        // Mouse picking
        void RequestPick(int x, int y);
        bool HasPickResult() const { return m_PickResultReady; }
        entt::entity ConsumePickResult();

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
        // Gathers lights + builds cascades on the CPU side, then pushes the result
        // into RenderPipeline via UploadLightUBO + the m_Cascades snapshot.
        void UpdateLightUniforms(Scene* scene);

        // Camera / editor state set each frame by App.
        CameraParams m_CameraParams;

        // Memory.
        std::unique_ptr<Memory::LinearAllocator> m_FrameAllocator;

        // Persistent viewport-sized render targets.
        FrameTargets m_Targets;

        // Per-frame light gathering + CSM cascade fit.
        LightGatherer                m_LightGatherer;
        CascadeBuilder               m_CascadeBuilder;
        DirectionalLightShadowParams m_ShadowParams;
        CascadeData                  m_Cascades;

        // Per-frame draw list (RenderMode-sorted buckets + tri count).
        DrawListBuilder m_DrawListBuilder;
        DrawList        m_DrawList;

        // Graphics resources + render-graph orchestration (owns all pipelines,
        // descriptor sets, samplers, UBOs, SSBOs, preview textures, etc.).
        std::unique_ptr<RenderPipeline> m_Pipeline;

        // Editor-facing state.
        PostProcessSettings m_PostProcessSettings;
        ShadeMode           m_ShadeMode    = ShadeMode::Lit;
        glm::vec4           m_OutlineColor = { 1.0f, 0.6f, 0.0f, 1.0f };
        bool                m_GridVisible  = true;

        // Frame debugger runtime state (capture state machine + archives).
        FrameDebugger m_FrameDebugger;

        // Mouse picking state.
        bool         m_PickPending     = false;
        bool         m_PickResultReady = false;
        glm::ivec2   m_PickCoord       = { 0, 0 };
        entt::entity m_PickedEntity    = entt::null;

        // Shader hot-reload (file watcher + queued reloads drained in Update).
        FileWatcher           m_ShaderWatcher;
        std::filesystem::path m_WatchedProjectShaderDir;
        std::mutex            m_ReloadMutex;
        std::set<std::string> m_PendingReloads;
    };
}
