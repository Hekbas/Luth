#pragma once

#include "luth/renderer/Texture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"

#include <vulkan/vulkan.h>
#include <filesystem>
#include <memory>
#include <vector>

namespace Luth
{
    struct IBLResult
    {
        std::shared_ptr<Texture>        irradianceMap;
        std::shared_ptr<Texture>        prefilteredMap;
        std::shared_ptr<Texture>        brdfLut;
        VkSampler                       iblSampler = VK_NULL_HANDLE;

        // Skybox resources (compiled alongside IBL)
        std::shared_ptr<VKVertexBuffer> skyboxVB;
        std::vector<u32>                skyboxVertSpv;
        std::vector<u32>                skyboxFragSpv;
    };

    namespace IBL
    {
        // Run IBL precomputation from an HDR equirectangular environment map.
        // Returns fallback 1x1 textures if hdrPath does not exist.
        // Caller is responsible for writing the IBL descriptors to Set 0.
        IBLResult Precompute(const std::filesystem::path& hdrPath);
    }
}
