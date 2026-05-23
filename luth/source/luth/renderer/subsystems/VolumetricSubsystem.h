#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
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

        // Stable per-view writes for the inject pass — bindings that don't change across frames:
        // b0/b1 (atlas storage images), b6 (shadow sampler). The per-frame SSBO bindings are
        // rewritten each frame via WriteInjectPerFrame.
        void WriteInjectView(ViewResources& vr);

        // Per-frame rewrites for the inject pass's per-view set, indexed by the active frame slot.
        // SSBO regions are tagged-heap allocations that differ each frame; b2 (Light), b3 (Cluster
        // grid), b4 (Light index), b5 (FogVolume) get refreshed against this frame's regions.
        void WriteInjectPerFrame(const Memory::GPUSubRegion& lightSSBORegion,
                                 const Memory::GPUSubRegion& clusterGridRegion,
                                 const Memory::GPUSubRegion& lightIndexRegion,
                                 const Memory::GPUSubRegion& fogVolumeRegion);

        // Compute pass: per-voxel dir-light + cluster point-light injection with CSM shadow and
        // local FogVolume modulation. Async-compute eligible. Dispatched against the 160x90x128
        // atlas grid.
        void AddInjectPass(RG::RenderGraph& rg);

        // Stable per-view writes for the integrate pass — b0 (density read), b1 (inScatter R/W).
        void WriteIntegrateView(ViewResources& vr);

        // Compute pass: walks froxel columns front-to-back, accumulating transmittance + in-scatter.
        // Reads volDensity, writes volInScatter in-place. Async-compute eligible.
        void AddIntegratePass(RG::RenderGraph& rg);

        // Stable per-view writes for the composite pass — b0 (sceneDepth sampler), b1 (volInScatter
        // sampler3D). Single descriptor set, not cycled — bindings don't change across frames.
        void WriteCompositeView(ViewResources& vr, FrameTargets& targets);

        // Graphics pass: blends fog-modulated radiance back into sceneColor via standard alpha blend.
        // Reads sceneColor (via blend), sceneDepth (sampler), volInScatter atlas (sampler3D), and
        // the Global UBO. Writes to sceneColor in-place.
        RG::ResourceHandle AddCompositePass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor,
                                            RG::ResourceHandle sceneDepth);

        VkSampler                   GetSampler()             const { return m_Sampler; }
        const Memory::GPUSubRegion& GetLastFogVolumeRegion() const { return m_LastFogVolumeRegion; }
        VkDescriptorSetLayout       GetInjectLayout()        const { return m_InjectDescLayout; }
        VkDescriptorSetLayout       GetIntegrateLayout()     const { return m_IntegrateDescLayout; }
        VkDescriptorSetLayout       GetCompositeLayout()     const { return m_CompositeDescLayout; }

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
    };
}
