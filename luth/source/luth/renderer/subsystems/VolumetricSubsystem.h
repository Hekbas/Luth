#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/renderer/lighting/LightTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"

#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace Luth
{
    class FrameTargets;
    class RenderPipeline;
    struct ViewResources;
    struct GatheredFogVolumes;

    // Wronski frustum voxel volumetric fog. Subsystem skeleton plus a single linear-clamp
    // sampler shared across the inject / integrate / composite pipelines. Each pipeline +
    // descriptor layout lands with its first-use commit (inject A4.7, integrate A4.9,
    // composite A4.13). The 3D atlases live on ViewResources (persistent VMA via 3D VKTexture
    // ctor); the per-frame FogVolume SSBO routes through GPUTaggedPageAllocator, mirroring
    // LightingSubsystem::UploadLightSSBO.
    class VolumetricSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Allocates a FogVolume SSBO region from GPUTaggedPageAllocator and copies the gathered
        // header + flexible array. Returns the region; caches m_LastFogVolumeRegion for the
        // injection pass binding. Returns an empty region when no JobContext (off the fiber path)
        // or when allocation fails.
        Memory::GPUSubRegion UploadFogVolumeSSBO(const GatheredFogVolumes& volumes);

        // Stable per-view writes for the inject pass — only b6 (shadow sampler) is stable now.
        // b0 (density) / b1 (in-scatter write) / b7 (history read) and b2-b5 (SSBOs) all rewrite
        // per frame in WriteInjectPerFrame — b0/b1/b7 parity-pick which physical atlas plays
        // which role this frame for temporal accumulation.
        void WriteInjectView(ViewResources& vr);

        // Per-frame rewrites for the inject set: b0 (density), b1 (current-frame in-scatter
        // write target), b7 (previous-frame in-scatter sampled as history) get parity-picked
        // from `frameAbs & 1`. b2-b5 SSBOs refresh against this frame's tagged-heap regions.
        void WriteInjectPerFrame(const Memory::GPUSubRegion& lightSSBORegion,
                                 const Memory::GPUSubRegion& clusterGridRegion,
                                 const Memory::GPUSubRegion& lightIndexRegion,
                                 const Memory::GPUSubRegion& fogVolumeRegion,
                                 u32 frameAbs);

        // Inject pass output — atlas handles after the storage writes, threaded into integrate so
        // both passes share the same `ResourceNode` per arch hazard #1 (no double ImportResource).
        struct InjectOutputs
        {
            RG::ResourceHandle density;
            RG::ResourceHandle inScatter;
        };

        // Compute pass: per-voxel dir-light + cluster point-light injection with CSM shadow and
        // local FogVolume modulation. Async-compute eligible. Dispatched against the 160x90x128
        // atlas grid. Takes per-cascade shadow handles so RG knows to transition the shadow array
        // layers from DSA → SHADER_READ_ONLY before sampling (shader binding 6).
        InjectOutputs AddInjectPass(RG::RenderGraph& rg,
                                    const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount]);

        // Stable per-view writes for the integrate pass — only b0 (density sampler) is stable.
        // b1 (in-scatter write target) parity-cycles between volInScatter and volInScatterHistory;
        // rewritten in WriteIntegratePerFrame against the same atlas inject wrote to this frame.
        void WriteIntegrateView(ViewResources& vr);

        // Per-frame rewrite of integrate b1 — matches inject's parity-chosen write target.
        void WriteIntegratePerFrame(ViewResources& vr, u32 frameAbs);

        // Compute pass: walks froxel columns front-to-back, accumulating transmittance + in-scatter.
        // Reads volDensity, writes volInScatter in-place. Async-compute eligible. Returns the
        // post-write inScatter handle so composite can declare its sampler read.
        RG::ResourceHandle AddIntegratePass(RG::RenderGraph& rg, InjectOutputs injectOut);

        // Stable per-view writes for the composite pass — only b0 (sceneDepth sampler) is stable.
        // b1 (volInScatter sampler3D) parity-cycles to whichever atlas integrate wrote to this
        // frame; rewritten in WriteCompositePerFrame. Composite descriptor set is cycled across
        // MAX_FRAMES_IN_FLIGHT to keep rewrites disjoint from in-flight reads.
        void WriteCompositeView(ViewResources& vr, FrameTargets& targets);

        // Per-frame rewrite of composite b1 — samples the atlas that was integrated this frame.
        void WriteCompositePerFrame(ViewResources& vr, FrameTargets& targets, u32 frameAbs);

        // Stable per-view write of the viz descriptor — only b0 (sceneDepth sampler) and b1
        // (volDensity sampler) are stable. b2 (volInScatter sampler) parity-rewrites per frame
        // in WriteVizPerFrame to follow the integrate ping-pong target. Viz set is cycled per
        // MAX_FRAMES_IN_FLIGHT slot like the other volumetric sets.
        void WriteVizView(ViewResources& vr, FrameTargets& targets);
        void WriteVizPerFrame(ViewResources& vr, u32 frameAbs);

        // Debug graphics pass: blits a heat-mapped density or raw in-scatter radiance over LDR.
        // ShadeMode::VolumetricDensity → mode 0; VolumetricInScatter → mode 1. Declares Read on
        // sceneDepth + density + inScatter so RG transitions all three to SHADER_READ_ONLY.
        RG::ResourceHandle AddVizPass(RG::RenderGraph& rg, RG::ResourceHandle ldrInput,
                                      RG::ResourceHandle density, RG::ResourceHandle inScatter,
                                      RG::ResourceHandle sceneDepth, u32 mode);

        // Graphics pass: blends fog-modulated radiance back into sceneColor via standard alpha blend.
        // Reads sceneColor (via blend), sceneDepth (sampler), volInScatter atlas (sampler3D), and
        // the Global UBO. Writes to sceneColor in-place.
        RG::ResourceHandle AddCompositePass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor,
                                            RG::ResourceHandle sceneDepth,
                                            RG::ResourceHandle inScatter);

        VkSampler                   GetSampler()             const { return m_Sampler; }
        const Memory::GPUSubRegion& GetLastFogVolumeRegion() const { return m_LastFogVolumeRegion; }
        VkDescriptorSetLayout       GetInjectLayout()        const { return m_InjectDescLayout; }
        VkDescriptorSetLayout       GetIntegrateLayout()     const { return m_IntegrateDescLayout; }
        VkDescriptorSetLayout       GetCompositeLayout()     const { return m_CompositeDescLayout; }
        VkDescriptorSetLayout       GetVizLayout()           const { return m_VizDescLayout; }

    private:
        RenderPipeline*      m_Pipeline = nullptr;
        VkSampler            m_Sampler  = VK_NULL_HANDLE;
        Memory::GPUSubRegion m_LastFogVolumeRegion{};

        // Inject pass — per-voxel light injection (dir + cluster + FogVolume modulation).
        VkDescriptorSetLayout              m_InjectDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKComputePipeline> m_InjectPipeline;
        std::vector<u32>                   m_InjectSpv;

        // Integrate pass — front-to-back ray march in-place over volInScatter.
        VkDescriptorSetLayout              m_IntegrateDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKComputePipeline> m_IntegratePipeline;
        std::vector<u32>                   m_IntegrateSpv;

        // Composite pass — fullscreen graphics pipeline; samples atlas + depth, blends back into
        // sceneColor via the pipeline's src + dst*src.a blend equation.
        VkDescriptorSetLayout              m_CompositeDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKPipeline>        m_CompositePipeline;
        std::vector<u32>                   m_FullscreenVertSpv;
        std::vector<u32>                   m_CompositeFragSpv;

        // Debug viz pass — heat-mapped density or in-scatter overlay onto LDR.
        VkDescriptorSetLayout              m_VizDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKPipeline>        m_VizPipeline;
        std::vector<u32>                   m_VizFragSpv;
    };
}
