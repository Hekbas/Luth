#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/UUID.h"
#include "luth/renderer/CameraParams.h"
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
        u32 width  = 0;
        u32 height = 0;

        // Owns every descriptor set below — one vkDestroyDescriptorPool
        // frees them all on release.
        VkDescriptorPool descPool = VK_NULL_HANDLE;

        // Set 0 descriptor: bindings 0 (Global UBO) + 5 (GTAO UBO) are rebound per
        // render-stage to fresh GPUTaggedPageAllocator regions in UpdateGlobalUniforms /
        // UpdateGTAOUBO. IBL samplers (1-3) and GTAO final sampler (4) are stable.
        VkDescriptorSet                  globalDescriptorSet = VK_NULL_HANDLE;

        // Bloom half-res ping-pong textures (RGBA16F).
        std::shared_ptr<Texture> bloomA;
        std::shared_ptr<Texture> bloomB;

        // GTAO half-res storage textures.
        std::shared_ptr<Texture> gtaoLinearDepth;
        std::shared_ptr<Texture> gtaoRawAO;
        std::shared_ptr<Texture> gtaoEdges;
        std::shared_ptr<Texture> gtaoFinal;

        // Bloom extract / blur / composite — bind view's SceneColor +
        // bloomA/B + shared PP UBO.
        VkDescriptorSet bloomExtractDescSet = VK_NULL_HANDLE;
        VkDescriptorSet bloomBlurHDescSet   = VK_NULL_HANDLE;
        VkDescriptorSet bloomBlurVDescSet   = VK_NULL_HANDLE;
        VkDescriptorSet compositeDescSet    = VK_NULL_HANDLE;

        // GTAO compute passes.
        VkDescriptorSet gtaoPrefilterDescSet = VK_NULL_HANDLE;
        VkDescriptorSet gtaoMainDescSet      = VK_NULL_HANDLE;
        VkDescriptorSet gtaoDenoiseDescSet   = VK_NULL_HANDLE;

        // Editor overlays — allocated for every view, bound only by the
        // scene view (game view's subgraph skips both passes via flags).
        VkDescriptorSet outlineDescSet = VK_NULL_HANDLE;
        VkDescriptorSet gridDescSet    = VK_NULL_HANDLE;
    };

    // Owns the per-frame render-graph assembly and execution. Created by
    // RenderingSystem in its ctor and invoked once per frame from Update
    // after BuildGPUObjectBuffer / DrawListBuilder::Build have populated
    // the inputs.
    //
    // RenderPipeline is tightly coupled to RenderingSystem in v1 of this
    // split (sub-task D of epic arch-renderer-split): it reads rendering
    // resources (pipelines, descriptor sets, SPIR-V, samplers) through a
    // RenderingSystem& reference. Sub-task E migrates those resources
    // onto RenderPipeline itself.
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

        // Build and record the render graph for one view into primaryCmd.
        // The cmd's begin/end/submit is owned by RenderingSystem::Update so
        // all visible views share one submission per frame. Caches the
        // active RenderView + ViewResources on the pipeline for passes to
        // read without a per-pass parameter.
        void Execute(const RenderView& view, void* primaryCmd);

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

        // Uploads LightUniforms to the light UBO. Called from RenderingSystem::
        // Update after LightingSystem populates the struct.
        void UploadLightUBO(const LightUniforms& lights);

        // Editor + frame-debugger lookups (RenderingSystem forwards to these).
        std::shared_ptr<Texture> GetNamedTexture(const std::string& name) const;
        void ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx);
        void BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ);

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

        // Temp accessors — expose state that *Subsystem extractions will own. Each is removed in
        // the sub-task that extracts it. invariant: all gone by sub-task E.
        const std::vector<u32>& GetFullscreenVertSpv() const   { return m_FullscreenVertSpv; }
        VkSampler               GetGTAOSampler() const         { return m_GTAOSampler; }

    private:
        // Init / Update helpers for the subsystems still on RP (GTAO /
        // PostProcess / EditorOverlays). Per-view state allocated lazily by
        // EnsureViewResources.
        void InitPostProcessResources();
        void InitAOResources();
        void CreatePipelines();
        void BuildSelectionPipelines();
        void BuildPostPipelines();
        void BuildOutlinePipeline();
        void BuildGridPipeline();
        void RegisterNamedTextures();

        // Render-graph pass builders. Each declares one RG pass (setup +
        // execute lambdas) and returns a handle to its primary output so
        // callers can chain the graph. All pass files live under
        // renderer/passes/ and used to be RenderingSystem methods.
        RG::ResourceHandle AddGTAODepthPrefilterPass(RG::RenderGraph& rg, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddGTAOMainPass(RG::RenderGraph& rg, RG::ResourceHandle linearDepth);
        RG::ResourceHandle AddGTAODenoisePass(RG::RenderGraph& rg, RG::ResourceHandle rawAO, RG::ResourceHandle linearDepth);
        RG::ResourceHandle AddBloomPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);
        RG::ResourceHandle AddPostProcessPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle bloomResult);
        SelectionMaskOutput AddSelectionMaskPass(RG::RenderGraph& rg);
        RG::ResourceHandle AddOutlinePass(RG::RenderGraph& rg, RG::ResourceHandle ldrOutput, SelectionMaskOutput maskOutput, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddGridPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);
        void AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);

        void CollectSelectedHandles(const std::vector<Entity>& selected, std::unordered_set<entt::entity>& outHandles) const;

        RG::RenderGraphSnapshot CaptureSnapshot(const RG::RenderGraph& rg);

        RenderingSystem& m_System;
        std::unique_ptr<FrameDebuggerContext> m_Debugger;

        // Active RenderView + ViewResources for the current Execute call.
        // Passes read these instead of taking the view as a parameter.
        const RenderView*  m_CurrentView          = nullptr;
        ViewResources*     m_CurrentViewResources = nullptr;

        // Per-view resource cache. Entries are owned here; panels call
        // ReleaseViewResources on destruction.
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

    private:
        // Split allocation + per-group descriptor writes for readability.
        void AllocateViewResources(ViewResources& vr, FrameTargets& targets);
        void RecreateViewTextures(ViewResources& vr, u32 halfW, u32 halfH);
        void WriteViewPostProcessSets(ViewResources& vr, FrameTargets& targets);
        void WriteViewGTAOSets(ViewResources& vr, FrameTargets& targets);
        void WriteViewOutlineSet(ViewResources& vr, FrameTargets& targets);
        void WriteViewGridSet(ViewResources& vr, FrameTargets& targets);
        void DestroyViewResources(ViewResources& vr);

        // ---- Subsystems (own their domain state + lifecycle + passes) ----
        GlobalSubsystem   m_Global;
        LightingSubsystem m_Lighting;
        GeometrySubsystem m_Geometry;

        // ---- GTAO shared state (per-view textures/UBO/sets in ViewResources) ----
        std::unique_ptr<VKComputePipeline> m_GTAOPrefilterPipeline;
        std::unique_ptr<VKComputePipeline> m_GTAOMainPipeline;
        std::unique_ptr<VKComputePipeline> m_GTAODenoisePipeline;
        VkSampler                          m_GTAOSampler             = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_GTAOPrefilterDescLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_GTAOMainDescLayout      = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_GTAODenoiseDescLayout   = VK_NULL_HANDLE;
        std::vector<u32>                   m_GTAOPrefilterSpv;
        std::vector<u32>                   m_GTAOMainSpv;
        std::vector<u32>                   m_GTAODenoiseSpv;

        // ---- Post-process shared state (per-view bloom + sets in ViewResources) ----
        // PostProcess UBO is allocated per render-stage from GPUTaggedPageAllocator;
        // binding 2 of each PP set is rewritten in UpdatePostProcessUBO each frame.
        VkSampler              m_PPSampler       = VK_NULL_HANDLE;
        VkDescriptorSetLayout  m_PPDescSetLayout = VK_NULL_HANDLE;

        // ---- Post-process pipelines ----
        std::unique_ptr<VKPipeline> m_BloomExtractPipeline;
        std::unique_ptr<VKPipeline> m_BloomBlurPipeline;
        std::unique_ptr<VKPipeline> m_PostProcessPipeline;

        // ---- Post-process shader SPIR-V ----
        std::vector<u32> m_FullscreenVertSpv;
        std::vector<u32> m_BloomExtractFragSpv;
        std::vector<u32> m_BloomBlurFragSpv;
        std::vector<u32> m_PostProcessFragSpv;

        // ---- Selection mask pipelines ----
        std::unique_ptr<VKPipeline> m_SelectionMaskPipeline;
        std::unique_ptr<VKPipeline> m_SelectionMaskSkinnedPipeline;
        std::vector<u32>            m_SelectionMaskVertSpv;
        std::vector<u32>            m_SelectionMaskFragSpv;
        std::vector<u32>            m_SelectionMaskSkinnedVertSpv;

        // ---- Outline shared state (per-view set in ViewResources) ----
        std::unique_ptr<VKPipeline> m_OutlinePipeline;
        std::vector<u32>            m_OutlineFragSpv;
        VkDescriptorSetLayout       m_OutlineDescSetLayout = VK_NULL_HANDLE;
        VkSampler                   m_OutlineSampler       = VK_NULL_HANDLE;

        // ---- Grid shared state (per-view set in ViewResources) ----
        std::unique_ptr<VKPipeline> m_GridPipeline;
        std::vector<u32>            m_GridFragSpv;
        VkDescriptorSetLayout       m_GridDescSetLayout = VK_NULL_HANDLE;
        VkSampler                   m_GridDepthSampler  = VK_NULL_HANDLE;

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
        const RG::RenderGraphSnapshot& GetGraphSnapshot() const { return m_GraphSnapshot; }

        // Resets the per-draw preview cache key — called from RS::ExitCapture.
        void ResetPreviewCacheKeys();
    };
}
