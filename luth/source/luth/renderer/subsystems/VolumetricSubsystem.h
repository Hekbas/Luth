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

    // Wronski frustum voxel volumetric fog. Four-pass chain:
    //   Inject     — pre-integrate per-voxel scatter (dir-light + cluster points + FogVolume mod).
    //   Integrate  — front-to-back Beer-Lambert ray march; writes accumulated transmittance + in-
    //                scatter in-place over volInScatter scratch.
    //   Resolve    — temporal reprojection + Karis 3x3x3 clamp + history blend. Reads scratch +
    //                prev-frame's resolved (parity ping-pong over HistA/B); writes this frame's
    //                resolved. Decouples temporal from inject so history domain matches resolve
    //                output (post-integrate), avoiding the energy-non-conservation that an
    //                inject-time temporal blend produced.
    //   Composite  — graphics fullscreen blend onto sceneColor. Samples the resolved atlas.
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

        // Allocates a FogVolume SSBO region from GPUTaggedPageAllocator and copies the gathered
        // header + flexible array. Returns the region; caches m_LastFogVolumeRegion for the
        // injection pass binding. Returns an empty region when no JobContext (off the fiber path)
        // or when allocation fails.
        Memory::GPUSubRegion UploadFogVolumeSSBO(const GatheredFogVolumes& volumes);

        // Stable per-view writes for the inject pass — b0 (density storage), b1 (in-scatter
        // storage), b6 (shadow array sampler). The SSBO bindings b2-b5 rewrite per-frame.
        void WriteInjectView(ViewResources& vr);

        // Per-frame rewrites for the inject set's SSBO bindings (Light, ClusterGrid, LightIndex,
        // FogVolume) against this frame's tagged-heap regions. b0/b1/b6 are stable (WriteInjectView).
        void WriteInjectPerFrame(const Memory::GPUSubRegion& lightSSBORegion,
                                 const Memory::GPUSubRegion& clusterGridRegion,
                                 const Memory::GPUSubRegion& lightIndexRegion,
                                 const Memory::GPUSubRegion& fogVolumeRegion);

        // Inject pass output — atlas handle threaded into integrate so both passes share the same
        // ResourceNode per arch hazard #1.
        struct InjectOutputs
        {
            RG::ResourceHandle density;
            RG::ResourceHandle inScatter;
        };

        // Compute pass: per-voxel scatter (Hillaire HG phase) with sun light-path absorption ray-
        // march + IBL ambient. Async-compute eligible. Takes per-cascade shadow handles so RG knows
        // to transition them to SHADER_READ_ONLY before sampling (binding 6).
        InjectOutputs AddInjectPass(RG::RenderGraph& rg,
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

        VkSampler                   GetSampler()             const { return m_Sampler; }
        const Memory::GPUSubRegion& GetLastFogVolumeRegion() const { return m_LastFogVolumeRegion; }
        VkDescriptorSetLayout       GetInjectLayout()        const { return m_InjectDescLayout; }
        VkDescriptorSetLayout       GetIntegrateLayout()     const { return m_IntegrateDescLayout; }
        VkDescriptorSetLayout       GetResolveLayout()       const { return m_ResolveDescLayout; }
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

        // Integrate pass — front-to-back ray march in-place over volInScatter scratch.
        VkDescriptorSetLayout              m_IntegrateDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKComputePipeline> m_IntegratePipeline;
        std::vector<u32>                   m_IntegrateSpv;

        // Resolve pass — temporal accumulation, scratch + prev → curr history.
        VkDescriptorSetLayout              m_ResolveDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKComputePipeline> m_ResolvePipeline;
        std::vector<u32>                   m_ResolveSpv;

        // Composite pass — fullscreen graphics; samples sceneDepth + resolved atlas, alpha-blends.
        VkDescriptorSetLayout              m_CompositeDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKPipeline>        m_CompositePipeline;
        std::vector<u32>                   m_FullscreenVertSpv;
        std::vector<u32>                   m_CompositeFragSpv;

        // Debug viz pass — heat-mapped density or in-scatter overlay onto LDR.
        VkDescriptorSetLayout              m_VizDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKPipeline>        m_VizPipeline;
        std::vector<u32>                   m_VizFragSpv;

        // Static 3D Worley-FBM noise texture — single shared instance (NOT per-view) used by the
        // inject pass to modulate density. Baked once at Init via a one-shot compute dispatch.
        // 128³ RGBA8 = 8 MB. Sampler is a tile-friendly LINEAR+REPEAT (m_Sampler is CLAMP_TO_EDGE
        // so we own a dedicated one).
        std::shared_ptr<Texture>           m_NoiseTexture;
        VkSampler                          m_NoiseSampler = VK_NULL_HANDLE;
    };
}
