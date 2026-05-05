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
        const std::unordered_map<UUID, u32, UUIDHash>& GetMaterialSlotMap() const { return m_MaterialSlotMap; }
        const std::unordered_map<entt::entity, u32>& GetEntityToSSBOIndex() const { return m_EntityToSSBOIndex; }

        // Entity lookup table for mouse picking (index 0 = null sentinel;
        // valid entities start at 1). Populated by BuildGPUObjectBuffer.
        const std::vector<entt::entity>& GetEntityLookup() const { return m_EntityLookup; }

        // Engine-side hot-reload service for .vert/.frag/.comp files. Project
        // shader dirs register via RenderingSystem::OnProjectLoaded, which
        // forwards to this getter.
        ShaderWatcher& GetShaderWatcher() { return m_ShaderWatcher; }

    private:
        // Init / Update helpers (moved from RenderingSystem in sub-task E1).
        // Per-view state (bloom / GTAO textures, UBOs, descriptor sets) is
        // allocated lazily by EnsureViewResources; these Init*Resources
        // functions now set up only the shared state (layouts, samplers,
        // pipelines, shared UBOs with view-independent content).
        void InitGlobalUniforms();
        void InitShadowResources();
        void InitPostProcessResources();
        void InitIBLResources(const fs::path& hdrPath);
        void InitObjectSSBODescriptorLayout();
        void InitGPUObjectBuffers();
        void InitCullPipeline();
        void InitAOResources();
        void CreatePipelines();
        void BuildPBRPipelines();
        void BuildShadowPipelines();
        void BuildDepthPrepassPipelines();
        void BuildSelectionPipelines();
        void BuildSkyboxPipeline();
        void BuildPostPipelines();
        void BuildOutlinePipeline();
        void BuildGridPipeline();
        void RegisterNamedTextures();

        // Render-graph pass builders. Each declares one RG pass (setup +
        // execute lambdas) and returns a handle to its primary output so
        // callers can chain the graph. All pass files live under
        // renderer/passes/ and used to be RenderingSystem methods.
        RG::ResourceHandle AddDepthPrepass(RG::RenderGraph& rg, RG::BufferHandle indirectBufferHandle);
        RG::ResourceHandle AddGTAODepthPrefilterPass(RG::RenderGraph& rg, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddGTAOMainPass(RG::RenderGraph& rg, RG::ResourceHandle linearDepth);
        RG::ResourceHandle AddGTAODenoisePass(RG::RenderGraph& rg, RG::ResourceHandle rawAO, RG::ResourceHandle linearDepth);
        RG::ResourceHandle AddShadowPass(RG::RenderGraph& rg, RG::BufferHandle indirectBufferHandle, u32 cascadeIndex);
        GeometryOutput AddGeometryPass(RG::RenderGraph& rg,
                                        const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount],
                                        RG::BufferHandle indirectBufferHandle,
                                        RG::ResourceHandle sceneDepth,
                                        RG::ResourceHandle gtaoFinalAO);
        RG::ResourceHandle AddSkyboxPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddBloomPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);
        RG::ResourceHandle AddPostProcessPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle bloomResult);
        SelectionMaskOutput AddSelectionMaskPass(RG::RenderGraph& rg);
        RG::ResourceHandle AddOutlinePass(RG::RenderGraph& rg, RG::ResourceHandle ldrOutput, SelectionMaskOutput maskOutput, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddGridPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);
        void AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);

        void CollectSelectedHandles(const std::vector<Entity>& selected, std::unordered_set<entt::entity>& outHandles) const;

        RG::RenderGraphSnapshot CaptureSnapshot(const RG::RenderGraph& rg);

        friend class FrameDebuggerContext;

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
        void WriteViewGlobalSet(ViewResources& vr);
        void WriteViewPostProcessSets(ViewResources& vr, FrameTargets& targets);
        void WriteViewGTAOSets(ViewResources& vr, FrameTargets& targets);
        void WriteViewOutlineSet(ViewResources& vr, FrameTargets& targets);
        void WriteViewGridSet(ViewResources& vr, FrameTargets& targets);
        void DestroyViewResources(ViewResources& vr);

        // ---- Set 0 layout (shared; per-view UBO + set in ViewResources) ----
        VkDescriptorSetLayout m_GlobalSetLayout = VK_NULL_HANDLE;

        // ---- Shadow map (4-layer 2D array) + per-layer views ----
        std::shared_ptr<Texture> m_ShadowMap;
        VkImageView              m_ShadowLayerViews[k_ShadowCascadeCount] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkSampler                m_ShadowSampler = VK_NULL_HANDLE;

        // ---- Set 3: Light UBO (per-frame from GPU tagged heap) + shadow sampler (stable) ----
        VkDescriptorPool      m_LightDescPool  = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_LightSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet       m_LightDescSet   = VK_NULL_HANDLE;

        // ---- Shadow pipeline (depth-only) ----
        std::unique_ptr<VKPipeline> m_ShadowPipeline;
        std::unique_ptr<VKPipeline> m_ShadowSkinnedPipeline;
        std::vector<u32>            m_ShadowVertSpv;
        std::vector<u32>            m_ShadowFragSpv;
        std::vector<u32>            m_ShadowSkinnedVertSpv;

        // ---- Depth prepass pipeline ----
        std::unique_ptr<VKPipeline> m_DepthPrepassPipeline;
        std::unique_ptr<VKPipeline> m_DepthPrepassSkinnedPipeline;
        std::vector<u32>            m_DepthPrepassVertSpv;
        std::vector<u32>            m_DepthPrepassSkinnedVertSpv;

        // ---- PBR pipeline manager ----
        PipelineManager  m_GeoPipelineManager;
        PipelineManager  m_GeoSkinnedPipelineManager;
        std::vector<u32> m_PBRVertSpv;
        std::vector<u32> m_PBRFragSpv;
        std::vector<u32> m_PBRSkinnedVertSpv;

        // ---- Material SSBO slot tracking (MaterialUUID -> SSBO index) ----
        std::unordered_map<UUID, u32, UUIDHash> m_MaterialSlotMap;

        // ---- GPU Object + Indirect regions (per-frame, allocated from tagged heap) ----
        // Both regions are allocated each render-stage in BuildGPUObjectBuffer;
        // FreeTag(N-2) returns their pages once the GPU has retired the consuming frame.
        // m_GPUObjectCount carries forward into Execute (cull dispatch + indirect draw).
        Memory::GPUSubRegion m_ObjectRegion{};
        Memory::GPUSubRegion m_IndirectRegion{};
        u32                  m_GPUObjectCount = 0;

        // ---- Set 5 — GPUObjectData SSBO descriptor (graphics pipeline) ----
        VkDescriptorPool      m_ObjectSSBODescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_ObjectSSBODescLayout = VK_NULL_HANDLE;
        VkDescriptorSet       m_ObjectSSBODescSet    = VK_NULL_HANDLE;

        // ---- Entity → SSBO index (rebuilt every frame in BuildGPUObjectBuffer) ----
        std::unordered_map<entt::entity, u32> m_EntityToSSBOIndex;
        std::vector<entt::entity>             m_EntityLookup;

        // ---- Cull compute pipeline + descriptor ----
        std::unique_ptr<VKComputePipeline> m_CullPipeline;
        VkDescriptorPool                   m_CullDescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_CullDescLayout = VK_NULL_HANDLE;
        VkDescriptorSet                    m_CullDescSet    = VK_NULL_HANDLE;

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

        // ---- Cached view-projection (feeds frustum cull + Frozen-state comparison) ----
        Mat4 m_CachedViewProj = Mat4(1.0f);

        // ---- Per-frame lighting snapshot (written by UpdateGlobalUniforms) ----
        // Decouples the pipeline from RenderingSystem's cascade state, which
        // now lives on LightingSystem. Read by Execute (cascade-frustum cull)
        // and the capturedFrame snapshot.
        CascadeData                  m_FrameCascades{};
        DirectionalLightShadowParams m_FrameShadowParams{};

        // ---- Post-process shared state (per-view bloom + sets in ViewResources) ----
        // UBO content is view-independent (scalar settings only).
        std::shared_ptr<VKUniformBuffer> m_PostProcessUBOBuffer;
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

        // ---- IBL resources ----
        std::shared_ptr<Texture> m_IrradianceMap;   // 32x32 cubemap, RGBA16F
        std::shared_ptr<Texture> m_PrefilteredMap;  // 128x128 cubemap, RGBA16F, 5 mips
        std::shared_ptr<Texture> m_BRDFLut;         // 512x512 2D, RG16F
        VkSampler                m_IBLSampler = VK_NULL_HANDLE;

        // ---- Skybox resources ----
        std::unique_ptr<VKPipeline>     m_SkyboxPipeline;
        std::shared_ptr<VKVertexBuffer> m_SkyboxVB;
        std::vector<u32>                m_SkyboxVertSpv;
        std::vector<u32>                m_SkyboxFragSpv;

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
