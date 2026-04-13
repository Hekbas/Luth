#pragma once

#include "luth/scene/System.h"
#include "luth/scene/Entity.h"
#include "luth/memory/Memory.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/renderer/rendergraph/FrameCapture.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
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
    struct GlobalUniforms {
        glm::mat4 viewProjection;
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec3 cameraPos;
        float     time;
        glm::mat4 lightSpaceMatrix;  // Set by UpdateLightUniforms before upload
        float     shadowBias;
        float     iblIntensity;
        float     skyboxIntensity;
        float     _pad;
    };

    enum class ShadeMode : u8 { Lit = 0, Unlit, Wireframe, Normals, EntityID };

    struct ObjectPushConstants {
        glm::mat4 modelMatrix;  // 64 bytes
        u32 materialIndex;      // 4 bytes — index into material SSBO
        u32 shadeMode;          // 4 bytes
        u32 entityID;           // 4 bytes — entity index for picking
        u32 boneOffset;          // 4 bytes — base index into BoneMatrices SSBO (0 for static)
    };

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

    struct DrawCommand {
        glm::mat4 modelMatrix;
        u32 materialSlot;
        std::shared_ptr<Model> model;
        u32 meshIndex;
        u32 entityIndex = 0;
        Material::CullMode cullMode = Material::CullMode::Back;
        bool isSkinned = false;
        u32 boneOffset = 0;
    };

    enum class DebuggerState : u8 { Inactive, CaptureRequested, Frozen };

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

        // Frame debugger capture
        void RequestCapture()   { if (m_DebuggerState == DebuggerState::Inactive) m_DebuggerState = DebuggerState::CaptureRequested; }
        void ExitCapture()      { m_DebuggerState = DebuggerState::Inactive; m_CapturedFrame.Clear(); }
        DebuggerState GetDebuggerState() const { return m_DebuggerState; }
        const RG::CapturedFrame& GetCapturedFrame() const { return m_CapturedFrame; }
        void SetDebuggerDrawLimit(u32 limit) { m_DebuggerDrawLimit = limit; }
        u32  GetDebuggerDrawLimit() const    { return m_DebuggerDrawLimit; }

    private:
        void InitGlobalUniforms();
        void InitShadowResources();
        void InitPostProcessResources();
        void InitIBLResources(const std::filesystem::path& hdrPath);
        void UpdateGlobalUniforms();
        void UpdateLightUniforms(Scene* scene);
        void UpdatePostProcessUBO();
        void UpdatePostProcessDescriptors();
        void CreatePipelines();
        u32  EnsureMaterialRegistered(std::shared_ptr<Material> material);

        RG::RenderGraphSnapshot CaptureSnapshot(const RG::RenderGraph& rg);
        void RegisterNamedTextures();

        RG::ResourceHandle AddShadowPass(RG::RenderGraph& rg, entt::registry& registry);
        GeometryOutput AddGeometryPass(RG::RenderGraph& rg, entt::registry& registry, RG::ResourceHandle shadowMapHandle);
        RG::ResourceHandle AddSkyboxPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddBloomPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);
        RG::ResourceHandle AddPostProcessPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle bloomResult);
        SelectionMaskOutput AddSelectionMaskPass(RG::RenderGraph& rg, entt::registry& registry);
        RG::ResourceHandle AddOutlinePass(RG::RenderGraph& rg, RG::ResourceHandle ldrOutput, SelectionMaskOutput maskOutput, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle AddGridPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);
        void AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);

        void CollectSelectedHandles(const std::vector<Entity>& selected, std::unordered_set<entt::entity>& outHandles) const;

        // Frame debugger re-recording
        void RenderCapturedFrame(u32 maxDrawCalls, Scene* scene);
        RG::ResourceHandle AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth);
        void InitDebugBlitResources();

        // Capture helpers (called during normal recording when CaptureRequested)
        void BeginCapturePass(const std::string& name, const std::string& activeTarget, bool isDepth, const RG::CapturedPipelineState& ps);
        void EndCapturePass();
        void CaptureDrawCall(const std::string& passName, const std::string& meshName, const std::string& entityName,
                             u32 entityIndex, u32 indexCount, const ObjectPushConstants& pc, const RG::CapturedPipelineState& ps);

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

        // Light space matrix (computed in UpdateLightUniforms, uploaded in UpdateGlobalUniforms)
        glm::mat4 m_CachedLightSpaceMatrix = glm::mat4(1.0f);
        float     m_CachedShadowBias = 0.005f;
        bool      m_CachedCastShadows = true;
        float     m_CachedShadowOrtho = 200.0f;
        float     m_CachedShadowDist  = 200.0f;

        // Shadow map
        std::shared_ptr<Texture> m_ShadowMap;
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

        // PBR Pipeline Manager (keyed by {shaderUUID, renderMode})
        PipelineManager m_GeoPipelineManager;
        PipelineManager m_GeoSkinnedPipelineManager;
        std::vector<u32> m_PBRVertSpv;
        std::vector<u32> m_PBRFragSpv;
        std::vector<u32> m_PBRSkinnedVertSpv;

        // Bone blocks now owned by Animation component (per-entity)

        // Material SSBO slot tracking (MaterialUUID -> SSBO index)
        std::unordered_map<UUID, u32, UUIDHash> m_MaterialSlotMap;

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

        // Frame debugger capture state
        DebuggerState      m_DebuggerState     = DebuggerState::Inactive;
        RG::CapturedFrame  m_CapturedFrame;
        u32                m_DebuggerDrawLimit  = UINT32_MAX;
        u32                m_ReplayDrawCounter  = 0;

        // Captured draw commands for re-recording (copied at freeze time)
        std::vector<DrawCommand> m_CapturedOpaqueDraws;
        std::vector<DrawCommand> m_CapturedCutoutDraws;
        std::vector<DrawCommand> m_CapturedTransparentDraws;

        // Debug blit resources (rescue blit for truncated frames)
        std::unique_ptr<VKPipeline>  m_DebugBlitPipeline;
        std::unique_ptr<VKPipeline>  m_DebugDepthPipeline;
        std::vector<u32>             m_DebugBlitFragSpv;
        std::vector<u32>             m_DebugDepthFragSpv;
        VkDescriptorPool             m_DebugBlitDescPool      = VK_NULL_HANDLE;
        VkDescriptorSetLayout        m_DebugBlitDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet              m_DebugBlitDescSet       = VK_NULL_HANDLE;
        VkSampler                    m_DebugBlitSampler       = VK_NULL_HANDLE;
    };

}
