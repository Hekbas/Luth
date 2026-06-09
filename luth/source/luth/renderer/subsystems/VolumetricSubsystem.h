#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/renderer/lighting/LightTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/resources/Texture.h"
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

    // Wronski frustum voxel volumetric fog. Five-pass chain:
    //   InjectDensity — per-voxel density + tint accumulation (FogVolume + analytic distance +
    //                   analytic height + Worley-FBM noise modulation). Writes vec4(density,
    //                   tint.rgb) to volDensity.
    //   InjectScatter — reads volDensity (sampler3D), computes CSM-shadowed dir light + clustered
    //                   point lights + IBL multi-scatter. Sun-ray absorption samples the density
    //                   atlas along the ray (proper density-aware extinction; restores god rays
    //                   through varying fog). Writes vec4(inScatter, 0) to volInScatter scratch.
    //   Integrate     — front-to-back Beer-Lambert ray march; writes accumulated transmittance
    //                   + in-scatter in-place over volInScatter scratch.
    //   Resolve       — temporal reprojection + Karis 3x3x3 clamp + history blend. Reads scratch +
    //                   prev-frame's resolved (parity ping-pong over HistA/B); writes this frame's
    //                   resolved. Decouples temporal from inject so history domain matches resolve
    //                   output (post-integrate), avoiding the energy-non-conservation that an
    //                   inject-time temporal blend produced.
    //   Composite     — graphics fullscreen blend onto sceneColor. Samples the resolved atlas.
    //
    // The viz pass (Vol Density / Vol In-Scatter ShadeMode) samples density + the resolved atlas
    // for diagnostic overlays.
    //
    // Atlases live on ViewResources (persistent VMA). Per-frame FogVolume SSBO routes through
    // GPUTaggedPageAllocator, mirroring LightingSubsystem::UploadLightSSBO.
    class VolumetricSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // RT fog shadows toggle gate (VolumetricSettings::rtShadows). Read by RenderPipeline's needTlas
        // gate + the scatter pass's AS-build→read barrier. Out-of-line: needs the RenderingSystem def.
        bool IsRtShadowsEnabled() const;

        // Allocates a FogVolume SSBO region from GPUTaggedPageAllocator and copies the gathered
        // header + flexible array. Returns the region; caches m_LastFogVolumeRegion for the
        // injection pass binding. Returns an empty region when no JobContext (off the fiber path)
        // or when allocation fails.
        Memory::GPUSubRegion UploadFogVolumeSSBO(const GatheredFogVolumes& volumes);

        // Stable per-view writes for the density pass — b0 (volDensity storage), b2 (noise sampler).
        // b1 (FogVolume SSBO) rewrites per-frame.
        void WriteInjectDensityView(ViewResources& vr);

        // Per-frame rewrite of the density set's FogVolume SSBO (b1) against this frame's tagged-heap region.
        void WriteInjectDensityPerFrame(const Memory::GPUSubRegion& fogVolumeRegion);

        // Stable per-view writes for the scatter pass — b0 (volDensity sampler3D), b1 (volInScatter
        // storage), b5 (shadow array sampler). SSBO bindings b2-b4 rewrite per-frame.
        void WriteInjectScatterView(ViewResources& vr);

        // Per-frame rewrite of the scatter set's SSBO bindings (Light b2, ClusterGrid b3,
        // LightIndex b4). b0/b1/b5 are stable (WriteInjectScatterView).
        void WriteInjectScatterPerFrame(const Memory::GPUSubRegion& lightSSBORegion,
                                        const Memory::GPUSubRegion& clusterGridRegion,
                                        const Memory::GPUSubRegion& lightIndexRegion);

        // Inject pass outputs — handles threaded into downstream passes so RG resolves barriers on
        // the same ResourceNode (hazard #1: no re-import). Scatter reads density via the handle
        // returned by AddInjectDensityPass; integrate consumes both density + scatter.
        struct InjectOutputs
        {
            RG::ResourceHandle density;
            RG::ResourceHandle inScatter;
        };

        // Density pass: per-voxel density + tint accumulation + noise modulation. Async-compute.
        // Returns the volDensity handle (scatter pass reads it, integrate reads it).
        RG::ResourceHandle AddInjectDensityPass(RG::RenderGraph& rg);

        // Scatter pass: reads volDensity (via density handle) and writes volInScatter. Sun-ray
        // absorption samples the density atlas along the ray. Takes per-cascade shadow handles so
        // RG transitions them to SHADER_READ_ONLY before sampling (binding 5). Returns inScatter
        // handle for integrate; InjectOutputs is reassembled by the caller for AddIntegratePass.
        RG::ResourceHandle AddInjectScatterPass(RG::RenderGraph& rg,
                                                RG::ResourceHandle density,
                                                const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount]);

        // Stable per-view writes for the integrate pass — both b0 (density sampler) and b1
        // (in-scatter storage write target = volInScatter scratch) are stable. No per-frame.
        void WriteIntegrateView(ViewResources& vr);

        // Compute pass: front-to-back ray march; reads volDensity + volInScatter (pre-integrate),
        // writes accumulated transmittance + in-scatter back to volInScatter (in-place). Returns
        // post-integrate handle that the resolve pass consumes.
        RG::ResourceHandle AddIntegratePass(RG::RenderGraph& rg, InjectOutputs injectOut);

        // Stable per-view writes for the resolve pass — only b0 (volInScatter scratch sampler) is
        // stable. b1 (prev history sampler) + b2 (curr history storage write target) parity-rewrite
        // per frame to ping-pong over volInScatterHistA / volInScatterHistB.
        void WriteResolveView(ViewResources& vr);

        // Per-frame rewrite of resolve b1 + b2 — parity picks (HistA, HistB) vs (HistB, HistA)
        // for (read prev, write curr).
        void WriteResolvePerFrame(ViewResources& vr, u32 frameAbs);

        // Compute pass: temporal accumulation. Reads scratch (this frame's post-integrate) + prev
        // resolved (reprojected via prevViewProjection), applies Karis 3x3x3 min/max clamp on the
        // 27 scratch neighbors, blends with temporalAlpha, writes curr resolved. Returns curr
        // resolved handle so composite + viz can declare reads.
        RG::ResourceHandle AddResolvePass(RG::RenderGraph& rg, RG::ResourceHandle scratchInScatter);

        // Stable per-view writes for the composite pass — only b0 (sceneDepth sampler) is stable.
        // b1 (resolved sampler3D) parity-cycles HistA / HistB.
        void WriteCompositeView(ViewResources& vr, FrameTargets& targets);

        // Per-frame rewrite of composite b1 — samples this frame's resolved history atlas.
        void WriteCompositePerFrame(ViewResources& vr, FrameTargets& targets, u32 frameAbs);

        // Stable per-view write of the viz descriptor — b0 (sceneDepth), b1 (volDensity) are stable.
        // b2 (resolved in-scatter sampler) parity-rewrites in WriteVizPerFrame.
        void WriteVizView(ViewResources& vr, FrameTargets& targets);
        void WriteVizPerFrame(ViewResources& vr, u32 frameAbs);

        // Debug graphics pass: blits a heat-mapped density or raw in-scatter radiance over LDR.
        // ShadeMode::VolumetricDensity → mode 0; VolumetricInScatter → mode 1.
        RG::ResourceHandle AddVizPass(RG::RenderGraph& rg, RG::ResourceHandle ldrInput,
                                      RG::ResourceHandle density, RG::ResourceHandle inScatter,
                                      RG::ResourceHandle sceneDepth, u32 mode);

        // Graphics pass: blends fog-modulated radiance back into sceneColor via standard alpha blend.
        RG::ResourceHandle AddCompositePass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor,
                                            RG::ResourceHandle sceneDepth,
                                            RG::ResourceHandle resolvedInScatter);

        VkSampler                   GetSampler()              const { return m_Sampler; }
        const Memory::GPUSubRegion& GetLastFogVolumeRegion()  const { return m_LastFogVolumeRegion; }
        VkDescriptorSetLayout       GetInjectDensityLayout()  const { return m_InjectDensityDescLayout; }
        VkDescriptorSetLayout       GetInjectScatterLayout()  const { return m_InjectScatterDescLayout; }
        VkDescriptorSetLayout       GetIntegrateLayout()      const { return m_IntegrateDescLayout; }
        VkDescriptorSetLayout       GetResolveLayout()        const { return m_ResolveDescLayout; }
        VkDescriptorSetLayout       GetCompositeLayout()      const { return m_CompositeDescLayout; }
        VkDescriptorSetLayout       GetVizLayout()            const { return m_VizDescLayout; }

    private:
        RenderPipeline*      m_Pipeline = nullptr;
        VkSampler            m_Sampler  = VK_NULL_HANDLE;
        Memory::GPUSubRegion m_LastFogVolumeRegion{};

        // Inject density.
        VkDescriptorSetLayout              m_InjectDensityDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKComputePipeline> m_InjectDensityPipeline;
        std::vector<u32>                   m_InjectDensitySpv;

        // Inject scatter.
        VkDescriptorSetLayout              m_InjectScatterDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKComputePipeline> m_InjectScatterPipeline;
        std::vector<u32>                   m_InjectScatterSpv;

        // Integrate.
        VkDescriptorSetLayout              m_IntegrateDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKComputePipeline> m_IntegratePipeline;
        std::vector<u32>                   m_IntegrateSpv;

        // Resolve.
        VkDescriptorSetLayout              m_ResolveDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKComputePipeline> m_ResolvePipeline;
        std::vector<u32>                   m_ResolveSpv;

        // Composite.
        VkDescriptorSetLayout              m_CompositeDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKPipeline>        m_CompositePipeline;
        std::vector<u32>                   m_FullscreenVertSpv;
        std::vector<u32>                   m_CompositeFragSpv;

        // Debug viz.
        VkDescriptorSetLayout              m_VizDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKPipeline>        m_VizPipeline;
        std::vector<u32>                   m_VizFragSpv;

        // 3D Worley-FBM noise (128³ RGBA8). Single shared instance, baked once at Init; modulates
        // inject density. Tile-friendly LINEAR+REPEAT sampler (m_Sampler is CLAMP_TO_EDGE).
        std::shared_ptr<Texture>           m_NoiseTexture;
        VkSampler                          m_NoiseSampler = VK_NULL_HANDLE;

        // 2D blue-noise dither (Roberts R2 quasi-random, 64² R8). Composite jitters sliceW ±0.5
        // slices to break Wronski log-Z banding; TAA integrates the dither over ~6 frames into
        // smooth gradients. NEAREST+REPEAT sampler — bilinear destroys the spectral properties.
        std::shared_ptr<Texture>           m_BlueNoise2D;
        VkSampler                          m_BlueNoiseSampler = VK_NULL_HANDLE;
    };
}
