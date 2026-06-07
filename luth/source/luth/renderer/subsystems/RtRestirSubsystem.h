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
    // Owns 4 compute pipelines (initial RIS+visibility, temporal reuse, spatial reuse, demodulated
    // shade) + the pass-local descriptor layout. Reservoir ping-pong pair + spatial-output buffer +
    // DI image are per-view (allocated in ViewResources). rayQuery-in-compute reads the TLAS via
    // Set 0 binding 6. see arch/rendering-pipeline.md
    class RtRestirSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Stable per-view Set 2 writes: b0 depth, b1 slimNormal, b3 DI image, b5 motion. b2/b4
        // (curr/prev reservoirs) swap each frame — written by WriteReservoirBindings, not here.
        void WriteView(ViewResources& vr, FrameTargets& targets);

        // Per-frame reservoir ping-pong: curr→b2, prev→b4 in the active slot, parity-selected by
        // frameAbs & 1u. b2/b4 are UPDATE_AFTER_BIND so the rewrite is race-safe against in-flight
        // slots. Must run before AddPasses each frame.
        void WriteReservoirBindings(ViewResources& vr);

        // Initial RIS + visibility, temporal reuse, spatial reuse, then demodulated shade. Returns
        // the DI image handle (demodulated diffuse irradiance) consumed by GeometryPass. No-op handle
        // when disabled / no TLAS. slimMotion feeds the temporal pass's reprojection.
        RG::ResourceHandle AddPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneDepth,
                                     RG::ResourceHandle slimNormal, RG::ResourceHandle slimMotion);

        VkSampler             GetSampler()   const { return m_Sampler; }
        VkDescriptorSetLayout GetSetLayout() const { return m_SetLayout; }

        // Per-view persistent reservoir buffer tag — Garlic large-tagged, freed only on resize.
        // Reserved high range, disjoint from the per-frame FreeTag(N-2) sweep.
        u32 NextReservoirTag() { return m_NextTag++; }

        // Backed by RestirSettings::enabled on the RenderingSystem (the editor toggles the setting).
        // GlobalSubsystem reads IsEnabled() to gate restirParams.x — keep it pointing at the setting
        // so the consumption flag tracks the same source AddPasses tests. Out-of-line: needs the
        // RenderingSystem definition, which can't be pulled into this header (RenderPipeline cycle).
        bool IsEnabled() const;
        void SetEnabled(bool e);

    private:
        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_InitialPipeline;
        std::unique_ptr<VKComputePipeline> m_TemporalPipeline;
        std::unique_ptr<VKComputePipeline> m_SpatialPipeline;
        std::unique_ptr<VKComputePipeline> m_ShadePipeline;

        VkSampler             m_Sampler   = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;   // Set 2 (pass-local)

        std::vector<u32> m_InitialSpv;
        std::vector<u32> m_TemporalSpv;
        std::vector<u32> m_SpatialSpv;
        std::vector<u32> m_ShadeSpv;

        u32  m_NextTag = 0xFFFF0000u;  // reserved range for persistent reservoir allocations
    };
}
