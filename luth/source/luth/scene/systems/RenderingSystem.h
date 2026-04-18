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

#include <memory>
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/renderer/rendergraph/FrameCapture.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/GPUTimerPool.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/pipeline/PipelineManager.h"
#include "luth/renderer/lighting/LightTypes.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/core/UUID.h"
#include "luth/resources/FileWatcher.h"

#include <entt/entt.hpp>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <mutex>

namespace Luth
{
    struct GlobalUniforms {
        glm::mat4 viewProjection;
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec3 cameraPos;
        float     time;
        glm::mat4 lightSpaceMatrix[k_ShadowCascadeCount]; // Per-cascade light-space matrices (Phase 13)
        glm::vec4 cascadeSplitsViewZ;                      // Far view-space depth per cascade
        glm::vec4 shadowBias;                              // Per-cascade depth bias (negative = shadows disabled)
        glm::vec4 shadowNormalBias;                        // Per-cascade normal bias (in shadow-map texels)
        glm::vec4 cascadeTexelSize;                        // Per-cascade world-space size of one shadow texel
        float     iblIntensity;
        float     skyboxIntensity;
        float     debugVisualizeCascades;                  // 0 = off, 1 = tint by cascade
        float     cascadeBlendWidth;                       // fraction of slice depth used for cross-cascade blend (0–1)
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

    class RenderPipeline;

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

        const RG::RenderGraphSnapshot& GetGraphSnapshot() const { return m_GraphSnapshot; }
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
        void ExitCapture()
        {
            // Free GPU-owned archives BEFORE clearing the metadata vectors.
            m_FrameDebugger.DestroyArchives();
            m_FrameDebugger.state = DebuggerState::Inactive;
            m_FrameDebugger.capturedFrame.Clear();
            // Phase 14E — drop the per-draw replay cache key; the preview
            // texture itself is reused on next capture.
            m_PerDrawPreviewKey = UINT64_MAX;
        }
        DebuggerState GetDebuggerState() const { return m_FrameDebugger.state; }
        const RG::CapturedFrame& GetCapturedFrame() const { return m_FrameDebugger.capturedFrame; }
        // Phase 14C — SetDebuggerDrawLimit / GetDebuggerDrawLimit removed; live
        // re-replay is gone, per-draw stepping arrives in Phase 14E.

        // Phase 14D — sampler shared with ImGui for archive previews. Allocated
        // lazily by InitDebugBlitResources (called when capture starts).
        VkSampler GetDebugSampler() const { return m_FrameDebugger.sampler; }

        // Phase 14E — per-draw replay-then-copy.
        //
        // Re-records the captured pass up to (and including) localDrawIdx using
        // the live UBOs/SSBOs/indirect buffer (these stay byte-stable in Frozen
        // since the live render path doesn't touch them; recapture refreshes
        // them when the camera moves). Result is copied into a persistent
        // RGBA16F preview the panel samples through ImGui.
        //
        // Cache is keyed by ((passIdx<<32)|localDrawIdx) and short-circuits
        // identical re-selections so dragging across the tree doesn't issue
        // redundant ImmediateSubmits.
        //
        // v1 supports GeometryPass only; other passes silently leave the
        // preview key unchanged so the panel falls back to the pass-output
        // archive (Phase 14D behavior).
        void ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx);
        VkImageView GetPerDrawPreviewView() const { return m_PerDrawPreviewView; }
        u64         GetPerDrawPreviewKey()  const { return m_PerDrawPreviewKey; }
        u32         GetPerDrawPreviewWidth()  const { return m_PerDrawPreviewWidth; }
        u32         GetPerDrawPreviewHeight() const { return m_PerDrawPreviewHeight; }

        // Phase 14F — depth-archive visualization.
        //
        // Linearizes a depth archive (single layer if `layer >= 0`, else whole
        // image — only single-layer depth makes sense in v1) into the
        // persistent RGBA8 m_DepthPreviewImage via the existing depth blit
        // pipeline. `near`/`far` pick the depth range that maps to [0..1].
        // Cache is keyed by ((u64)archiveIdx<<32)|(u32 layer + 1) so layer -1
        // is distinguishable from layer 0.
        void BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ);
        VkImageView GetDepthPreviewView()   const { return m_DepthPreviewView; }
        u32         GetDepthPreviewWidth()  const { return m_DepthPreviewWidth; }
        u32         GetDepthPreviewHeight() const { return m_DepthPreviewHeight; }

    private:
        void InitGlobalUniforms();
        void InitObjectSSBODescriptorLayout();
        void InitGPUObjectBuffers();
        void InitCullPipeline();
        void InitAOResources();         // GTAO persistent textures + pipelines (epic #58)
        void UpdateAODescriptors();     // Rewrite GTAO descriptor sets (post-Resize)
        void UpdateGTAOUBO();           // Push GTAOSettings → GPU UBO each frame
        void InitShadowResources();
        void InitPostProcessResources();
        void InitIBLResources(const std::filesystem::path& hdrPath);
        void UpdateGlobalUniforms();
        void UpdateLightUniforms(Scene* scene);

        void UpdatePostProcessUBO();
        void UpdatePostProcessDescriptors();
        void CreatePipelines();
        u32  EnsureMaterialRegistered(std::shared_ptr<Material> material);
        void BuildGPUObjectBuffer(entt::registry& registry);

        void RegisterNamedTextures();

        // Phase 14C — RenderCapturedFrame removed (live re-replay).
        // AddDebugBlitPass + InitDebugBlitResources kept for Phase 14D
        // depth->color preview blits.
        RG::ResourceHandle AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth);
        void InitDebugBlitResources();

        // Phase 14E — per-draw preview helpers
        void EnsurePerDrawPreviewTexture(u32 width, u32 height);
        void DestroyPerDrawPreviewTexture();

        // Phase 14F — depth preview helpers
        void EnsureDepthPreviewTexture(u32 width, u32 height);
        void DestroyDepthPreviewTexture();

        // Camera / editor state set each frame by App
        CameraParams m_CameraParams;

        // Memory
        std::unique_ptr<Memory::LinearAllocator> m_FrameAllocator;

        // Persistent viewport-sized render targets (scene color, depth, LDR,
        // entity-ID, selection mask/depth). Allocated in ctor, resized via Resize().
        FrameTargets m_Targets;

        // Entity ID → entity lookup (populated by BuildGPUObjectBuffer; index 0 = entt::null)
        std::vector<entt::entity> m_EntityLookup;

        // Global UBO (Set 0)
        std::shared_ptr<VKUniformBuffer> m_GlobalUniformBuffer;
        VkDescriptorSetLayout m_GlobalSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet m_GlobalDescriptorSet = VK_NULL_HANDLE;

        // Per-frame light gathering + CSM cascade fit. LightGatherer fills
        // LightUniforms + m_ShadowParams from the ECS; CascadeBuilder consumes
        // m_ShadowParams + CameraParams and fills m_Cascades. UpdateGlobalUniforms
        // copies both into the Global UBO.
        LightGatherer                 m_LightGatherer;
        CascadeBuilder                m_CascadeBuilder;
        DirectionalLightShadowParams  m_ShadowParams;
        CascadeData                   m_Cascades;

        // Shadow map (4-layer 2D array) + cached per-layer views for ShadowPass.Ci attachments
        std::shared_ptr<Texture> m_ShadowMap;
        VkImageView              m_ShadowLayerViews[k_ShadowCascadeCount] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkSampler                m_ShadowSampler = VK_NULL_HANDLE;

        // Light UBO + shadow descriptor (Set 3)
        std::shared_ptr<VKUniformBuffer> m_LightUniformBuffer;
        VkDescriptorPool      m_LightDescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_LightSetLayout  = VK_NULL_HANDLE;
        VkDescriptorSet       m_LightDescSet    = VK_NULL_HANDLE;

        // Shadow pipeline (depth-only)
        std::unique_ptr<VKPipeline> m_ShadowPipeline;
        std::unique_ptr<VKPipeline> m_ShadowSkinnedPipeline;
        std::vector<u32>            m_ShadowVertSpv;
        std::vector<u32>            m_ShadowFragSpv;
        std::vector<u32>            m_ShadowSkinnedVertSpv;

        // Depth prepass pipeline (depth-only, camera-space). Shares shadowDepth.frag
        // (empty main) as the null fragment stage; only the vertex shader differs.
        std::unique_ptr<VKPipeline> m_DepthPrepassPipeline;
        std::unique_ptr<VKPipeline> m_DepthPrepassSkinnedPipeline;
        std::vector<u32>            m_DepthPrepassVertSpv;
        std::vector<u32>            m_DepthPrepassSkinnedVertSpv;

        // PBR Pipeline Manager (keyed by {shaderUUID, renderMode})
        PipelineManager m_GeoPipelineManager;
        PipelineManager m_GeoSkinnedPipelineManager;
        std::vector<u32> m_PBRVertSpv;
        std::vector<u32> m_PBRFragSpv;
        std::vector<u32> m_PBRSkinnedVertSpv;

        // Bone blocks now owned by Animation component (per-entity)

        // Material SSBO slot tracking (MaterialUUID -> SSBO index)
        std::unordered_map<UUID, u32, UUIDHash> m_MaterialSlotMap;

        // GPU Object + Indirect Buffers (persistent, CPU_TO_GPU, pre-allocated)
        static constexpr u32 k_MaxGPUObjects        = 4096;
        // Indirect buffer is partitioned into 5 regions: camera + 4 cascades.
        // Each region is k_IndirectRegionStride commands; total = k_IndirectRegionCount * stride.
        static constexpr u32 k_IndirectRegionCount  = 1 + k_ShadowCascadeCount;
        static constexpr u32 k_IndirectRegionStride = k_MaxGPUObjects;
        VkBuffer      m_ObjectSSBO         = VK_NULL_HANDLE;
        VmaAllocation m_ObjectSSBOAlloc    = nullptr;
        void*         m_ObjectSSBOMapped   = nullptr;
        VkBuffer      m_IndirectBuffer        = VK_NULL_HANDLE;
        VmaAllocation m_IndirectBufferAlloc   = nullptr;
        void*         m_IndirectBufferMapped  = nullptr;
        u32           m_GPUObjectCount        = 0;

        // Set 5 — GPUObjectData SSBO descriptor (graphics pipeline)
        VkDescriptorPool      m_ObjectSSBODescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_ObjectSSBODescLayout = VK_NULL_HANDLE;
        VkDescriptorSet       m_ObjectSSBODescSet    = VK_NULL_HANDLE;

        // Entity → SSBO index (0-based), rebuilt every frame in BuildGPUObjectBuffer
        std::unordered_map<entt::entity, u32> m_EntityToSSBOIndex;

        // Cull compute pipeline + descriptor
        std::unique_ptr<VKComputePipeline> m_CullPipeline;
        VkDescriptorSetLayout              m_CullDescLayout = VK_NULL_HANDLE;
        VkDescriptorSet                    m_CullDescSet    = VK_NULL_HANDLE;

        // ---- GTAO (epic #58) ----
        // Persistent half-res linear-depth texture (R32_SFLOAT, storage-image usage).
        // Filled by the prefilter compute pass; sampled by gtao_main.comp.
        std::shared_ptr<Texture>           m_GTAOLinearDepth;
        // Persistent half-res raw AO + edges (filled by main pass, consumed by denoise).
        std::shared_ptr<Texture>           m_GTAORawAO;        // R8
        std::shared_ptr<Texture>           m_GTAOEdges;        // R8
        // Final half-res denoised AO sampled by pbr.frag.
        std::shared_ptr<Texture>           m_GTAOFinal;        // R8
        // Compute pipelines (one per GTAO stage).
        std::unique_ptr<VKComputePipeline> m_GTAOPrefilterPipeline;
        std::unique_ptr<VKComputePipeline> m_GTAOMainPipeline;
        std::unique_ptr<VKComputePipeline> m_GTAODenoisePipeline;
        // GPU-side settings UBO (std140 GTAOUBO) — pushed each frame from m_PostProcessSettings.gtao.
        std::shared_ptr<VKUniformBuffer>   m_GTAOUBOBuffer;
        // Shared sampler for GTAO compute reads (linear clamp, no mips).
        VkSampler                          m_GTAOSampler    = VK_NULL_HANDLE;
        // Descriptor layouts / pools / sets — one pool owns all GTAO sets so a
        // single Resize can free + re-allocate them together.
        VkDescriptorPool                   m_GTAODescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_GTAOPrefilterDescLayout = VK_NULL_HANDLE;
        VkDescriptorSet                    m_GTAOPrefilterDescSet    = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_GTAOMainDescLayout      = VK_NULL_HANDLE;
        VkDescriptorSet                    m_GTAOMainDescSet         = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_GTAODenoiseDescLayout   = VK_NULL_HANDLE;
        VkDescriptorSet                    m_GTAODenoiseDescSet      = VK_NULL_HANDLE;
        // Compiled SPIR-V kept around for hot-reload rebuild.
        std::vector<u32>                   m_GTAOPrefilterSpv;
        std::vector<u32>                   m_GTAOMainSpv;
        std::vector<u32>                   m_GTAODenoiseSpv;

        // Cached view-projection (for frustum extraction — populated in UpdateGlobalUniforms)
        glm::mat4 m_CachedViewProj = glm::mat4(1.0f);

        // Per-frame draw list (RenderMode-sorted opaque/cutout/transparent buckets +
        // tri-count summary). Built by m_DrawListBuilder before pass dispatch; vectors
        // are reused across frames (Clear() just resets sizes).
        DrawListBuilder m_DrawListBuilder;
        DrawList        m_DrawList;

        // Per-frame render-graph assembly + execution (sub-task D of arch-renderer-split).
        // Created in ctor; invoked once per frame from Update() after all per-frame
        // CPU work (UBO upload, GPU object buffer, draw list build) has run.
        std::unique_ptr<RenderPipeline> m_Pipeline;

        // Post-process settings & UBO
        PostProcessSettings m_PostProcessSettings;
        std::shared_ptr<VKUniformBuffer> m_PostProcessUBOBuffer;

        // Bloom textures (half-res RGBA16F, persistent)
        std::shared_ptr<Texture> m_BloomA;
        std::shared_ptr<Texture> m_BloomB;

        // Post-process sampler + descriptors
        VkSampler              m_PPSampler = VK_NULL_HANDLE;
        VkDescriptorPool       m_PPDescPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout  m_PPDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet        m_BloomExtractDescSet = VK_NULL_HANDLE;
        VkDescriptorSet        m_BloomBlurHDescSet = VK_NULL_HANDLE;
        VkDescriptorSet        m_BloomBlurVDescSet = VK_NULL_HANDLE;
        VkDescriptorSet        m_CompositeDescSet = VK_NULL_HANDLE;

        // Post-process pipelines
        std::unique_ptr<VKPipeline> m_BloomExtractPipeline;
        std::unique_ptr<VKPipeline> m_BloomBlurPipeline;
        std::unique_ptr<VKPipeline> m_PostProcessPipeline;

        // Post-process shader SPIR-V
        std::vector<u32> m_FullscreenVertSpv;
        std::vector<u32> m_BloomExtractFragSpv;
        std::vector<u32> m_BloomBlurFragSpv;
        std::vector<u32> m_PostProcessFragSpv;

        // Selection mask pass resources (mask + depth owned by m_Targets)
        std::unique_ptr<VKPipeline>     m_SelectionMaskPipeline;
        std::unique_ptr<VKPipeline>     m_SelectionMaskSkinnedPipeline;
        std::vector<u32>                m_SelectionMaskVertSpv;
        std::vector<u32>                m_SelectionMaskFragSpv;
        std::vector<u32>                m_SelectionMaskSkinnedVertSpv;

        // Outline pass resources
        std::unique_ptr<VKPipeline>  m_OutlinePipeline;
        std::vector<u32>             m_OutlineFragSpv;
        VkDescriptorPool             m_OutlineDescPool      = VK_NULL_HANDLE;
        VkDescriptorSetLayout        m_OutlineDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet              m_OutlineDescSet       = VK_NULL_HANDLE;
        VkSampler                    m_OutlineSampler       = VK_NULL_HANDLE;
        glm::vec4                    m_OutlineColor         = { 1.0f, 0.6f, 0.0f, 1.0f };

        // Grid pass resources (editor-only infinite grid)
        std::unique_ptr<VKPipeline>  m_GridPipeline;
        std::vector<u32>             m_GridFragSpv;
        VkDescriptorPool             m_GridDescPool      = VK_NULL_HANDLE;
        VkDescriptorSetLayout        m_GridDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet              m_GridDescSet       = VK_NULL_HANDLE;
        VkSampler                    m_GridDepthSampler  = VK_NULL_HANDLE;
        bool                         m_GridVisible       = true;

        // IBL resources
        std::shared_ptr<Texture> m_IrradianceMap;    // 32x32 cubemap, RGBA16F
        std::shared_ptr<Texture> m_PrefilteredMap;    // 128x128 cubemap, RGBA16F, 5 mips
        std::shared_ptr<Texture> m_BRDFLut;           // 512x512 2D, RG16F
        VkSampler m_IBLSampler = VK_NULL_HANDLE;

        // Skybox resources
        std::unique_ptr<VKPipeline> m_SkyboxPipeline;
        std::shared_ptr<VKVertexBuffer> m_SkyboxVB;
        std::vector<u32> m_SkyboxVertSpv;
        std::vector<u32> m_SkyboxFragSpv;

        // Shader hot-reload
        FileWatcher m_ShaderWatcher;
        fs::path m_WatchedProjectShaderDir;  // Empty when no project shader dir is watched.
        std::mutex m_ReloadMutex;
        std::set<std::string> m_PendingReloads;
        bool m_PendingUtilityReload = false;

        void RecompileUtilityShaders();

        // Stats (tri count lives on m_DrawList.visibleTriCount)
        ShadeMode m_ShadeMode = ShadeMode::Lit;

        // Mouse picking state
        bool m_PickPending = false;
        bool m_PickResultReady = false;
        glm::ivec2 m_PickCoord = { 0, 0 };
        entt::entity m_PickedEntity = entt::null;

        // Frame debugger data
        RG::RenderGraphSnapshot m_GraphSnapshot;
        GPUTimerPool m_GPUTimers;
        std::unordered_map<std::string, std::shared_ptr<Texture>> m_NamedTextures;

        // Frame debugger
        FrameDebugger m_FrameDebugger;

        // Phase 14E — per-draw preview (RGBA16F mirror of SceneColor at draw N).
        // Allocated lazily on first ReplayPassUpToDraw and resized whenever
        // the scene color target's dimensions change. Persistent across captures.
        VkImage         m_PerDrawPreviewImage  = VK_NULL_HANDLE;
        VkImageView     m_PerDrawPreviewView   = VK_NULL_HANDLE;
        VmaAllocation   m_PerDrawPreviewAlloc  = nullptr;
        u32             m_PerDrawPreviewWidth  = 0;
        u32             m_PerDrawPreviewHeight = 0;
        // Cache key — ((u64)passIdx << 32) | (u64)localDrawIdx, or UINT64_MAX
        // when no replay has been issued yet (or after invalidation).
        u64             m_PerDrawPreviewKey    = UINT64_MAX;

        // Phase 14F — RGBA8 depth-linearized preview for cascade slices and
        // any other depth archive. Sized to the cascade resolution by default
        // (k_ShadowResolution × k_ShadowResolution) but can be re-allocated.
        VkImage         m_DepthPreviewImage  = VK_NULL_HANDLE;
        VkImageView     m_DepthPreviewView   = VK_NULL_HANDLE;
        VmaAllocation   m_DepthPreviewAlloc  = nullptr;
        u32             m_DepthPreviewWidth  = 0;
        u32             m_DepthPreviewHeight = 0;
        // Cache key — ((u64)archiveIdx << 32) | (u32)(layer + 1), so layer
        // values of -1 / 0 are distinguishable. UINT64_MAX = invalid.
        u64             m_DepthPreviewKey    = UINT64_MAX;
    };

}
