#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/UUID.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/QueueRecorders.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/renderer/lighting/LightTypes.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/GPUTimerPool.h"
#include "luth/renderer/pipeline/PipelineManager.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/shader/ShaderWatcher.h"
#include "luth/renderer/subsystems/GlobalSubsystem.h"
#include "luth/renderer/subsystems/LightingSubsystem.h"
#include "luth/renderer/subsystems/GeometrySubsystem.h"
#include "luth/renderer/subsystems/GTAOSubsystem.h"
#include "luth/renderer/subsystems/VolumetricSubsystem.h"
#include "luth/renderer/subsystems/PostProcessSubsystem.h"
#include "luth/renderer/subsystems/EditorOverlaysSubsystem.h"
#include "luth/renderer/subsystems/DebugDrawSubsystem.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/subsystems/RtRestirSubsystem.h"
#include "luth/renderer/subsystems/SkinningSubsystem.h"
#include "luth/renderer/subsystems/IDenoiser.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

#include <entt/entt.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Luth
{
    class Entity;
    class FrameTargets;
    class Material;
    class RenderingSystem;
    class FrameDebuggerContext;
    struct GeometryOutput;
    struct SelectionMaskOutput;
    struct RenderSnapshot;
    namespace fs = std::filesystem;

    // Per-view input to RenderPipeline::Execute. One RenderView per visible
    // viewport. targets is non-owning.
    //
    // viewIndex selects the view's slice of the shared indirect buffer:
    // regions [viewIndex * k_IndirectRegionsPerView, +k_IndirectRegionsPerView).
    // emitImGuiPass: only the primary view records the ImGui pass (backbuffer
    // write + ImGui::GetDrawData — once per frame, after ImGui::Render).
    struct RenderView
    {
        FrameTargets* targets              = nullptr;
        CameraParams  camera;
        u32           viewIndex            = 0;
        bool          drawGrid             = true;
        bool          drawSelectionOutline = true;
        bool          drawDebugShapes      = false;  // off by default; scene view enables explicitly
        bool          emitImGuiPass        = true;
        // Set by the view's owner when the user has requested a Frame Debugger
        // capture and selected this view (Scene or Game) as the source. Drives
        // the BeginCapture / archive-sink wiring in RenderPipeline::Execute.
        // Decoupled from emitImGuiPass so capture can target the game view.
        bool          captureRequested     = false;
    };

    // GPU resources bound to a specific FrameTargets. Keyed by targets
    // pointer in RenderPipeline::m_ViewResources, allocated on first use
    // by EnsureViewResources, recreated on size change, destroyed on
    // ReleaseViewResources or pipeline shutdown. Having a distinct set
    // per view lets multiple subgraphs share one primary command buffer
    // without mid-frame vkUpdateDescriptorSets aliasing.
    struct ViewResources
    {
        // Identity token minted at creation; survives resize, dies with
        // ReleaseViewResources. invariant: replay validates against this
        // to catch FrameTargets-pointer reuse after panel close.
        u64 id     = 0;
        u32 width  = 0;
        u32 height = 0;

        // Owns every descriptor set below — one vkDestroyDescriptorPool frees them all on release.
        VkDescriptorPool descPool = VK_NULL_HANDLE;

        // Set 0 descriptor: bindings 0 (Global UBO) + 5 (GTAO UBO) are rebound per
        // render-stage to fresh GPUTaggedPageAllocator regions in UpdateGlobalUniforms /
        // UpdateGTAOUBO against per-frame slot. IBL samplers (1-3) and GTAO final
        // sampler (4) are stable — replicated across all slots at WriteView time.
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> globalDescriptorSet{};

        // Bloom half-res ping-pong textures (RGBA16F).
        std::shared_ptr<Texture> bloomA;
        std::shared_ptr<Texture> bloomB;

        // GTAO half-res storage textures.
        std::shared_ptr<Texture> gtaoLinearDepth;
        std::shared_ptr<Texture> gtaoRawAO;
        std::shared_ptr<Texture> gtaoEdges;
        std::shared_ptr<Texture> gtaoFinal;

        // Volumetric fog atlases (RGBA16F). View-frustum-aligned; persistent across frames so the
        // resolve pass can reproject + blend with prev frame's resolved output. Allocated via the
        // 3D VKTexture ctor (STORAGE + SAMPLED, null internal sampler — VolumetricSubsystem owns
        // the shared linear-clamp sampler). Dims pulled from VolumetricSettings::quality preset.
        //
        // volInScatter is the scratch atlas — inject writes pre-integrate per-voxel scatter, then
        // integrate reads + writes the post-integrate cumulative in-place. volInScatterHistA/B
        // ping-pong as the temporal-resolve I/O pair: each frame the resolve pass reads one as
        // "prev resolved" and writes the other as "current resolved". Composite + viz sample the
        // "current resolved" atlas of the active frame parity.
        std::shared_ptr<Texture> volDensity;
        std::shared_ptr<Texture> volInScatter;
        std::shared_ptr<Texture> volInScatterHistA;
        std::shared_ptr<Texture> volInScatterHistB;

        // Bloom extract / blur / composite — bind view's SceneColor +
        // bloomA/B + shared PP UBO. Cycled — UpdateUBO writes binding 2 of
        // all 4 sets atomically against the per-frame slot.
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> bloomExtractDescSet{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> bloomBlurHDescSet{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> bloomBlurVDescSet{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> compositeDescSet{};

        // GTAO compute passes.
        VkDescriptorSet gtaoPrefilterDescSet = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> gtaoMainDescSet{};
        VkDescriptorSet gtaoDenoiseDescSet   = VK_NULL_HANDLE;

        // Editor overlays — allocated for every view, bound only by the
        // scene view (game view's subgraph skips both passes via flags).
        VkDescriptorSet outlineDescSet = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> gridDescSet{};

        // Slim G-buffer live viz (ShadeMode toggle). Single set, written once at AllocateViewResources
        // time pointing at the 4 slim FrameTargets. Bindings: 0=normal, 1=roughness, 2=motion, 3=matID.
        VkDescriptorSet slimVizDescSet = VK_NULL_HANDLE;

        // Forward+ cluster compute descriptor sets. Cycled — each frame the cluster AABB + grid
        // tagged-heap regions get rewritten into the slot's bindings before dispatch.
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> clusterBuildDescSet{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> lightAssignDescSet{};

        // Volumetric inject density pass. Cycled — b1 (FogVolume SSBO) rewrites per frame against
        // a fresh tagged-heap region; b0 + b2 are stable per-view.
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> volInjectDensityDescSet{};

        // Volumetric inject scatter pass. Cycled — b2-b4 (Light, ClusterGrid, LightIndex SSBOs)
        // rewrite per frame; b0/b1/b5 stable. Reads volDensity written by the density pass via the
        // shared ResourceNode (RG inserts the barrier).
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> volInjectScatterDescSet{};

        // Volumetric integrate pass. Cycled; reads + writes volInScatter (scratch) in-place.
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> volIntegrateDescSet{};

        // Volumetric resolve pass. Cycled; reads scratch + prev history (parity), writes curr
        // history (parity). Temporal accumulation happens here, post-integrate.
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> volResolveDescSet{};

        // Volumetric composite. Cycled — b1 (in-scatter sampler) parity-picks the resolved history
        // atlas (HistA or HistB). b0 (sceneDepth sampler) is stable across slots.
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> volCompositeDescSet{};

        // Volumetric debug viz. Cycled — b2 follows the same ping-pong parity as composite.
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> volVizDescSet{};

        // Set 3 (Lighting). Per-view because cluster grid + light index are per-view; LightSSBO
        // also lives in a per-view tagged-heap region. b3 (shadow sampler) written once at view
        // alloc time, propagates to all slots. b0/b1/b2 rebound each frame by UploadLightingResources.
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> lightDescSet{};

        // Cluster debug viz: single set, 1 binding = SceneDepth sampler. Stable per-view; written
        // by WriteClusterVizView at AllocateViewResources time.
        VkDescriptorSet clusterVizDescSet = VK_NULL_HANDLE;

        // Per-view previous-frame view-projection — feeds ubo.prevViewProjection for motion vectors.
        // GlobalSubsystem::m_CachedViewProj is shared across views, so multi-view rendering (Scene +
        // Game panel) cross-contaminates the prev-VP. Per-view storage keeps each view's prev-VP
        // independent. Identity-initialized → frame 0 has nonsense motion, settles by frame 1.
        Mat4 prevViewProj{ 1.0f };

        // Prev-frame near/far for the resolve pass's reprojection slice math — needed because the
        // current frame's nearZ/farZ may differ if camera FOV/clip planes animate.
        f32 prevNearZ = 0.0f;
        f32 prevFarZ  = 0.0f;

        // Volumetric atlas dimensions live here so changing VolumetricSettings::quality at runtime
        // re-allocates the atlases (mirrors width/height resize handling). volQualityCached tracks
        // the value at last allocation; EnsureViewResources compares + recreates on mismatch.
        u32 volDimX = 0, volDimY = 0, volDimZ = 0;
        u32 volQualityCached = ~0u;

        // TAA history (Karis14 YCoCg-clip recipe). Viewport-sized RGBA16F, persistent across frames,
        // ping-pong via frameAbs parity matching volInScatterHistA/B shape. Bootstrap-cleared at
        // resize so frame 0 history read is well-defined. currentJitter / prevJitter are NDC-space
        // sub-pixel offsets stored on the view because GlobalSubsystem is shared across views.
        Vec2 currentJitter{ 0.0f, 0.0f };
        Vec2 prevJitter{ 0.0f, 0.0f };
        std::shared_ptr<Texture> taaHistoryA;
        std::shared_ptr<Texture> taaHistoryB;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> taaResolveDescSet{};

        // RT sun-shadow mask (Phase B.3). Viewport-sized R8 storage image, written by raygen on
        // AsyncCompute and sampled by pbr.frag (Set 3 binding 4) when ShadowingMode::RtShadows is
        // active. Lifetime mirrors taaHistoryA/B — persistent, recreated on resize. The cycled
        // descriptor set carries the pass-local bindings (SceneDepth + slimNormal + mask storage).
        std::shared_ptr<Texture> sunShadowMask;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> rtShadowPassDescSet{};

        // ReSTIR DI (Bitterli 2020). restirReservoir[2] is a ping-pong pair of Garlic device-local
        // large-tagged buffers (w*h*32 B each) reused across frames — freed only on resize via
        // FreeTag. Tags stay in NextReservoirTag's reserved high range, disjoint from the per-frame
        // FreeTag(N-2) sweep. Temporal reuse reads last frame's reservoir (prev) while writing this
        // frame's (curr); parity = frameAbs & 1u picks which slot is curr — Set 2 b2/b4 rebound
        // per frame to swap. restirDI is viewport-sized rgba16f STORAGE+SAMPLED (demodulated diffuse
        // irradiance, consumed by pbr.frag Set 3 b5). The cycled set carries Set 2's depth/normal +
        // motion samplers + reservoir SSBOs + DI storage image.
        Memory::GPUSubRegion restirReservoir[2]{};
        u32 restirReservoirTag[2] = { 0, 0 };
        // Spatial-reuse output — a SINGLE Garlic device-local buffer (not ping-pong): fully
        // overwritten then consumed each frame. The spatial pass reads b2 (temporal output) for
        // self+neighbours and writes here (Set 2 b6); shade reads it. Reserved high tag, freed only
        // on resize/destroy. The temporal ping-pong (b2) stays intact as next frame's history.
        Memory::GPUSubRegion restirSpatial{};
        u32 restirSpatialTag = 0;
        std::shared_ptr<Texture> restirDI;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> restirDescSet{};

        // SVGF denoiser output — viewport-sized RGBA16F STORAGE+SAMPLED, same shape as restirDI. The
        // denoiser reads restirDI (noisy demodulated DI) and writes the denoised result here; pbr.frag
        // Set 3 b5 samples THIS (not restirDI), so the denoiser owns the slot whenever ReSTIR is on and
        // the A/B is denoise-vs-raw with no binding swap. History + per-pass sets grow as the SVGF
        // passes land. see arch/rendering-pipeline.md
        std::shared_ptr<Texture> svgfDenoised;
        VkDescriptorSet svgfPassthroughDescSet = VK_NULL_HANDLE;

        // SVGF temporal history — RGBA16F storage images kept in GENERAL, ping-pong by frame parity
        // (curr = [p], prev = [p^1]). colorHist = integrated color (rgb) + variance (a); moments =
        // (mu1, mu2, histLen); geom = (linearZ, octN.x, octN.y) for the disocclusion test. Bootstrap-
        // cleared so frame 0's prev read is well-defined. The two reproject sets are pre-built per
        // parity (set[p] reads [p^1], writes [p]); AddPasses binds svgfReprojectDescSet[frameAbs & 1].
        std::shared_ptr<Texture> svgfColorHist[2];
        std::shared_ptr<Texture> svgfMoments[2];
        std::shared_ptr<Texture> svgfGeom[2];
        VkDescriptorSet svgfReprojectDescSet[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    };

    // Orchestrates per-frame render-graph assembly and execution. Created by RenderingSystem and
    // invoked once per frame after the DrawList and GPUObjectBuffer have been populated. Owns six
    // per-domain subsystems (Global, Lighting, Geometry, GTAO, PostProcess, EditorOverlays); each
    // subsystem contributes its render-graph passes and manages the descriptor lifecycle for the
    // Sets it owns. See arch/rendering-pipeline.md.
    class RenderPipeline
    {
    public:
        explicit RenderPipeline(RenderingSystem& system);
        ~RenderPipeline();

        // One-time init: allocates all Vulkan pipeline resources (UBOs,
        // descriptor sets, samplers, SPIR-V, IBL maps, pipelines). Runs
        // when the Vulkan backend is active; no-op otherwise. Called from
        // RenderingSystem::ctor after FrameTargets has been allocated.
        void Initialize(u32 viewportWidth, u32 viewportHeight);

        // Tear down all Vulkan resources. Called from RenderingSystem::dtor.
        void Shutdown();

        // Build and record the render graph for one view into the per-view QueueRecorders triplet (gA / compute /
        // gB primary command buffers). Begin/end/submit is owned by RenderingSystem::Update — all visible views
        // share the per-frame ring of recorders. Caches the active RenderView + ViewResources on the pipeline so
        // passes can read them without a per-pass parameter. Returns true iff any pass routed to async-compute,
        // for the backend's per-view submit topology to decide whether to issue the compute submit at all.
        bool Execute(const RenderView& view, QueueRecorders recorders);

        // Minimal graph (ImGui only). Used by the Frame Debugger Frozen state
        // when the camera hasn't moved — the LDR output still holds the last
        // captured image, so redrawing just the UI is enough to keep Dear ImGui
        // responsive without rebuilding the full graph.
        void ExecuteMinimal();

        // Post-resize hook — rebuilds the scene view's ViewResources entry
        // at the new size. Game panel resizes route through its own
        // FrameTargets and hit EnsureViewResources directly.
        void OnResize(u32 width, u32 height);

        // Cache m_CurrentViewResources before the per-view UBO writes in
        // RenderingSystem::RecordView run (they read it). Safe to call
        // every frame — only re-allocates on target-size change.
        void PrepareForTargets(FrameTargets& targets);

        // Reload the environment HDR → irradiance + prefiltered cubemaps + BRDF LUT.
        void ReloadSkybox(const fs::path& hdrPath);

        // Per-frame CPU-side GPU state prep. Called from RenderingSystem::
        // RenderToView before the graph executes. The CascadeData +
        // DirectionalLightShadowParams are produced by LightingSystem and
        // cached on this Pipeline for the remainder of the view (Execute +
        // CaptureSnapshot read them through m_FrameCascades / m_FrameShadowParams).
        void UpdateGlobalUniforms(const CameraParams& camera, const CascadeData& cascades, const DirectionalLightShadowParams& shadowParams);
        void UpdatePostProcessUBO();
        void UpdateGTAOUBO();
        // Allocates this frame's Object + Indirect regions from GPUTaggedPageAllocator,
        // populates them from snapshot, and rewrites Set 5 + cull descriptors. Tagged
        // with the absolute render-frame index so FreeTag(N-2) reclaims them once the
        // GPU has retired the consuming frame.
        void BuildGPUObjectBuffer(const RenderSnapshot& snapshot);
        u32  EnsureMaterialRegistered(std::shared_ptr<Material> material);

        // Editor + frame-debugger lookups (RenderingSystem forwards to these).
        std::shared_ptr<Texture> GetNamedTexture(const std::string& name) const;
        void ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx);
        void BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ);
        void BlitArchivedSlimToPreview(u32 archiveIdx, u32 mode, float scale);

        // Maps consumed by DrawListBuilder (populated by BuildGPUObjectBuffer).
        const std::unordered_map<UUID, u32, UUIDHash>& GetMaterialSlotMap() const { return m_Geometry.GetMaterialSlotMap(); }
        const std::unordered_map<entt::entity, u32>& GetEntityToSSBOIndex() const { return m_Geometry.GetEntityToSSBOIndex(); }

        // Entity lookup table for mouse picking (index 0 = null sentinel;
        // valid entities start at 1). Populated by BuildGPUObjectBuffer.
        const std::vector<entt::entity>& GetEntityLookup() const { return m_Geometry.GetEntityLookup(); }

        // Engine-side hot-reload service for .vert/.frag/.comp files. Project
        // shader dirs register via RenderingSystem::OnProjectLoaded, which
        // forwards to this getter.
        ShaderWatcher& GetShaderWatcher() { return m_ShaderWatcher; }

        // Owning RenderingSystem (set by ctor). FrameDebuggerContext + future
        // subsystems read scene state through this accessor.
        RenderingSystem&       GetSystem()       { return m_System; }
        const RenderingSystem& GetSystem() const { return m_System; }

        // Active per-view scratch (set during Execute; consumed by FrameDebuggerContext + subsystems).
        const RenderView*    GetCurrentView()                { return m_CurrentView; }
        ViewResources*       GetCurrentViewResources()       { return m_CurrentViewResources; }
        const ViewResources* GetCurrentViewResources() const { return m_CurrentViewResources; }

        // Subsystem accessors — preferred path for cross-subsystem reads.
        GlobalSubsystem&         GetGlobal()         { return m_Global; }
        const GlobalSubsystem&   GetGlobal()   const { return m_Global; }
        LightingSubsystem&       GetLighting()       { return m_Lighting; }
        const LightingSubsystem& GetLighting() const { return m_Lighting; }
        GeometrySubsystem&       GetGeometry()       { return m_Geometry; }
        const GeometrySubsystem& GetGeometry() const { return m_Geometry; }
        GTAOSubsystem&           GetGTAO()         { return m_GTAO; }
        const GTAOSubsystem&     GetGTAO()   const { return m_GTAO; }
        PostProcessSubsystem&       GetPostProcess()       { return m_PostProcess; }
        const PostProcessSubsystem& GetPostProcess() const { return m_PostProcess; }

    private:
        void RegisterNamedTextures();

        // ImGui pass — single-view residual on the orchestrator.
        void AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);

        RG::RenderGraphSnapshot CaptureSnapshot(const RG::RenderGraph& rg);

        RenderingSystem& m_System;
        std::unique_ptr<FrameDebuggerContext> m_Debugger;

        // Active RenderView + ViewResources for the current Execute call.
        // Passes read these instead of taking the view as a parameter.
        const RenderView*  m_CurrentView          = nullptr;
        ViewResources*     m_CurrentViewResources = nullptr;

        // Per-view resource cache. Entries are owned here; panels call ReleaseViewResources on destruction.
        std::unordered_map<FrameTargets*, ViewResources> m_ViewResources;

        // ---- Constants (shared with RS-side callers when needed) ----
    public:
        static constexpr u32 k_MaxGPUObjects          = 4096;
        static constexpr u32 k_MaxViews               = 2; // scene + game; bump for PIP / reflections later
        static constexpr u32 k_IndirectRegionsPerView = 1 + k_ShadowCascadeCount;
        static constexpr u32 k_IndirectRegionCount    = k_MaxViews * k_IndirectRegionsPerView;
        static constexpr u32 k_IndirectRegionStride   = k_MaxGPUObjects;

        // Return the cached ViewResources for targets, allocating on first
        // use and rebuilding textures/descriptors on size change. Release*
        // is called from the owning panel's dtor. Host-only — safe to call
        // outside a frame.
        ViewResources& EnsureViewResources(FrameTargets& targets);
        void           ReleaseViewResources(FrameTargets& targets);

        // True iff `targets` has a cached ViewResources whose identity token
        // matches `expectedId`. Replay path uses this to validate that the
        // captured FrameTargets is still the same instance — pointer reuse
        // after panel close + reopen would mint a different id.
        bool HasViewResources(FrameTargets* targets, u64 expectedId) const;

        // Lookup without minting — returns nullptr if `targets` isn't in the
        // map. Used by replay to fetch the captured view's resources.
        ViewResources*       GetViewResources(FrameTargets* targets);
        const ViewResources* GetViewResources(FrameTargets* targets) const;

    private:
        // Split allocation + per-group descriptor writes for readability.
        void AllocateViewResources(ViewResources& vr, FrameTargets& targets);
        void RecreateViewTextures(ViewResources& vr, u32 fullW, u32 fullH, u32 halfW, u32 halfH);
        void DestroyViewResources(ViewResources& vr);

        // ---- Subsystems (own their domain state + lifecycle + passes) ----
        GlobalSubsystem         m_Global;
        LightingSubsystem       m_Lighting;
        GeometrySubsystem       m_Geometry;
        GTAOSubsystem           m_GTAO;
        VolumetricSubsystem     m_Volumetric;
        PostProcessSubsystem    m_PostProcess;
        EditorOverlaysSubsystem m_EditorOverlays;
        DebugDrawSubsystem      m_DebugDraw;
        RtSubsystem             m_Rt;
        RtRestirSubsystem       m_Restir;
        SkinningSubsystem       m_Skinning;
        std::unique_ptr<IDenoiser> m_Denoise;  // SVGF today; swappable to NRD/RELAX via the settings toggle

    public:
        EditorOverlaysSubsystem&       GetEditorOverlays()       { return m_EditorOverlays; }
        const EditorOverlaysSubsystem& GetEditorOverlays() const { return m_EditorOverlays; }
        RtSubsystem&                   GetRt()                   { return m_Rt; }
        const RtSubsystem&             GetRt()             const { return m_Rt; }
        RtRestirSubsystem&             GetRestir()               { return m_Restir; }
        const RtRestirSubsystem&       GetRestir()         const { return m_Restir; }
        SkinningSubsystem&             GetSkinning()             { return m_Skinning; }
        const SkinningSubsystem&       GetSkinning()       const { return m_Skinning; }
        IDenoiser&                     GetDenoise()              { return *m_Denoise; }
        const IDenoiser&               GetDenoise()        const { return *m_Denoise; }

    private:
        // ---- Graph snapshot + GPU timers + named-texture registry ----
        RG::RenderGraphSnapshot m_GraphSnapshot;
        GPUTimerPool            m_GPUTimers;
        std::unordered_map<std::string, std::shared_ptr<Texture>> m_NamedTextures;

        // ---- Shader hot-reload (engine + project dirs) ----
        ShaderWatcher m_ShaderWatcher;

    public:
        // Accessors forwarded to the frame-debugger context so editor panels
        // can sample preview textures and invalidate caches without needing
        // access to the context class directly.
        VkImageView GetPerDrawPreviewView()  const;
        u64         GetPerDrawPreviewKey()   const;
        u32         GetPerDrawPreviewWidth() const;
        u32         GetPerDrawPreviewHeight()const;
        VkImageView GetDepthPreviewView()    const;
        u32         GetDepthPreviewWidth()   const;
        u32         GetDepthPreviewHeight()  const;
        VkImageView GetSlimPreviewView()     const;
        u32         GetSlimPreviewWidth()    const;
        u32         GetSlimPreviewHeight()   const;
        const RG::RenderGraphSnapshot& GetGraphSnapshot() const { return m_GraphSnapshot; }

        // Resets the per-draw preview cache key — called from RS::ExitCapture.
        void ResetPreviewCacheKeys();
    };
}
