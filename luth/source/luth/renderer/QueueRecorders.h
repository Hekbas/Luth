#pragma once

#include <vulkan/vulkan.h>

namespace Luth
{
    // The per-view triplet of primary command buffers around the async-compute split.
    // gA  = graphics work before the first AsyncCompute pass in the view's render graph.
    // compute = AsyncCompute passes (recorded on a compute-family command buffer when async compute exists).
    // gB  = graphics work after the first AsyncCompute pass (Geometry / PostProcess / overlays / ImGui).
    // Empty cmd buffers are valid no-op submits; per-view metadata (first/last) drives the submit topology
    // in VulkanBackend::SubmitFrame. See docs/development/arch/multi-queue.md.
    struct QueueRecorders
    {
        VkCommandBuffer gA      = VK_NULL_HANDLE;
        VkCommandBuffer compute = VK_NULL_HANDLE;
        VkCommandBuffer gB      = VK_NULL_HANDLE;
    };
}
