#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/FrameData.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/lighting/LightTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/resources/Texture.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class RenderPipeline;
    namespace fs = std::filesystem;

    // Owns Set 3, shadow map, IBL maps, skybox VB + shadow/skybox pipelines.
    // invariant: Init() must precede BuildPipelines(geoLayouts) — the latter needs Set 5.
    class LightingSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline, const fs::path& hdrPath);
        void BuildPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);
        void Shutdown();

        // Re-bake IBL from a new HDR. Rebuilds the skybox pipeline (prefiltered
        // mip count may change) and rewrites every cached view's Set 0.
        void ReloadSkybox(const fs::path& hdrPath, const std::vector<VkDescriptorSetLayout>& geoLayouts);

        // Hot-reload hook. Returns true if name matched and was handled.
        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv,
                              const std::vector<VkDescriptorSetLayout>& geoLayouts);

        // Per-frame: rebind Set 3 binding 0 to a fresh tagged-heap region.
        void UploadLightUBO(const LightUniforms& lights);

        // Render-graph contributions.
        RG::ResourceHandle AddShadowPass(RG::RenderGraph& rg, RG::BufferHandle indirectBufferHandle, u32 cascadeIndex);
        RG::ResourceHandle AddSkyboxPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);

        // ---- Accessors ----
        VkDescriptorSetLayout GetSetLayout() const          { return m_LightSetLayout; }
        VkDescriptorSet       GetLightDescSet(u32 slot) const { return m_LightDescSet[slot]; }
        VkSampler             GetShadowSampler() const      { return m_ShadowSampler; }
        const std::shared_ptr<Texture>& GetShadowMap() const { return m_ShadowMap; }
        VkImageView           GetShadowLayerView(u32 i) const { return m_ShadowLayerViews[i]; }
        VKPipeline*           GetShadowPipeline() const     { return m_ShadowPipeline.get(); }
        VKPipeline*           GetShadowSkinnedPipeline() const { return m_ShadowSkinnedPipeline.get(); }

        // Depth-prepass + selection pipelines reuse this null-fragment SPV.
        // Temp accessor — folds away once those pipelines move into Geometry/EditorOverlays.
        const std::vector<u32>& GetShadowFragSpv() const { return m_ShadowFragSpv; }

        const std::shared_ptr<Texture>& GetIrradianceMap()  const { return m_IrradianceMap; }
        const std::shared_ptr<Texture>& GetPrefilteredMap() const { return m_PrefilteredMap; }
        const std::shared_ptr<Texture>& GetBRDFLut()        const { return m_BRDFLut; }
        VkSampler                       GetIBLSampler()     const { return m_IBLSampler; }
        bool IsIBLReady() const { return m_IrradianceMap && m_PrefilteredMap && m_BRDFLut && m_IBLSampler; }

    private:
        void CreateShadowResources(VkDevice device);
        void LoadIBL(const fs::path& hdrPath);
        void BuildShadowPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);
        void BuildSkyboxPipeline(const std::vector<VkDescriptorSetLayout>& geoLayouts);

        RenderPipeline* m_Pipeline = nullptr;

        // Set 3 + shadow map.
        std::shared_ptr<Texture> m_ShadowMap;
        VkImageView              m_ShadowLayerViews[k_ShadowCascadeCount] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkSampler                m_ShadowSampler  = VK_NULL_HANDLE;
        VkDescriptorPool         m_LightDescPool  = VK_NULL_HANDLE;
        VkDescriptorSetLayout    m_LightSetLayout = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_LightDescSet{};

        // Shadow pipelines + SPV.
        std::unique_ptr<VKPipeline> m_ShadowPipeline;
        std::unique_ptr<VKPipeline> m_ShadowSkinnedPipeline;
        std::vector<u32>            m_ShadowVertSpv;
        std::vector<u32>            m_ShadowFragSpv;
        std::vector<u32>            m_ShadowSkinnedVertSpv;

        // IBL.
        std::shared_ptr<Texture> m_IrradianceMap;
        std::shared_ptr<Texture> m_PrefilteredMap;
        std::shared_ptr<Texture> m_BRDFLut;
        VkSampler                m_IBLSampler = VK_NULL_HANDLE;

        // Skybox.
        std::unique_ptr<VKPipeline>     m_SkyboxPipeline;
        std::shared_ptr<VKVertexBuffer> m_SkyboxVB;
        std::vector<u32>                m_SkyboxVertSpv;
        std::vector<u32>                m_SkyboxFragSpv;
    };
}
