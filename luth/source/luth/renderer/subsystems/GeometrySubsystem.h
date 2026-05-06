#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/UUID.h"
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
#include <vector>

namespace Luth
{
    class Material;
    class RenderPipeline;
    struct RenderSnapshot;
    struct GeometryOutput;

    // Owns Set 5 (per-draw GPU object SSBO + indirect args), the cull compute
    // pipeline, the PBR + depth-prepass graphics pipelines, the per-frame
    // entity↔SSBO mapping, and the geometry-side render-graph passes
    // (cull / depth-prepass / forward PBR).
    // invariant: Init() must precede BuildPipelines(geoLayouts) — the Set 5
    // layout is created in Init and consumed by the shared geoLayouts vector.
    class GeometrySubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void BuildPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv,
                              const std::vector<VkDescriptorSetLayout>& geoLayouts);

        // Per-render-stage: rebuild Object + Indirect regions in the GPU tagged
        // heap and rewrite Set 5 + cull descriptors.
        void BuildGPUObjectBuffer(const RenderSnapshot& snapshot);
        u32  EnsureMaterialRegistered(std::shared_ptr<Material> material);

        // Render-graph contributions.
        void AddCullPass(RG::RenderGraph& rg,
                         RG::BufferHandle objectBuffer, RG::BufferHandle indirectBuffer,
                         const std::array<Vec4, 6>& frustumPlanes, u32 destOffset,
                         const char* passName);
        RG::ResourceHandle AddDepthPrepass(RG::RenderGraph& rg, RG::BufferHandle indirectBufferHandle);
        GeometryOutput     AddGeometryPass(RG::RenderGraph& rg,
                                           const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount],
                                           RG::BufferHandle indirectBufferHandle,
                                           RG::ResourceHandle sceneDepth,
                                           RG::ResourceHandle gtaoFinalAO);

        // ---- Accessors ----
        VkDescriptorSetLayout       GetSet5Layout()         const { return m_ObjectSSBODescLayout; }
        VkDescriptorSet             GetObjectSSBODescSet()  const { return m_ObjectSSBODescSet; }
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

    private:
        void InitObjectSSBO();
        void InitCullPipeline();
        void BuildPBRPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);
        void BuildDepthPrepassPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);

        RenderPipeline* m_Pipeline = nullptr;

        // Set 5 — GPUObjectData SSBO descriptor (graphics).
        VkDescriptorPool      m_ObjectSSBODescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_ObjectSSBODescLayout = VK_NULL_HANDLE;
        VkDescriptorSet       m_ObjectSSBODescSet    = VK_NULL_HANDLE;

        // Per-render-stage regions (allocated each frame from tagged heap).
        Memory::GPUSubRegion m_ObjectRegion{};
        Memory::GPUSubRegion m_IndirectRegion{};
        u32                  m_GPUObjectCount = 0;

        // Material registry + entity→SSBO index (rebuilt each render stage).
        std::unordered_map<UUID, u32, UUIDHash>   m_MaterialSlotMap;
        std::unordered_map<entt::entity, u32>     m_EntityToSSBOIndex;
        std::vector<entt::entity>                 m_EntityLookup;

        // Cull compute pipeline + descriptor.
        std::unique_ptr<VKComputePipeline> m_CullPipeline;
        VkDescriptorPool                   m_CullDescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout              m_CullDescLayout = VK_NULL_HANDLE;
        VkDescriptorSet                    m_CullDescSet    = VK_NULL_HANDLE;

        // Depth-prepass pipelines + SPV.
        std::unique_ptr<VKPipeline> m_DepthPrepassPipeline;
        std::unique_ptr<VKPipeline> m_DepthPrepassSkinnedPipeline;
        std::vector<u32>            m_DepthPrepassVertSpv;
        std::vector<u32>            m_DepthPrepassSkinnedVertSpv;

        // PBR pipeline managers (shader-mode-keyed; lazy build).
        PipelineManager  m_GeoPipelineManager;
        PipelineManager  m_GeoSkinnedPipelineManager;
        std::vector<u32> m_PBRVertSpv;
        std::vector<u32> m_PBRFragSpv;
        std::vector<u32> m_PBRSkinnedVertSpv;
    };
}
