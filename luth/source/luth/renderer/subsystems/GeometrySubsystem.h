#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/UUID.h"
#include "luth/core/FrameData.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/lighting/LightTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/pipeline/PipelineManager.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

#include <entt/entt.hpp>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Luth
{
    class Material;
    class RenderPipeline;
    struct RenderSnapshot;
    struct GeometryOutput;
    struct SlimGBufferOutput;

    // Owns Set 5 (per-draw GPU object SSBO + indirect args), the cull compute pipeline, the PBR + depth-prepass
    // graphics pipelines, the per-frame entity<->SSBO mapping, and the geometry-side render-graph passes
    // (cull / depth-prepass / forward PBR).
    // invariant: Init() must precede BuildPipelines(geoLayouts); the Set 5 layout is created in Init and
    // consumed by the shared geoLayouts vector.
    class GeometrySubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void BuildPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv,
                              const std::vector<VkDescriptorSetLayout>& geoLayouts);

        // Per-render-stage: rebuild Object + Indirect regions in the GPU tagged heap and rewrite Set 5 + cull descriptors.
        void BuildGPUObjectBuffer(const RenderSnapshot& snapshot);
        u32  EnsureMaterialRegistered(std::shared_ptr<Material> material);

        // Perf observability: meshes skipped last build because the per-frame GPU object cap
        // (k_MaxGPUObjects) was hit (a silent draw-drop). >0 means raise the cap or add culling/LOD.
        static u32 GetDroppedObjectCount();

        // Render-graph contributions.
        void AddCullPass(RG::RenderGraph& rg,
                         RG::BufferHandle objectBuffer, RG::BufferHandle indirectBuffer,
                         const std::array<Vec4, 6>& frustumPlanes, u32 destOffset,
                         const char* passName);
        RG::ResourceHandle AddDepthPrepass(RG::RenderGraph& rg, RG::BufferHandle indirectBufferHandle);
        SlimGBufferOutput  AddSlimGBufferPass(RG::RenderGraph& rg,
                                              RG::BufferHandle indirectBufferHandle,
                                              RG::ResourceHandle sceneDepth);
        GeometryOutput     AddGeometryPass(RG::RenderGraph& rg,
                                           const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount],
                                           RG::BufferHandle indirectBufferHandle,
                                           RG::ResourceHandle sceneDepth,
                                           RG::ResourceHandle gtaoFinalAO,
                                           RG::ResourceHandle rtShadowMask,
                                           RG::ResourceHandle diHandle,
                                           RG::ResourceHandle giDIHandle,
                                           RG::ResourceHandle reflHandle,
                                           RG::ResourceHandle diSpecHandle);

        // ---- Accessors ----
        VkDescriptorSetLayout       GetSet5Layout()         const { return m_ObjectSSBODescLayout; }
        VkDescriptorSet             GetObjectSSBODescSet(u32 slot) const { return m_ObjectSSBODescSet[slot]; }
        const Memory::GPUSubRegion& GetObjectRegion()       const { return m_ObjectRegion; }
        const Memory::GPUSubRegion& GetIndirectRegion()     const { return m_IndirectRegion; }
        u32                         GetGPUObjectCount()     const { return m_GPUObjectCount; }

        const std::unordered_map<UUID, u32, UUIDHash>& GetMaterialSlotMap() const { return m_MaterialSlotMap; }
        const std::unordered_map<entt::entity, u32>&   GetEntityToSSBOIndex() const { return m_EntityToSSBOIndex; }
        const std::vector<entt::entity>&               GetEntityLookup() const { return m_EntityLookup; }

        PipelineManager&        GetGeoPipelineManager()        { return m_GeoPipelineManager; }
        PipelineManager&        GetGeoSkinnedPipelineManager() { return m_GeoSkinnedPipelineManager; }
        const std::vector<u32>& GetPBRVertSpv()        const { return m_PBRVertSpv; }
        const std::vector<u32>& GetPBRFragSpv()        const { return m_PBRFragSpv; }
        const std::vector<u32>& GetPBRSkinnedVertSpv() const { return m_PBRSkinnedVertSpv; }
        VKPipeline*             GetDepthPrepassPipeline()        const { return m_DepthPrepassPipeline.get(); }
        VKPipeline*             GetDepthPrepassSkinnedPipeline() const { return m_DepthPrepassSkinnedPipeline.get(); }
        VKPipeline*             GetSlimGBufferPipeline()         const { return m_SlimGBufferPipeline.get(); }
        VKPipeline*             GetSlimGBufferSkinnedPipeline()  const { return m_SlimGBufferSkinnedPipeline.get(); }

    private:
        void InitObjectSSBO();
        void InitCullPipeline();
        void BuildPBRPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);
        void BuildDepthPrepassPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);
        void BuildSlimGBufferPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);

        // Maps a node-graph material's fragment-shader UUID to its SPIR-V (cached). Invalid -> the stock
        // m_PBRFragSpv; an unresolved UUID also falls back to stock so a not-yet-loaded graph never stalls.
        const std::vector<u32>& ResolveFragSpv(const UUID& fragShaderUUID);

        RenderPipeline* m_Pipeline = nullptr;

        // Set 5: GPUObjectData SSBO descriptor (graphics).
        VkDescriptorPool      m_ObjectSSBODescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_ObjectSSBODescLayout = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_ObjectSSBODescSet{};

        // Per-render-stage regions (allocated each frame from tagged heap).
        Memory::GPUSubRegion m_ObjectRegion{};
        Memory::GPUSubRegion m_IndirectRegion{};
        u32                  m_GPUObjectCount = 0;

        // Material registry + entity->SSBO index (rebuilt each render stage).
        std::unordered_map<UUID, u32, UUIDHash>   m_MaterialSlotMap;
        std::unordered_map<entt::entity, u32>     m_EntityToSSBOIndex;
        std::vector<entt::entity>                 m_EntityLookup;

        // Frame N-1's worldMatrix per entity (motion vectors). Rebuilt by atomic-replace at end of
        // BuildGPUObjectBuffer: despawned entities drop, newly-spawned fall back to current model on
        // next frame. Render-side state; gameplay never touches it.
        std::unordered_map<entt::entity, Mat4>    m_PrevModelByEntity;

        // Cull compute pipeline + descriptor.
        std::unique_ptr<VKComputePipeline> m_CullPipeline;
        VkDescriptorPool                   m_CullDescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_CullDescLayout = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_CullDescSet{};

        // Depth-prepass pipelines + SPV.
        std::unique_ptr<VKPipeline> m_DepthPrepassPipeline;
        std::unique_ptr<VKPipeline> m_DepthPrepassSkinnedPipeline;
        std::vector<u32>            m_DepthPrepassVertSpv;
        std::vector<u32>            m_DepthPrepassSkinnedVertSpv;

        // Slim G-buffer pipelines + SPV. Opaque: depth-EQUAL against prepass depth, no depth write.
        // Cutout: shares the shaders but tests LESS_OR_EQUAL + writes its own depth (the opaque-only
        // prepass omits cutout) + alpha-tests in slim_gbuffer.slang, so RT shadows/reflections + GTAO
        // reconstruct from the holed cutout surface, not the geometry behind it. Full PBR vtx stride.
        std::unique_ptr<VKPipeline> m_SlimGBufferPipeline;
        std::unique_ptr<VKPipeline> m_SlimGBufferSkinnedPipeline;
        std::unique_ptr<VKPipeline> m_SlimGBufferCutoutPipeline;
        std::unique_ptr<VKPipeline> m_SlimGBufferCutoutSkinnedPipeline;
        std::vector<u32>            m_SlimGBufferVertSpv;
        std::vector<u32>            m_SlimGBufferSkinnedVertSpv;
        std::vector<u32>            m_SlimGBufferFragSpv;

        // PBR pipeline managers (shader-mode-keyed; lazy build).
        PipelineManager  m_GeoPipelineManager;
        PipelineManager  m_GeoSkinnedPipelineManager;
        std::vector<u32> m_PBRVertSpv;
        std::vector<u32> m_PBRFragSpv;
        std::vector<u32> m_PBRSkinnedVertSpv;

        // Shaded-wireframe overlay: flat line-polygon pipelines redrawn over the lit fill (static + skinned).
        std::unique_ptr<VKPipeline> m_WireframeOverlayPipeline;
        std::unique_ptr<VKPipeline> m_WireframeOverlaySkinnedPipeline;
        std::vector<u32>            m_WireframeOverlayFragSpv;

        // Per-material node-graph fragment SPIR-V, keyed by the material's graph-shader UUID. Populated
        // lazily from ShaderLibrary in ResolveFragSpv; the stock pbr fragment is never stored here.
        std::unordered_map<UUID, std::vector<u32>, UUIDHash> m_GraphFragSpv;

        // Materials whose graph has been lowered + compiled this run (once-guard for the lazy codegen
        // trigger in EnsureMaterialRegistered). An editor edit clears a material's entry to re-emit.
        std::unordered_set<UUID, UUIDHash> m_GraphCompiled;
    };
}
