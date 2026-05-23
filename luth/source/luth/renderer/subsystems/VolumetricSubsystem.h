#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace Luth
{
    class RenderPipeline;
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

        VkSampler                   GetSampler()             const { return m_Sampler; }
        const Memory::GPUSubRegion& GetLastFogVolumeRegion() const { return m_LastFogVolumeRegion; }

    private:
        RenderPipeline*      m_Pipeline = nullptr;
        VkSampler            m_Sampler  = VK_NULL_HANDLE;
        Memory::GPUSubRegion m_LastFogVolumeRegion{};
    };
}
