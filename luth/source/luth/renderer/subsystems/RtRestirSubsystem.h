#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"

#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class FrameTargets;
    class RenderPipeline;
    struct ViewResources;

    // ReSTIR DI (Bitterli 2020) — spatiotemporal reservoir resampling for the point lights.
    // Owns 2 compute pipelines (initial RIS+visibility, demodulated shade) + the pass-local
    // descriptor layout. Reservoir buffer + DI image are per-view (allocated in ViewResources).
    // rayQuery-in-compute reads the TLAS via Set 0 binding 6. see arch/rendering-pipeline.md
    class RtRestirSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Stable per-view writes: Set 2 = depth + slimNormal samplers, reservoir SSBO, DI image.
        void WriteView(ViewResources& vr, FrameTargets& targets);

        // Initial RIS + visibility, then demodulated shade. Returns the DI image handle (demodulated
        // diffuse irradiance) consumed by GeometryPass. No-op handle when disabled / no TLAS.
        RG::ResourceHandle AddPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneDepth, RG::ResourceHandle slimNormal);

        VkSampler             GetSampler()   const { return m_Sampler; }
        VkDescriptorSetLayout GetSetLayout() const { return m_SetLayout; }

        // Per-view persistent reservoir buffer tag — Garlic large-tagged, freed only on resize.
        // Reserved high range, disjoint from the per-frame FreeTag(N-2) sweep.
        u32 NextReservoirTag() { return m_NextTag++; }

        bool IsEnabled() const { return m_Enabled; }
        void SetEnabled(bool e) { m_Enabled = e; }

    private:
        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_InitialPipeline;
        std::unique_ptr<VKComputePipeline> m_ShadePipeline;

        VkSampler             m_Sampler   = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;   // Set 2 (pass-local)

        std::vector<u32> m_InitialSpv;
        std::vector<u32> m_ShadeSpv;

        u32  m_NextTag = 0xFFFF0000u;  // reserved range for persistent reservoir allocations
        bool m_Enabled = true;
    };
}
