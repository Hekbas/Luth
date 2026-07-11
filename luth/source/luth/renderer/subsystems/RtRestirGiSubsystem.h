#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"

#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class FrameTargets;
    class RenderPipeline;
    struct ViewResources;

    // ReSTIR GI (Ouyang 2021): spatiotemporal reservoir resampling for 1-bounce indirect diffuse.
    // Owns 4 compute pipelines (initial 1-bounce sample, temporal, spatial + final visibility,
    // demodulated shade) + the pass-local Set 2 layout (same 7-binding shape as the DI subsystem).
    // Single scratch reservoir + spatial/history buffer + GI image are per-view (ViewResources);
    // temporal history is the previous frame's SPATIAL output. rayQuery-in-compute (initial +
    // spatial) reads the TLAS via Set 0 binding 6. see arch/rendering-pipeline.md
    class RtRestirGiSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Stable per-view Set 2 writes: b0 depth, b1 slimNormal, b2 scratch reservoir, b3 GI image,
        // b4 history (= the spatial buffer), b5 motion, b6 spatial output (same buffer as b4).
        void WriteView(ViewResources& vr, FrameTargets& targets);

        // Initial 1-bounce path sample, temporal + spatial reuse (spatial ends with the final-visibility
        // trace), then demodulated shade. Returns the GI image handle (demodulated indirect-diffuse
        // irradiance) consumed by GeometryPass. No-op handle when disabled / no TLAS. slimMotion feeds
        // temporal reprojection.
        RG::ResourceHandle AddPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneDepth,
                                     RG::ResourceHandle slimNormal, RG::ResourceHandle slimMotion);

        // Half-res GI bilateral upscale: reads the half-res denoised GI (svgfGiHalf) + full-res depth/normal
        // guides, writes the full-res svgfGiDenoised. Only wired when RestirGiSettings::halfResolution.
        VkDescriptorSetLayout GetUpscaleLayout() const { return m_UpscaleSetLayout; }
        void WriteUpscaleView(ViewResources& vr, FrameTargets& targets);
        RG::ResourceHandle AddUpscalePass(RG::RenderGraph& rg, RG::ResourceHandle giHalf,
                                          RG::ResourceHandle sceneDepth, RG::ResourceHandle slimNormal);

        VkSampler             GetSampler()   const { return m_Sampler; }
        VkDescriptorSetLayout GetSetLayout() const { return m_SetLayout; }

        // Debug-viz (ShadeMode::RestirGiReservoir): a fullscreen graphics pass heat-mapping the spatial
        // reservoir's M (confidence) + age (staleness) over LDR. Its own 1-set layout (b0 depth sampler,
        // b1 spatial-reservoir SSBO (the buffer is CONCURRENT, so the graphics-queue read is sync-safe)).
        VkDescriptorSetLayout GetReservoirVizLayout() const { return m_ReservoirVizSetLayout; }
        void WriteReservoirVizView(ViewResources& vr, FrameTargets& targets);
        RG::ResourceHandle AddReservoirVizPass(RG::RenderGraph& rg, RG::ResourceHandle ldrInput,
                                               RG::ResourceHandle sceneDepth);

        // Per-view persistent reservoir buffer tag: Garlic large-tagged, freed only on resize. Reserved
        // high range DISJOINT from DI's 0xFFFF0000; both subsystems mint into the same heap.
        u32 NextReservoirTag() { return m_NextTag++; }

        // Backed by RestirGiSettings::enabled on the RenderingSystem (the editor toggles the setting).
        // GlobalSubsystem reads IsEnabled() to gate restirParams.y. Out-of-line: needs the
        // RenderingSystem definition, which can't be pulled into this header (RenderPipeline cycle).
        bool IsEnabled() const;
        void SetEnabled(bool e);

    private:
        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_InitialPipeline;
        std::unique_ptr<VKComputePipeline> m_TemporalPipeline;
        std::unique_ptr<VKComputePipeline> m_SpatialPipeline;
        std::unique_ptr<VKComputePipeline> m_ShadePipeline;
        std::unique_ptr<VKComputePipeline> m_UpscalePipeline;   // half-res GI bilateral upscale to full

        VkSampler             m_Sampler          = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_SetLayout        = VK_NULL_HANDLE;   // Set 2 (pass-local)
        VkDescriptorSetLayout m_UpscaleSetLayout = VK_NULL_HANDLE;   // Set 1 for the upscale pass

        std::vector<u32> m_InitialSpv;
        std::vector<u32> m_TemporalSpv;
        std::vector<u32> m_SpatialSpv;
        std::vector<u32> m_ShadeSpv;
        std::vector<u32> m_UpscaleSpv;

        // Reservoir debug-viz graphics pipeline (fullscreen.vert + restir_gi_reservoir_viz.slang).
        std::unique_ptr<VKPipeline> m_ReservoirVizPipeline;
        VkDescriptorSetLayout       m_ReservoirVizSetLayout = VK_NULL_HANDLE;
        std::vector<u32>            m_FullscreenVertSpv;
        std::vector<u32>            m_ReservoirVizFragSpv;

        u32  m_NextTag = 0xFFFF8000u;  // reserved range for persistent reservoir allocations (disjoint from DI)
    };
}
