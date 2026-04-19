#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/UUID.h"
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
    class Material;
    class RenderingSystem;
    class FrameDebuggerContext;
    struct GeometryOutput;
    struct SelectionMaskOutput;
    namespace fs = std::filesystem;

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

        // Build + execute the render graph for one frame.
        void Execute(entt::registry& registry);

        // Minimal graph (ImGui only). Used by the Frame Debugger Frozen state
        // when the camera hasn't moved — the LDR output still holds the last
        // captured image, so redrawing just the UI is enough to keep Dear ImGui
        // responsive without rebuilding the full graph.
        void ExecuteMinimal();

        // Recreate viewport-dependent resources (post-process descriptors,
        // GTAO storage textures, outline/grid descriptors). Called from
        // RenderingSystem::Resize after FrameTargets::Resize.
        void OnResize(u32 width, u32 height);

        // Reload the environment HDR → irradiance + prefiltered cubemaps + BRDF LUT.
        void ReloadSkybox(const fs::path& hdrPath);

        // Per-frame CPU-side GPU state prep. Called from RenderingSystem::Update
        // before the draw list is built and the graph executes. The CascadeData
        // + DirectionalLightShadowParams are produced by LightingSystem and
        // cached on this Pipeline for the remainder of the frame (Execute +
        // CaptureSnapshot read them through m_FrameCascades / m_FrameShadowParams).
        void UpdateGlobalUniforms(const CascadeData& cascades, const DirectionalLightShadowParams& shadowParams);
        void UpdatePostProcessUBO();
        void UpdateGTAOUBO();
        void BuildGPUObjectBuffer(entt::registry& registry);
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
        // All of these read/write RenderingSystem private fields via friend
        // access; field ownership migrates to RenderPipeline in sub-task E3.
        void InitGlobalUniforms();
        void InitShadowResources();
        void InitPostProcessResources();
        void UpdatePostProcessDescriptors();
        void InitIBLResources(const fs::path& hdrPath);
        void InitObjectSSBODescriptorLayout();
        void InitGPUObjectBuffers();
        void InitCullPipeline();
        void InitAOResources();
        void UpdateAODescriptors();
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
        RG::ResourceHandle AddDepthPrepass(RG::RenderGraph& rg, entt::registry& registry, RG::BufferHandle indirectBufferHandle);
        RG::ResourceHandle AddGTAODepthPrefilterPass(RG::RenderGraph& rg, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddGTAOMainPass(RG::RenderGraph& rg, RG::ResourceHandle linearDepth);
        RG::ResourceHandle AddGTAODenoisePass(RG::RenderGraph& rg, RG::ResourceHandle rawAO, RG::ResourceHandle linearDepth);
        RG::ResourceHandle AddShadowPass(RG::RenderGraph& rg, entt::registry& registry, RG::BufferHandle indirectBufferHandle, u32 cascadeIndex);
        GeometryOutput AddGeometryPass(RG::RenderGraph& rg, entt::registry& registry,
                                        const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount],
                                        RG::BufferHandle indirectBufferHandle,
                                        RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddSkyboxPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddBloomPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);
        RG::ResourceHandle AddPostProcessPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle bloomResult);
        SelectionMaskOutput AddSelectionMaskPass(RG::RenderGraph& rg, entt::registry& registry);
        RG::ResourceHandle AddOutlinePass(RG::RenderGraph& rg, RG::ResourceHandle ldrOutput, SelectionMaskOutput maskOutput, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddGridPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);
        void AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);

        void CollectSelectedHandles(const std::vector<Entity>& selected, std::unordered_set<entt::entity>& outHandles) const;

        RG::RenderGraphSnapshot CaptureSnapshot(const RG::RenderGraph& rg);

        friend class FrameDebuggerContext;

        RenderingSystem& m_System;
        std::unique_ptr<FrameDebuggerContext> m_Debugger;

        // ---- Constants (shared with RS-side callers when needed) ----
    public:
        static constexpr u32 k_MaxGPUObjects        = 4096;
        static constexpr u32 k_IndirectRegionCount  = 1 + k_ShadowCascadeCount;
        static constexpr u32 k_IndirectRegionStride = k_MaxGPUObjects;

    private:
        // ---- Global UBO (Set 0) ----
        std::shared_ptr<VKUniformBuffer> m_GlobalUniformBuffer;
        VkDescriptorSetLayout m_GlobalSetLayout     = VK_NULL_HANDLE;
        VkDescriptorSet       m_GlobalDescriptorSet = VK_NULL_HANDLE;

        // ---- Shadow map (4-layer 2D array) + per-layer views ----
        std::shared_ptr<Texture> m_ShadowMap;
        VkImageView              m_ShadowLayerViews[k_ShadowCascadeCount] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkSampler                m_ShadowSampler = VK_NULL_HANDLE;

        // ---- Light UBO + shadow descriptor (Set 3) ----
        std::shared_ptr<VKUniformBuffer> m_LightUniformBuffer;
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

        // ---- GPU Object + Indirect Buffers (persistent, CPU_TO_GPU) ----
        VkBuffer      m_ObjectSSBO         = VK_NULL_HANDLE;
        VmaAllocation m_ObjectSSBOAlloc    = nullptr;
        void*         m_ObjectSSBOMapped   = nullptr;
        VkBuffer      m_IndirectBuffer        = VK_NULL_HANDLE;
        VmaAllocation m_IndirectBufferAlloc   = nullptr;
        void*         m_IndirectBufferMapped  = nullptr;
        u32           m_GPUObjectCount        = 0;

        // ---- Set 5 — GPUObjectData SSBO descriptor (graphics pipeline) ----
        VkDescriptorPool      m_ObjectSSBODescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_ObjectSSBODescLayout = VK_NULL_HANDLE;
        VkDescriptorSet       m_ObjectSSBODescSet    = VK_NULL_HANDLE;

        // ---- Entity → SSBO index (rebuilt every frame in BuildGPUObjectBuffer) ----
        std::unordered_map<entt::entity, u32> m_EntityToSSBOIndex;
        std::vector<entt::entity>             m_EntityLookup;

        // ---- Cull compute pipeline + descriptor ----
        std::unique_ptr<VKComputePipeline> m_CullPipeline;
        VkDescriptorSetLayout              m_CullDescLayout = VK_NULL_HANDLE;
        VkDescriptorSet                    m_CullDescSet    = VK_NULL_HANDLE;

        // ---- GTAO (epic #58) ----
        std::shared_ptr<Texture>           m_GTAOLinearDepth;
        std::shared_ptr<Texture>           m_GTAORawAO;
        std::shared_ptr<Texture>           m_GTAOEdges;
        std::shared_ptr<Texture>           m_GTAOFinal;
        std::unique_ptr<VKComputePipeline> m_GTAOPrefilterPipeline;
        std::unique_ptr<VKComputePipeline> m_GTAOMainPipeline;
        std::unique_ptr<VKComputePipeline> m_GTAODenoisePipeline;
        std::shared_ptr<VKUniformBuffer>   m_GTAOUBOBuffer;
        VkSampler                          m_GTAOSampler    = VK_NULL_HANDLE;
        VkDescriptorPool                   m_GTAODescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_GTAOPrefilterDescLayout = VK_NULL_HANDLE;
        VkDescriptorSet                    m_GTAOPrefilterDescSet    = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_GTAOMainDescLayout      = VK_NULL_HANDLE;
        VkDescriptorSet                    m_GTAOMainDescSet         = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_GTAODenoiseDescLayout   = VK_NULL_HANDLE;
        VkDescriptorSet                    m_GTAODenoiseDescSet      = VK_NULL_HANDLE;
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

        // ---- Post-process UBO / sampler / descriptors ----
        std::shared_ptr<VKUniformBuffer> m_PostProcessUBOBuffer;
        VkSampler              m_PPSampler          = VK_NULL_HANDLE;
        VkDescriptorPool       m_PPDescPool         = VK_NULL_HANDLE;
        VkDescriptorSetLayout  m_PPDescSetLayout    = VK_NULL_HANDLE;
        VkDescriptorSet        m_BloomExtractDescSet = VK_NULL_HANDLE;
        VkDescriptorSet        m_BloomBlurHDescSet   = VK_NULL_HANDLE;
        VkDescriptorSet        m_BloomBlurVDescSet   = VK_NULL_HANDLE;
        VkDescriptorSet        m_CompositeDescSet    = VK_NULL_HANDLE;

        // ---- Bloom textures (half-res RGBA16F, persistent) ----
        std::shared_ptr<Texture> m_BloomA;
        std::shared_ptr<Texture> m_BloomB;

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

        // ---- Outline pass resources ----
        std::unique_ptr<VKPipeline> m_OutlinePipeline;
        std::vector<u32>            m_OutlineFragSpv;
        VkDescriptorPool            m_OutlineDescPool      = VK_NULL_HANDLE;
        VkDescriptorSetLayout       m_OutlineDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet             m_OutlineDescSet       = VK_NULL_HANDLE;
        VkSampler                   m_OutlineSampler       = VK_NULL_HANDLE;

        // ---- Grid pass resources ----
        std::unique_ptr<VKPipeline> m_GridPipeline;
        std::vector<u32>            m_GridFragSpv;
        VkDescriptorPool            m_GridDescPool      = VK_NULL_HANDLE;
        VkDescriptorSetLayout       m_GridDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet             m_GridDescSet       = VK_NULL_HANDLE;
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
