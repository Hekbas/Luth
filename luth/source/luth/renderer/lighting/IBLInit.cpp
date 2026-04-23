#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/lighting/IBLPrecompute.h"

namespace Luth
{
    void RenderPipeline::InitIBLResources(const fs::path& hdrPath)
    {
        // Run precomputation (equirect -> cubemap -> irradiance -> prefilter -> BRDF LUT)
        IBLResult ibl = IBL::Precompute(hdrPath);

        m_IrradianceMap  = ibl.irradianceMap;
        m_PrefilteredMap = ibl.prefilteredMap;
        m_BRDFLut        = ibl.brdfLut;
        m_IBLSampler     = ibl.iblSampler;
        m_SkyboxVB       = ibl.skyboxVB;
        m_SkyboxVertSpv  = std::move(ibl.skyboxVertSpv);
        m_SkyboxFragSpv  = std::move(ibl.skyboxFragSpv);

        // IBL bindings (Set 0 binding 1-3) live on the per-view global
        // descriptor set; rewrite every cached view so ReloadSkybox picks
        // up the new textures without forcing viewers to re-create their
        // ViewResources entry. Empty on first-time init — views created
        // afterwards will pick up these pointers via WriteViewGlobalSet.
        for (auto& [targets, vr] : m_ViewResources)
            WriteViewGlobalSet(vr);
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
