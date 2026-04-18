#pragma once

#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/core/LuthTypes.h"

#include <array>
#include <vulkan/vulkan.h>

namespace Luth
{
    struct FrameDebugger;

    void AddCullComputePass(
        RG::RenderGraph&                rg,
        RG::BufferHandle                objectBuffer,
        RG::BufferHandle                indirectBuffer,
        VKComputePipeline*              pipeline,
        VkDescriptorSet                 descSet,
        const std::array<Vec4, 6>& frustumPlanes,
        u32                             objectCount,
        u32                             destOffset = 0,           // command index offset into indirect buffer
        const char*                     passName   = "FrustumCull",
        FrameDebugger*                  debugger   = nullptr);
}
