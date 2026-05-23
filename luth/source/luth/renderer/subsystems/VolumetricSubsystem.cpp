#include "luthpch.h"
#include "luth/renderer/subsystems/VolumetricSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"

namespace Luth
{
    void VolumetricSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear-clamp sampler shared by the volumetric pipelines. 3D VKTexture ctor returns a
        // null sampler so each consumer subsystem owns the sampler that matches its sampling
        // needs — Wronski wants linear-clamp on the 3D atlas, distinct from the anisotropic
        // mip-aware sampler the bindless 2D path emits.
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_Sampler);
    }

    void VolumetricSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        if (m_Sampler) vkDestroySampler(device, m_Sampler, nullptr);
        m_Sampler = VK_NULL_HANDLE;
    }

    bool VolumetricSubsystem::OnShaderReloaded(const std::string& /*name*/, const std::vector<u32>& /*spv*/)
    {
        // No pipelines yet — each lands with its first-use commit. Reload requests for
        // volumetric_*.{comp,frag} no-op here until those commits add their handlers.
        return false;
    }
}
