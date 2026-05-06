#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"

#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace Luth
{
    class Entity;
    class FrameTargets;
    class RenderPipeline;
    struct ViewResources;
    struct SelectionMaskOutput;

    // Owns the editor-only overlay resources and passes: SelectionMask
    // (rigid + skinned graphics pipelines on Sets 0-4), Outline (fullscreen
    // pass with its own descriptor set), Grid (fullscreen pass with its own
    // descriptor set + GlobalUBO at binding 0).
    // invariant: Init() must precede BuildPipelines(geoLayouts) — Selection
    // pipelines consume the shared 6-layout vector (only Sets 0-4 in practice).
    class EditorOverlaysSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void BuildPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv,
                              const std::vector<VkDescriptorSetLayout>& geoLayouts);

        void WriteOutlineView(ViewResources& vr, FrameTargets& targets);
        void WriteGridView(ViewResources& vr, FrameTargets& targets);

        SelectionMaskOutput AddSelectionMaskPass(RG::RenderGraph& rg);
        RG::ResourceHandle  AddOutlinePass(RG::RenderGraph& rg, RG::ResourceHandle ldrOutput,
                                            SelectionMaskOutput maskOutput, RG::ResourceHandle sceneDepth);
        RG::ResourceHandle  AddGridPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth);

        // Recursively collect entity handles + descendants for the selection mask.
        void CollectSelectedHandles(const std::vector<Entity>& selected, std::unordered_set<entt::entity>& outHandles) const;

        VkDescriptorSetLayout GetOutlineLayout() const { return m_OutlineDescSetLayout; }
        VkDescriptorSetLayout GetGridLayout()    const { return m_GridDescSetLayout; }

    private:
        void CreateLayouts();
        void BuildSelectionPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts);
        void BuildOutlinePipeline();
        void BuildGridPipeline();

        RenderPipeline* m_Pipeline = nullptr;

        // Selection mask (5-set graphics, push-constant per draw).
        std::unique_ptr<VKPipeline> m_SelectionMaskPipeline;
        std::unique_ptr<VKPipeline> m_SelectionMaskSkinnedPipeline;
        std::vector<u32>            m_SelectionMaskVertSpv;
        std::vector<u32>            m_SelectionMaskFragSpv;
        std::vector<u32>            m_SelectionMaskSkinnedVertSpv;

        // Outline (fullscreen, alpha-blended).
        std::unique_ptr<VKPipeline> m_OutlinePipeline;
        std::vector<u32>            m_OutlineFragSpv;
        VkDescriptorSetLayout       m_OutlineDescSetLayout = VK_NULL_HANDLE;
        VkSampler                   m_OutlineSampler       = VK_NULL_HANDLE;

        // Grid (fullscreen, alpha-blended).
        std::unique_ptr<VKPipeline> m_GridPipeline;
        std::vector<u32>            m_GridFragSpv;
        VkDescriptorSetLayout       m_GridDescSetLayout = VK_NULL_HANDLE;
        VkSampler                   m_GridDepthSampler  = VK_NULL_HANDLE;

        // Own copy of fullscreen.vert (PostProcess loads it too — both via idempotent
        // ShaderLibrary::LoadEngine, then OnShaderReloaded refreshes both subsystems' copies).
        std::vector<u32> m_FullscreenVertSpv;
    };
}
