#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/lighting/IBLPrecompute.h"

namespace Luth
{
    void RenderPipeline::InitIBLResources(const fs::path& hdrPath)
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Run precomputation (equirect -> cubemap -> irradiance -> prefilter -> BRDF LUT)
        IBLResult ibl = IBL::Precompute(hdrPath);

        m_IrradianceMap  = ibl.irradianceMap;
        m_PrefilteredMap = ibl.prefilteredMap;
        m_BRDFLut        = ibl.brdfLut;
        m_IBLSampler     = ibl.iblSampler;
        m_SkyboxVB       = ibl.skyboxVB;
        m_SkyboxVertSpv  = std::move(ibl.skyboxVertSpv);
        m_SkyboxFragSpv  = std::move(ibl.skyboxFragSpv);

        // Write IBL descriptors to Set 0 (bindings 1-3)
        {
            auto vkIrr = std::static_pointer_cast<VKTexture>(m_IrradianceMap);
            auto vkPf  = std::static_pointer_cast<VKTexture>(m_PrefilteredMap);
            auto vkLut = std::static_pointer_cast<VKTexture>(m_BRDFLut);

            VkDescriptorImageInfo irrInfo{};
            irrInfo.sampler = m_IBLSampler;
            irrInfo.imageView = vkIrr->GetImageView();
            irrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo pfInfo{};
            pfInfo.sampler = m_IBLSampler;
            pfInfo.imageView = vkPf->GetImageView();
            pfInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo lutInfo{};
            lutInfo.sampler = m_IBLSampler;
            lutInfo.imageView = vkLut->GetImageView();
            lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3] = {};
            writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[0].dstSet = m_GlobalDescriptorSet;
            writes[0].dstBinding = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &irrInfo;

            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[1].dstSet = m_GlobalDescriptorSet;
            writes[1].dstBinding = 2;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &pfInfo;

            writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[2].dstSet = m_GlobalDescriptorSet;
            writes[2].dstBinding = 3;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].descriptorCount = 1;
            writes[2].pImageInfo = &lutInfo;

            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }
    }

    void RenderPipeline::ReloadSkybox(const fs::path& hdrPath)
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        vkDeviceWaitIdle(device);

        // Destroy old IBL sampler (textures freed by shared_ptr reset in InitIBLResources)
        if (m_IBLSampler) {
            vkDestroySampler(device, m_IBLSampler, nullptr);
            m_IBLSampler = VK_NULL_HANDLE;
        }

        InitIBLResources(hdrPath);

        // Rebuild skybox pipeline (new prefiltered map may have different mip count)
        m_SkyboxPipeline.reset();
        CreatePipelines();

        LH_CORE_INFO("Skybox reloaded from '{}'", hdrPath.string());
    }
}
