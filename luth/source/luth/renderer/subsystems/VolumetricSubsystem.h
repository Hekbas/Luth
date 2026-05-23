#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"

#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace Luth
{
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

        // Stable per-view writes for the inject pass: image bindings to the view's volDensity +
        // volInScatter atlases. Same write replicated across all MAX_FRAMES_IN_FLIGHT slots until
        // temporal ping-pong arrives — the slot then differentiates which atlas is "current".
        void WriteInjectView(ViewResources& vr);

        // Compute pass: per-voxel dir-light in-scatter into volDensity / volInScatter.
        // Async-compute eligible. Dispatched against the 160x90x128 atlas grid.
        void AddInjectPass(RG::RenderGraph& rg);

        VkSampler                   GetSampler()             const { return m_Sampler; }
        const Memory::GPUSubRegion& GetLastFogVolumeRegion() const { return m_LastFogVolumeRegion; }
        VkDescriptorSetLayout       GetInjectLayout()        const { return m_InjectDescLayout; }

    private:
        RenderPipeline*      m_Pipeline = nullptr;
        VkSampler            m_Sampler  = VK_NULL_HANDLE;
        Memory::GPUSubRegion m_LastFogVolumeRegion{};

        // Inject pass — first volumetric pipeline on the wire. Two storage images written per voxel.
        VkDescriptorSetLayout              m_InjectDescLayout = VK_NULL_HANDLE;
        std::unique_ptr<VKComputePipeline> m_InjectPipeline;
        std::vector<u32>                   m_InjectSpv;
    };
}
