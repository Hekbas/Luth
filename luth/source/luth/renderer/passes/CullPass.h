#pragma once

#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/core/LuthTypes.h"

#include <array>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace Luth
{
    void AddCullComputePass(
        RG::RenderGraph&                rg,
        RG::BufferHandle                objectBuffer,
        RG::BufferHandle                indirectBuffer,
        VKComputePipeline*              pipeline,
        VkDescriptorSet                 descSet,
        const std::array<glm::vec4, 6>& frustumPlanes,
        u32                             objectCount);
}
