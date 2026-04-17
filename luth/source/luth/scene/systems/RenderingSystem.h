#pragma once

#include "luth/scene/System.h"
#include "luth/scene/Entity.h"
#include "luth/memory/Memory.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/FrameDebugger.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/renderer/rendergraph/FrameCapture.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/GPUTimerPool.h"
#include "luth/renderer/Texture.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"
#include "luth/renderer/PipelineManager.h"
#include "luth/renderer/PostProcessSettings.h"
#include "luth/core/UUID.h"
#include "luth/resources/FileWatcher.h"

#include <entt/entt.hpp>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <mutex>

namespace Luth
{
    // Number of shadow cascades for directional-light CSM (Phase 13)
    inline constexpr u32 k_ShadowCascadeCount = 4;
    inline constexpr u32 k_ShadowResolution   = 2048;

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

    // ---- Light data structs (mirrored in pbr.frag Set 3) ----

    struct DirectionalLightData {
        glm::vec3 direction;   // 12
        float     intensity;   // 4
        glm::vec3 color;       // 12
        float     _pad;        // 4
    };  // 32 bytes

    struct PointLightData {
        glm::vec3 position;    // 12
        float     range;       // 4
        glm::vec3 color;       // 12
        float     intensity;   // 4
    };  // 32 bytes

    struct LightUniforms {
        DirectionalLightData dirLight;
        PointLightData       pointLights[64];
        int                  numPointLights;
        int                  _pad[3];
    };

    struct GeometryOutput {
        RG::ResourceHandle color;
        RG::ResourceHandle depth;
        RG::ResourceHandle entityID;
    };

    struct SelectionMaskOutput {
        RG::ResourceHandle mask;
        RG::ResourceHandle depth;
    };

    class RenderingSystem : public System
    {
    public:
        RenderingSystem(u32 viewportWidth = 1280, u32 viewportHeight = 720);
        ~RenderingSystem();

        void Update(Scene* scene) override;
        void Resize(u32 width, u32 height);

        // Project lifecycle hooks: extend / restrict the shader hot-reload
        // watcher to cover the active project's shaders directory.
        void OnProjectLoaded();
        void OnProjectUnloaded();

        std::shared_ptr<Texture> GetSceneColor() const { return m_LDROutput ? m_LDROutput : m_SceneColor; }
        PostProcessSettings& GetPostProcessSettings() { return m_PostProcessSettings; }
        const PostProcessSettings& GetPostProcessSettings() const { return m_PostProcessSettings; }

        u64 GetFrameAllocatorUsage() const { return m_FrameAllocator->GetUsedMemory(); }
        u64 GetFrameAllocatorTotal() const { return m_FrameAllocator->GetTotalSize(); }

        const RG::RenderGraphSnapshot& GetGraphSnapshot() const { return m_GraphSnapshot; }
        std::shared_ptr<Texture> GetNamedTexture(const std::string& name) const;

        u32 GetTriangleCount() const { return m_VisibleTriCount; }

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

        // CSM helpers (Phase 13B)
        void ComputeCascadeSplits(float nearZ, float farZ, float lambda, float outFar[k_ShadowCascadeCount]) const;
        // Computes the light-space view-projection matrix for one cascade slice.
        // `outWorldHalfExtent` receives the world-space half-extent of the cascade's
        // ortho footprint (radius for stabilized, max of X/Y half-extents otherwise),
        // which the shader uses to scale per-texel quantities like normal bias.
        glm::mat4 ComputeCascadeMatrix(float nearD, float farD,
                                        const glm::vec3& lightDir,
                                        float tanHalfFovY, float aspect,
                                        const glm::mat4& camViewInv,
                                        bool stabilize,
                                        float& outWorldHalfExtent) const;
        void UpdatePostProcessUBO();
        void UpdatePostProcessDescriptors();
        void CreatePipelines();
        u32  EnsureMaterialRegistered(std::shared_ptr<Material> material);
        void BuildGPUObjectBuffer(entt::registry& registry);

        RG::RenderGraphSnapshot CaptureSnapshot(const RG::RenderGraph& rg);
        void RegisterNamedTextures();

        RG::ResourceHandle AddDepthPrepass(RG::RenderGraph& rg, entt::registry& registry, RG::BufferHandle indirectBufferHandle);
        RG::ResourceHandle AddGTAODepthPrefilterPass(RG::RenderGraph& rg, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddGTAOMainPass(RG::RenderGraph& rg, RG::ResourceHandle linearDepth);
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

        // Scene color + depth output
        std::shared_ptr<Texture> m_SceneColor;
        std::shared_ptr<Texture> m_SceneDepth;

        // Entity ID buffer (R32_UINT, for mouse picking + selection outline)
        std::shared_ptr<Texture> m_EntityIDBuffer;
        std::vector<entt::entity> m_EntityLookup; // index 0 = entt::null

        // Global UBO (Set 0)
        std::shared_ptr<VKUniformBuffer> m_GlobalUniformBuffer;
        VkDescriptorSetLayout m_GlobalSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet m_GlobalDescriptorSet = VK_NULL_HANDLE;

        // Per-cascade light-space matrices (computed in UpdateLightUniforms, uploaded in UpdateGlobalUniforms).
        // In 13A all four entries are identical (single-camera-fit); per-cascade fitting lands in 13B.
        glm::mat4 m_CachedLightSpaceMatrix[k_ShadowCascadeCount] = {
            glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)
        };
        glm::vec4 m_CachedCascadeSplitsViewZ = glm::vec4(0.0f);  // Per-cascade far view-Z (absolute)
        glm::vec4 m_CachedShadowBias       = glm::vec4(0.005f);   // Per-cascade depth bias (negative = disabled)
        glm::vec4 m_CachedShadowNormalBias = glm::vec4(0.0f);     // Per-cascade normal bias (in texels)
        glm::vec4 m_CachedCascadeTexelSize = glm::vec4(1.0f);     // World-space size of one shadow texel per cascade
        float     m_CachedCascadeBlendWidth = 0.2f;               // Cross-cascade blend fraction
        bool      m_CachedDebugVisualizeCascades = false;          // Tint by cascade index
        bool      m_CachedCastShadows = true;

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
        // Compiled SPIR-V kept around for hot-reload rebuild.
        std::vector<u32>                   m_GTAOPrefilterSpv;
        std::vector<u32>                   m_GTAOMainSpv;

        // Cached view-projection (for frustum extraction — populated in UpdateGlobalUniforms)
        glm::mat4 m_CachedViewProj = glm::mat4(1.0f);

        // Draw command buffers (reused across frames to avoid per-frame heap allocation)
        std::vector<DrawCommand> m_OpaqueDraws;
        std::vector<DrawCommand> m_CutoutDraws;
        std::vector<DrawCommand> m_TransparentDraws;

        // Post-process settings & UBO
        PostProcessSettings m_PostProcessSettings;
        std::shared_ptr<VKUniformBuffer> m_PostProcessUBOBuffer;

        // LDR output (post-tonemapped, for ScenePanel display)
        std::shared_ptr<Texture> m_LDROutput;

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

        // Selection mask pass resources
        std::shared_ptr<Texture>        m_SelectionMask;      // RGBA8 — .r = 1.0 for selected
        std::shared_ptr<Texture>        m_SelectionDepth;     // D32_Float — depth of selected geometry
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

        // Stats
        u32 m_VisibleTriCount = 0;
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
        // m_SceneColor's dimensions change. Persistent across captures.
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
